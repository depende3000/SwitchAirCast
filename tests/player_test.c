/*
 * player_test - runs SwitchAirCast's media player against a URL without the AirPlay layer
 * and prints its state over time, consuming frames like the main loop does.
 *
 *   SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy player_test <url> [seek_at seek_to] [pause_at]
 *
 * PLAYER_SAFARI=1 or 2 replays what Safari on a Mac sends around the start of playback:
 *   1: play while loading; once playing: pause (+0.4 s), play (+1.0 s), seek to 0 (+1.07 s)
 *   2: play while loading, pause just before playback starts, play right after it starts
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>
#include <libavutil/avutil.h>

#include "audio.h"
#include "platform.h"
#include "player.h"
#include "video.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <url> [seek_at seek_to] [pause_at]\n", argv[0]);
        return 2;
    }
    double seek_at = argc > 3 ? atof(argv[2]) : -1, seek_to = argc > 3 ? atof(argv[3]) : 0;
    double pause_at = argc > 4 ? atof(argv[4]) : -1;
    platform_init();
    SDL_Init(SDL_INIT_AUDIO);
    video_init(true);
    audio_init();

    player_options_t opts = { .audio = true, .max_height = 1080 };
    player_start(player_request_new(argv[1], 0, ""), &opts);
    double start = now_s(), next_print = 0;
    unsigned long shown = 0;
    int64_t last_pts = AV_NOPTS_VALUE;
    bool seeked = false, paused = false;
    int safari = getenv("PLAYER_SAFARI") ? atoi(getenv("PLAYER_SAFARI")) : 0;
    int safari_step = 0;
    double playing_at = -1;
    unsigned long shown_at_script_end = 0;
    for (;;) {
        double t = now_s() - start;
        if (safari) {
            player_status_t s0;
            player_get_status(&s0);
            if (playing_at < 0 && s0.state == PLAYER_PLAYING) playing_at = t;
            double since = playing_at < 0 ? -1 : t - playing_at;
            if (safari_step == 0) {
                printf("--- safari: play (loading)\n");
                player_set_paused(false);
                safari_step++;
            } else if (safari == 2 && safari_step == 1 && s0.state == PLAYER_LOADING) {
                printf("--- safari: pause (loading)\n");
                player_set_paused(true);
                safari_step++;
            } else if (safari == 2 && safari_step == 2 && since >= 0.05) {
                printf("--- safari: play\n");
                player_set_paused(false);
                safari_step = 9;
                shown_at_script_end = shown;
            } else if (safari == 1 && safari_step == 1 && since >= 0.4) {
                printf("--- safari: pause\n");
                player_set_paused(true);
                safari_step++;
            } else if (safari == 1 && safari_step == 2 && since >= 1.0) {
                printf("--- safari: play\n");
                player_set_paused(false);
                safari_step++;
            } else if (safari == 1 && safari_step == 3 && since >= 1.07) {
                printf("--- safari: seek 0\n");
                player_seek(0);
                safari_step = 9;
                shown_at_script_end = shown;
            } else if (safari_step == 9 && since >= 6.0) {
                printf("frames shown in the 5 s after Safari's requests: %lu\n", shown - shown_at_script_end);
                break;
            }
        }
        if (!seeked && seek_at >= 0 && t >= seek_at) {
            printf("--- seek to %.1f\n", seek_to);
            player_seek(seek_to);
            seeked = true;
        }
        if (!paused && pause_at >= 0 && t >= pause_at) {
            printf("--- pause 2 s\n");
            player_set_paused(true);
            paused = true;
        }
        if (paused && pause_at >= 0 && t >= pause_at + 2.0 && t < pause_at + 2.1) {
            printf("--- resume\n");
            player_set_paused(false);
        }
        int64_t clock = player_clock_us();
        AVFrame *f = video_take_due_frame(clock);
        if (f) {
            shown++;
            last_pts = f->pts;
            av_frame_free(&f);
        }
        player_status_t st;
        player_get_status(&st);
        if (t >= next_print) {
            next_print = t + 0.5;
            printf("t=%5.2f state=%d pos=%6.2f dur=%5.1f paused=%d buf=%d clock=%7.2f frame=%7.2f shown=%lu "
                   "audio_q=%4d ms vpkts=%3d vframes=%d\n",
                   t, st.state, st.position, st.duration, st.paused, st.buffering,
                   clock == AV_NOPTS_VALUE ? -1.0 : clock / 1e6, last_pts == AV_NOPTS_VALUE ? -1.0 : last_pts / 1e6,
                   shown, audio_queued_ms(), video_queued_packets(), video_queued_frames());
            fflush(stdout);
        }
        if (st.state == PLAYER_ENDED) {
            printf("ended at t=%.2f (failed=%d) after %lu frames, last frame %.2f s\n", t, st.failed, shown,
                   last_pts / 1e6);
            break;
        }
        if (t > 120) break;
        usleep(16666);
    }
    player_stop();
    audio_shutdown();
    video_shutdown();
    SDL_Quit();
    platform_exit();
    return 0;
}
