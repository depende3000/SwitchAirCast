/*
 * Runs the DLNA renderer with a simulated player, for tests/dlna_client.py (or a real
 * control point such as BubbleUPnP) to talk to.
 *
 *   build/linux/dlna_test [seconds]     stays up that long (default 60)
 *
 * Media whose URL contains "fail" fails to open; otherwise each item "plays" for 3 s
 * (URLs containing "long": 600 s), so the end of a track and the queued next one happen
 * quickly. Every callback is printed on stdout.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dlna.h"
#include "platform.h"

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static char url[2048];
static double started_at, base_pos, duration;
static bool active, paused, failed, ended;
static double paused_pos;
static int volume = 70;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

static double position_locked(void) {
    if (paused) return paused_pos;
    double p = base_pos + now_s() - started_at;
    return p > duration ? duration : p;
}

static void play(const dlna_media_t *m) {
    pthread_mutex_lock(&mutex);
    snprintf(url, sizeof(url), "%s", m->url);
    started_at = now_s() + 0.3;   /* "loading" */
    base_pos = m->start;
    duration = strstr(url, "long") ? 600.0 : 3.0;
    active = true;
    paused = false;
    failed = strstr(url, "fail") != NULL;
    ended = false;
    pthread_mutex_unlock(&mutex);
    printf("EVENT play url=%s start=%.1f title=%s artist=%s album=%s art=%s client=%s\n", m->url, m->start, m->title,
           m->artist, m->album, m->art_url, m->client);
    fflush(stdout);
}

static void stop(void) {
    pthread_mutex_lock(&mutex);
    active = false;
    ended = false;
    pthread_mutex_unlock(&mutex);
    printf("EVENT stop\n");
    fflush(stdout);
}

static void set_paused(bool p) {
    pthread_mutex_lock(&mutex);
    if (active && p != paused) {
        if (p) {
            paused_pos = position_locked();
        } else {
            base_pos = paused_pos;
            started_at = now_s();
        }
        paused = p;
    }
    pthread_mutex_unlock(&mutex);
    printf("EVENT paused=%d\n", p);
    fflush(stdout);
}

static void seek(double s) {
    pthread_mutex_lock(&mutex);
    base_pos = paused_pos = s;
    started_at = now_s();
    pthread_mutex_unlock(&mutex);
    printf("EVENT seek %.1f\n", s);
    fflush(stdout);
}

static void get_playback(dlna_playback_t *pb) {
    pthread_mutex_lock(&mutex);
    memset(pb, 0, sizeof(*pb));
    if (active && failed && now_s() >= started_at) {
        active = false;
        pb->failed = true;
    }
    if (active && !paused && now_s() >= started_at && position_locked() >= duration) {
        active = false;
        ended = true;
    }
    if (!active) {
        pb->state = DLNA_STOPPED;
        pb->ended = ended;
        pb->failed = pb->failed || failed;
    } else if (now_s() < started_at) {
        pb->state = DLNA_TRANSITIONING;
    } else {
        pb->state = paused ? DLNA_PAUSED : DLNA_PLAYING;
        pb->position = position_locked();
        pb->duration = duration;
    }
    pthread_mutex_unlock(&mutex);
}

static int get_volume(void) {
    return volume;
}

static void set_volume(int v) {
    volume = v;
    printf("EVENT volume %d\n", v);
    fflush(stdout);
}

static void set_coverart(const void *data, int len) {
    printf("EVENT coverart %d bytes\n", len);
    fflush(stdout);
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 60;
    platform_init();
    dlna_callbacks_t cbs = { play, stop, set_paused, seek, get_playback, get_volume, set_volume, set_coverart };
    if (!dlna_start("DLNA Test Switch", "02:11:22:33:44:55", 0, &cbs)) {
        fprintf(stderr, "dlna_start failed\n");
        return 1;
    }
    printf("PORT %d\n", dlna_port());
    fflush(stdout);
    sleep((unsigned) seconds);
    dlna_stop();
    platform_exit();
    return 0;
}
