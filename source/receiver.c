/*
 * SwitchAirCast - AirPlay receiver
 *
 * The protocol work (mDNS advertising, pairing, FairPlay, RTSP, mirroring and
 * audio streams) is done by UxPlay's library in lib/airplay. This file plays the
 * role of uxplay.cpp: it implements the library callbacks and drives resets.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "receiver.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "audio.h"
#include "dnssd.h"
#include "i18n.h"
#include "logger.h"
#include "platform.h"
#include "player.h"
#include "raop.h"
#include "stream.h"
#include "video.h"

/* drop a client that has not sent its 2-second feedback heartbeat for this long */
#define MISSED_FEEDBACK_LIMIT 15

static raop_t *raop = NULL;
static dnssd_t *dnssd = NULL;
static unsigned short raop_port = 0;
static config_t config;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static receiver_status_t status;
static int open_connections = 0;
static bool mirror_running = false;
static int missed_feedback = 0;
/* only mirroring/audio sessions send heartbeats; AirPlay video sessions never do */
static bool feedback_seen = false;
static time_t last_watchdog = 0;
static uint64_t progress_stamp_ms = 0;
static uint8_t *coverart = NULL;
static int coverart_len = 0;

/* reset requests from library threads, handled on the main thread */
static bool reset_pending = false;
static bool reset_httpd = false;
static bool reset_remove_connections = false;

/* AirPlay video (HLS casting): requests from library threads, run on the main thread */
static player_request_t *pending_play = NULL;
static bool pending_video_stop = false;
static uint64_t video_ended_ms = 0;
static char hls_languages[64];
/* two buffers: a new profile never overwrites the one a library thread may be reading */
static char hls_profile_buf[2][64];
static const char *hls_profile = "";

static void request_video_stop(void) {
    pthread_mutex_lock(&mutex);
    player_request_free(pending_play);
    pending_play = NULL;
    pending_video_stop = true;
    pthread_mutex_unlock(&mutex);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static void request_reset(bool httpd, bool remove_connections) {
    pthread_mutex_lock(&mutex);
    reset_pending = true;
    reset_httpd |= httpd;
    reset_remove_connections |= remove_connections;
    pthread_mutex_unlock(&mutex);
}

static void clear_now_playing_locked(void) {
    status.title[0] = status.artist[0] = status.album[0] = '\0';
    status.has_progress = false;
    status.position = status.duration = 0.0;
    free(coverart);
    coverart = NULL;
    coverart_len = 0;
    status.coverart_serial++;
}

/* ---- connection callbacks ---- */

static void conn_init(void *cls) {
    pthread_mutex_lock(&mutex);
    open_connections++;
    missed_feedback = 0;
    pthread_mutex_unlock(&mutex);
}

static void conn_destroy(void *cls) {
    bool last = false;
    pthread_mutex_lock(&mutex);
    if (open_connections > 0) open_connections--;
    last = (open_connections == 0);
    if (last) {
        feedback_seen = false;
        status.client_name[0] = '\0';
        status.client_model[0] = '\0';
        mirror_running = false;
        clear_now_playing_locked();
    }
    pthread_mutex_unlock(&mutex);
    if (last) {
        request_video_stop();
        audio_stop();
        video_flush();
    }
}

static void conn_reset(void *cls, int reason) {
    platform_log("receiver: connection reset (reason %d)", reason);
    request_reset(true, true);
}

static void conn_feedback(void *cls) {
    pthread_mutex_lock(&mutex);
    missed_feedback = 0;
    feedback_seen = true;
    pthread_mutex_unlock(&mutex);
}

static void report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit) {
    platform_log("receiver: connection request from %s (%s), device id %s", name, model, deviceid);
    pthread_mutex_lock(&mutex);
    snprintf(status.client_name, sizeof(status.client_name), "%s", name ? name : "");
    snprintf(status.client_model, sizeof(status.client_model), "%s", model ? model : "");
    pthread_mutex_unlock(&mutex);
    *admit = true;
}

/* ---- video callbacks ---- */

static void video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    /* the library sets the first byte to 1 when decryption produced invalid NAL units */
    if (data->data_len < 5 || data->data[0] != 0) return;
    video_decode(data->data, data->data_len);
}

static void video_flush_cb(void *cls) {
    video_flush();
}

static void video_pause(void *cls) {
}

static void video_resume(void *cls) {
}

static void video_reset_cb(void *cls, reset_type_t type) {
    platform_log("receiver: video reset (type %d)", (int) type);
    switch (type) {
    case RESET_TYPE_NOHOLD:
        /* a new device took over; the library already dropped the old connections */
        request_video_stop();
        if (config.video_casting) raop_destroy_airplay_video(raop, -1);
        audio_stop();
        video_flush();
        break;
    case RESET_TYPE_HLS_SHUTDOWN:
    case RESET_TYPE_HLS_EOS:
        /* AirPlay video finished or was stopped (as UxPlay handles it) */
        request_video_stop();
        raop_destroy_airplay_video(raop, -1);
        if (type == RESET_TYPE_HLS_SHUTDOWN) raop_remove_hls_connections(raop);
        break;
    case RESET_TYPE_RTP_SHUTDOWN:
    case RESET_TYPE_RTP_TO_HLS_TEARDOWN:
        /* mirroring stopped on the device */
        pthread_mutex_lock(&mutex);
        mirror_running = false;
        pthread_mutex_unlock(&mutex);
        video_flush();
        request_reset(false, type == RESET_TYPE_RTP_SHUTDOWN);
        break;
    default:
        video_flush();
        break;
    }
}

static int video_set_codec_cb(void *cls, video_codec_t codec) {
    /* screen mirroring replaces any AirPlay video that is playing */
    request_video_stop();
    return video_set_codec(codec == VIDEO_CODEC_H265);
}

static void video_report_size(void *cls, float *width_source, float *height_source, float *width, float *height) {
    platform_log("receiver: video size %.0fx%.0f (source %.0fx%.0f)", *width, *height, *width_source, *height_source);
}

static void mirror_video_running(void *cls, bool is_running) {
    pthread_mutex_lock(&mutex);
    mirror_running = is_running;
    pthread_mutex_unlock(&mutex);
}

/* ---- audio callbacks ---- */

static void audio_get_format(void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen,
                             bool *isMedia, uint64_t *audioFormat) {
    platform_log("receiver: audio format ct=%d spf=%d screen=%d media=%d format=0x%llx", *ct, *spf,
                 *usingScreen, *isMedia, (unsigned long long) *audioFormat);
    if (config.audio) audio_start(*ct, *spf);
}

static void audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    if (config.audio) audio_push(data->data, data->data_len, data->ct);
}

static void audio_flush_cb(void *cls) {
    audio_flush();
}

static double audio_set_client_volume(void *cls) {
    return (double) audio_get_volume();
}

static void audio_set_volume_cb(void *cls, float volume) {
    audio_set_volume(volume);
}

static void copy_dmap_string(char *dst, size_t dst_len, const unsigned char *src, int len) {
    size_t n = (size_t) len < dst_len - 1 ? (size_t) len : dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

/* DMAP "mlit" listing item: a sequence of [4-byte tag][4-byte length][value] fields */
static void audio_set_metadata(void *cls, const void *buffer, int buflen) {
    const unsigned char *p = buffer;
    if (buflen < 8 || memcmp(p, "mlit", 4) != 0) return;
    int len = (int) be32(p + 4);
    p += 8;
    buflen -= 8;
    if (len < buflen) buflen = len;
    pthread_mutex_lock(&mutex);
    status.title[0] = status.artist[0] = status.album[0] = '\0';
    while (buflen >= 8) {
        int item_len = (int) be32(p + 4);
        if (item_len < 0 || item_len > buflen - 8) break;
        const unsigned char *value = p + 8;
        if (!memcmp(p, "minm", 4)) {
            copy_dmap_string(status.title, sizeof(status.title), value, item_len);
        } else if (!memcmp(p, "asar", 4)) {
            copy_dmap_string(status.artist, sizeof(status.artist), value, item_len);
        } else if (!memcmp(p, "asal", 4)) {
            copy_dmap_string(status.album, sizeof(status.album), value, item_len);
        }
        p += 8 + item_len;
        buflen -= 8 + item_len;
    }
    pthread_mutex_unlock(&mutex);
}

static void audio_set_coverart(void *cls, const void *buffer, int buflen) {
    pthread_mutex_lock(&mutex);
    free(coverart);
    coverart = NULL;
    coverart_len = 0;
    if (buffer && buflen > 0) {
        coverart = malloc((size_t) buflen);
        if (coverart) {
            memcpy(coverart, buffer, (size_t) buflen);
            coverart_len = buflen;
        }
    }
    status.coverart_serial++;
    pthread_mutex_unlock(&mutex);
}

static void audio_stop_coverart_rendering(void *cls) {
    pthread_mutex_lock(&mutex);
    clear_now_playing_locked();
    pthread_mutex_unlock(&mutex);
}

static void audio_set_progress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end) {
    /* RTP timestamps at 44.1 kHz */
    pthread_mutex_lock(&mutex);
    status.position = (double) (uint32_t) (*curr - *start) / 44100.0;
    status.duration = (double) (uint32_t) (*end - *start) / 44100.0;
    status.has_progress = status.duration > 0.0;
    progress_stamp_ms = now_ms();
    pthread_mutex_unlock(&mutex);
}

static void audio_remote_control_id(void *cls, const char *dacp_id, const char *active_remote_header) {
}

/* ---- pairing / access control ----
 * iPhones and iPads connect without a code. A Mac asks for a one-time code before it
 * sends video (pair-pin-start): the code is shown on screen until it is used. */

/* hidden sooner when the Mac finishes or fails pairing, or a button is pressed */
#define PIN_SHOW_MS 60000UL

static uint64_t pin_shown_ms = 0;

static void clear_pin(void);

/* pin "" means the pairing finished or failed: stop showing the code */
static void display_pin(void *cls, char *pin) {
    if (!pin || !pin[0]) {
        clear_pin();
        return;
    }
    pthread_mutex_lock(&mutex);
    bool same = !strcmp(status.pin, pin);
    snprintf(status.pin, sizeof(status.pin), "%s", pin);
    pin_shown_ms = now_ms();
    pthread_mutex_unlock(&mutex);
    if (!same) platform_log("receiver: showing AirPlay code %s for pairing", pin);
}

static void clear_pin(void) {
    pthread_mutex_lock(&mutex);
    status.pin[0] = '\0';
    pin_shown_ms = 0;
    pthread_mutex_unlock(&mutex);
}

void receiver_dismiss_pin(void) {
    clear_pin();
}

static void register_client(void *cls, const char *device_id, const char *pk_str, const char *name) {
    platform_log("receiver: paired with %s (%s)", name ? name : "?", device_id ? device_id : "?");
    clear_pin();
}

static bool check_register(void *cls, const char *pk_str) {
    return true;
}

static const char *passwd(void *cls, int *len) {
    *len = 0;
    return NULL;
}

static void export_dacp(void *cls, const char *active_remote, const char *dacp_id) {
}

/* ---- AirPlay video (the AirPlay button in YouTube and similar apps) ---- */

static void add_playlist_cb(void *cls, const char *path, const char *data, size_t len) {
    player_request_add_playlist((player_request_t *) cls, path, data, len);
}

/* Called on the HTTP server thread once the playback URL (and, for HLS, every
 * playlist fetched from the phone) is ready. */
static void on_video_play(void *cls, const char *location, const float start_position) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "http://localhost:%u", (unsigned) raop_get_port(raop));
    platform_log("receiver: AirPlay video %s (start at %.1f s)", location, start_position);
    player_request_t *req = player_request_new(location, start_position, prefix);
    if (!req) return;
    if (!strncmp(location, prefix, strlen(prefix))) {
        int n = raop_hls_export_playlists(raop, add_playlist_cb, req);
        platform_log("receiver: %d playlists for the player", n);
    }
    pthread_mutex_lock(&mutex);
    player_request_free(pending_play);
    pending_play = req;
    pending_video_stop = false;
    pthread_mutex_unlock(&mutex);
}

static void on_video_scrub(void *cls, const float position) {
    player_seek(position);
}

static void on_video_rate(void *cls, const float rate) {
    if (rate == 0.0f) {
        player_set_paused(true);
    } else if (rate == 1.0f) {
        player_set_paused(false);
    }
}

static void on_video_stop(void *cls) {
    request_video_stop();
}

static void on_video_acquire_playback_info(void *cls, playback_info_t *info) {
    player_status_t st;
    player_get_status(&st);
    pthread_mutex_lock(&mutex);
    bool starting = pending_play != NULL;
    pthread_mutex_unlock(&mutex);

    info->rate = st.paused ? 0.0f : 1.0f;
    info->seek_start = 0.0;
    info->seek_duration = st.duration;
    info->ready_to_play = st.state == PLAYER_PLAYING;
    info->playback_buffer_empty = st.buffering;
    info->playback_buffer_full = st.buffered > 1.0;
    info->playback_likely_to_keep_up = true;
    if (!starting && st.state == PLAYER_PLAYING) {
        info->duration = st.duration;
        info->position = st.position;
    } else if (!starting && st.state == PLAYER_ENDED) {
        /* tells the library (and the phone) that playback is over */
        info->duration = -1.0;
        info->position = -1.0;
    } else {
        /* not ready yet: the library answers without playback information */
        info->duration = 0.0;
        info->position = -1.0;
    }
}

static float on_video_playlist_remove(void *cls) {
    player_status_t st;
    player_set_paused(true);
    player_get_status(&st);
    return (float) st.position;
}

/* Trims the master playlist before the library fetches media playlists from the
 * phone: H.264 only (decoded by NVDEC), no taller than the configured height. */
static void get_custom_profile(void *cls, const char **custom_profile) {
    *custom_profile = hls_profile;
}

static void set_hls_profile(int height) {
    char *buf = hls_profile_buf[hls_profile == hls_profile_buf[0] ? 1 : 0];
    snprintf(buf, sizeof(hls_profile_buf[0]), "AVC:%d:HEVC:0:VP9:0:AV1:0", height);
    hls_profile = buf;
}

static void log_callback(void *cls, int level, const char *msg) {
    platform_log("%s", msg);
}

/* ---- device id ---- */

static bool parse_hw_addr(const char *str, char hw_addr[6]) {
    unsigned int b[6];
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) hw_addr[i] = (char) b[i];
    return true;
}

/* A random, locally administered MAC-style id, kept on the SD card so that the
 * sender sees the same receiver (and the same pairing key) every time. */
static void load_device_id(char *mac, size_t len, char hw_addr[6]) {
    char path[600];
    snprintf(path, sizeof(path), "%s/device_id", platform_data_dir());
    FILE *f = fopen(path, "r");
    if (f) {
        char line[64] = { 0 };
        bool ok = fgets(line, sizeof(line), f) && parse_hw_addr(line, hw_addr);
        fclose(f);
        if (ok) {
            snprintf(mac, len, "%02x:%02x:%02x:%02x:%02x:%02x", (unsigned char) hw_addr[0],
                     (unsigned char) hw_addr[1], (unsigned char) hw_addr[2], (unsigned char) hw_addr[3],
                     (unsigned char) hw_addr[4], (unsigned char) hw_addr[5]);
            return;
        }
    }
    unsigned char r[6];
    platform_random(r, sizeof(r));
    r[0] = (unsigned char) ((r[0] & ~0x01) | 0x02);
    for (int i = 0; i < 6; i++) hw_addr[i] = (char) r[i];
    snprintf(mac, len, "%02x:%02x:%02x:%02x:%02x:%02x", r[0], r[1], r[2], r[3], r[4], r[5]);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", mac);
        fclose(f);
    }
}

/* ---- lifecycle ---- */

bool receiver_start(const config_t *cfg, const char *name, char *err, size_t errlen) {
    config = *cfg;
    memset(&status, 0, sizeof(status));

    char mac[32];
    char hw_addr[6];
    load_device_id(mac, sizeof(mac), hw_addr);
    platform_log("receiver: name \"%s\", device id %s", name, mac);

    int error = 0;
    dnssd = dnssd_init(name, (int) strlen(name), hw_addr, 6, 0, &error);
    if (!dnssd || error) {
        snprintf(err, errlen, tr(S_ERR_DISCOVERY), error);
        return false;
    }
    /* UxPlay's default feature set, minus what SwitchAirCast does not implement */
    dnssd_set_airplay_features(dnssd, 0, cfg->video_casting ? 1 : 0);  /* AirPlay video (URL) playback */
    dnssd_set_airplay_features(dnssd, 4, cfg->video_casting ? 1 : 0);  /* HLS */
    /* the receiver can fetch media the sender serves itself; without it macOS reports
     * "CannotFetchMediaFromSender" and QuickTime refuses to send local files */
    dnssd_set_airplay_features(dnssd, 35, cfg->video_casting ? 1 : 0);
    dnssd_set_airplay_features(dnssd, 42, 0);  /* H.265 screen mirroring */
    /* bit 27 (legacy pairing) stays on: without it a Mac asks for a code (pair-pin-start)
     * but then gives up without showing the box to type it in */

    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.conn_init = conn_init;
    cbs.conn_destroy = conn_destroy;
    cbs.conn_reset = conn_reset;
    cbs.conn_feedback = conn_feedback;
    cbs.audio_process = audio_process;
    cbs.video_process = video_process;
    cbs.audio_flush = audio_flush_cb;
    cbs.video_flush = video_flush_cb;
    cbs.video_pause = video_pause;
    cbs.video_resume = video_resume;
    cbs.audio_set_client_volume = audio_set_client_volume;
    cbs.audio_set_volume = audio_set_volume_cb;
    cbs.audio_get_format = audio_get_format;
    cbs.video_report_size = video_report_size;
    cbs.audio_set_metadata = audio_set_metadata;
    cbs.audio_set_coverart = audio_set_coverart;
    cbs.audio_stop_coverart_rendering = audio_stop_coverart_rendering;
    cbs.audio_set_progress = audio_set_progress;
    cbs.audio_remote_control_id = audio_remote_control_id;
    cbs.report_client_request = report_client_request;
    cbs.display_pin = display_pin;
    cbs.register_client = register_client;
    cbs.check_register = check_register;
    cbs.passwd = passwd;
    cbs.export_dacp = export_dacp;
    cbs.video_reset = video_reset_cb;
    cbs.video_set_codec = video_set_codec_cb;
    cbs.mirror_video_running = mirror_video_running;
    cbs.on_video_play = on_video_play;
    cbs.on_video_scrub = on_video_scrub;
    cbs.on_video_rate = on_video_rate;
    cbs.on_video_stop = on_video_stop;
    cbs.on_video_acquire_playback_info = on_video_acquire_playback_info;
    cbs.on_video_playlist_remove = on_video_playlist_remove;
    cbs.get_custom_profile = get_custom_profile;

    raop = raop_init(&cbs);
    if (!raop) {
        snprintf(err, errlen, "%s", tr(S_ERR_SERVER));
        goto fail;
    }
    raop_set_log_callback(raop, log_callback, NULL);
    raop_set_log_level(raop, cfg->debug_log ? LOGGER_DEBUG : LOGGER_INFO);
    if (raop_init2(raop, 1 /* nohold: a new device may take over */, mac, "")) {
        free(raop);
        raop = NULL;
        snprintf(err, errlen, "%s", tr(S_ERR_SERVER));
        goto fail;
    }
    raop_set_plist(raop, "width", cfg->width);
    raop_set_plist(raop, "height", cfg->height);
    raop_set_plist(raop, "refreshRate", 60);
    raop_set_plist(raop, "maxFPS", cfg->max_fps);
    /* lets a Mac that paired with a code reconnect with pair-verify alone; the code is
     * random (0) and only ever asked for by Macs, the advertisement still says "no password" */
    raop_set_plist(raop, "pin", 0);
    if (cfg->video_casting) {
        raop_set_plist(raop, "hls", 1);
        /* the library requires a language list when HLS is on; a code no stream uses
         * ("zxx", no linguistic content) makes it fall back to the default track */
        if (!strcasecmp(cfg->video_language, "system")) {
            platform_get_languages(hls_languages, sizeof(hls_languages));
        } else if (!strcasecmp(cfg->video_language, "default")) {
            snprintf(hls_languages, sizeof(hls_languages), "zxx");
        } else {
            snprintf(hls_languages, sizeof(hls_languages), "%s", cfg->video_language);
        }
        raop_set_lang(raop, NULL, NULL, hls_languages);
        set_hls_profile(cfg->height);
        platform_log("receiver: AirPlay video enabled (languages %s, profile %s)", hls_languages, hls_profile);
    }

    unsigned short tcp[2] = { 0, (unsigned short) cfg->port };
    unsigned short udp[3] = { 0, 0, 0 };
    raop_set_tcp_ports(raop, tcp);
    raop_set_udp_ports(raop, udp);

    raop_port = raop_get_port(raop);
    if (raop_start_httpd(raop, &raop_port) < 0) {
        platform_log("receiver: port %d unavailable, using a dynamic port", cfg->port);
        raop_port = 0;
        if (raop_start_httpd(raop, &raop_port) < 0) {
            snprintf(err, errlen, "%s", tr(S_ERR_PORT));
            goto fail;
        }
    }
    raop_set_port(raop, raop_port);
    raop_set_dnssd(raop, dnssd);

    if ((error = dnssd_register_raop(dnssd, raop_port)) != 0 ||
        (error = dnssd_register_airplay(dnssd, raop_port)) != 0) {
        snprintf(err, errlen, tr(S_ERR_ADVERTISE), error);
        goto fail;
    }
    platform_log("receiver: AirPlay on port %d, features 0x%llX", raop_port,
                 (unsigned long long) dnssd_get_airplay_features(dnssd));
    last_watchdog = time(NULL);
    return true;

fail:
    receiver_stop();
    return false;
}

void receiver_set_video_size(int width, int height, int max_fps) {
    config.width = width;
    config.height = height;
    config.max_fps = max_fps;
    if (!raop) return;
    /* the library reads these for every /info request, so the next device gets them */
    raop_set_plist(raop, "width", width);
    raop_set_plist(raop, "height", height);
    raop_set_plist(raop, "maxFPS", max_fps);
    if (config.video_casting) set_hls_profile(height);
    platform_log("receiver: next connections get %dx%d at %d fps", width, height, max_fps);
}

void receiver_stop(void) {
    player_stop();
    pthread_mutex_lock(&mutex);
    player_request_free(pending_play);
    pending_play = NULL;
    pthread_mutex_unlock(&mutex);
    if (dnssd) {
        dnssd_unregister_raop(dnssd);
        dnssd_unregister_airplay(dnssd);
    }
    if (raop) {
        raop_destroy(raop);
        raop = NULL;
    }
    if (dnssd) {
        dnssd_destroy(dnssd);
        dnssd = NULL;
    }
    pthread_mutex_lock(&mutex);
    free(coverart);
    coverart = NULL;
    coverart_len = 0;
    pthread_mutex_unlock(&mutex);
}

static bool video_playing(void) {
    player_status_t st;
    player_get_status(&st);
    return st.state == PLAYER_LOADING || st.state == PLAYER_PLAYING;
}

/* Starts and stops the AirPlay video player as the library threads requested. */
static void poll_video(void) {
    pthread_mutex_lock(&mutex);
    player_request_t *play = pending_play;
    bool stop = pending_video_stop;
    pending_play = NULL;
    pending_video_stop = false;
    pthread_mutex_unlock(&mutex);

    if (stop) {
        player_stop();
        player_reset();
        video_ended_ms = 0;
    }
    if (play) {
        player_options_t opts = { .audio = config.audio, .max_height = config.height };
        player_start(play, &opts);
        video_ended_ms = 0;
    }

    /* the phone learns about the end from its next playback-info request; if it has
     * gone away, return to the idle screen after a while */
    player_status_t st;
    player_get_status(&st);
    if (st.state == PLAYER_ENDED) {
        if (!video_ended_ms) video_ended_ms = now_ms();
        else if (now_ms() - video_ended_ms > 5000) {
            player_stop();
            player_reset();
            video_ended_ms = 0;
        }
    } else {
        video_ended_ms = 0;
    }
}

void receiver_stop_video(void) {
    /* the phone sees the end on its next playback-info request */
    player_stop();
}

void receiver_poll(void) {
    if (!raop) return;

    poll_video();

    /* mirroring/audio clients send a feedback request every two seconds; the watchdog
     * only runs once a session has sent one (AirPlay video sessions never do) */
    time_t now = time(NULL);
    if (now != last_watchdog) {
        last_watchdog = now;
        bool video = video_playing();
        pthread_mutex_lock(&mutex);
        bool lost = false;
        if (open_connections > 0 && feedback_seen && !video) {
            if (++missed_feedback > MISSED_FEEDBACK_LIMIT) {
                missed_feedback = 0;
                lost = true;
            }
        } else {
            missed_feedback = 0;
        }
        pthread_mutex_unlock(&mutex);
        if (lost) {
            platform_log("receiver: no feedback from client for %d s, resetting", MISSED_FEEDBACK_LIMIT);
            request_reset(true, true);
        }
    }

    pthread_mutex_lock(&mutex);
    bool pending = reset_pending, httpd = reset_httpd, remove = reset_remove_connections;
    reset_pending = reset_httpd = reset_remove_connections = false;
    pthread_mutex_unlock(&mutex);
    if (!pending) return;

    platform_log("receiver: reset (httpd %d, drop connections %d)", httpd, remove);
    if (httpd) raop_stop_httpd(raop);
    if (httpd || !video_playing()) {
        /* a mirroring -> AirPlay video handover must not cut off the new video */
        player_stop();
        player_reset();
        audio_stop();
        video_flush();
    }
    if (remove) raop_remove_known_connections(raop);
    if (httpd) {
        unsigned short port = raop_get_port(raop);
        if (raop_start_httpd(raop, &port) >= 0) {
            raop_set_port(raop, port);
        } else {
            platform_log("receiver: could not restart the AirPlay server on port %d", port);
        }
    }
    pthread_mutex_lock(&mutex);
    mirror_running = false;
    pthread_mutex_unlock(&mutex);
}

void receiver_get_status(receiver_status_t *st) {
    bool video = video_playing();
    pthread_mutex_lock(&mutex);
    *st = status;
    if (video || pending_play) {
        st->state = RECEIVER_VIDEO;
    } else if (mirror_running) {
        st->state = RECEIVER_MIRRORING;
    } else if (open_connections > 0) {
        st->state = RECEIVER_CONNECTED;
    } else {
        st->state = RECEIVER_WAITING;
    }
    if (st->has_progress && progress_stamp_ms) {
        st->position += (double) (now_ms() - progress_stamp_ms) / 1000.0;
        if (st->position > st->duration) st->position = st->duration;
    }
    /* the code is no longer needed once the Mac streams, and expires like the library's */
    if (status.pin[0] && (st->state == RECEIVER_VIDEO || now_ms() - pin_shown_ms > PIN_SHOW_MS)) {
        status.pin[0] = '\0';
        pin_shown_ms = 0;
    }
    if (st->state == RECEIVER_VIDEO) st->pin[0] = '\0';
    pthread_mutex_unlock(&mutex);
    st->audio_playing = audio_active();
}

void receiver_disconnect(void) {
    request_reset(false, true);
}

bool receiver_get_coverart(uint8_t **data, int *len) {
    bool ok = false;
    pthread_mutex_lock(&mutex);
    if (coverart && coverart_len > 0) {
        *data = malloc((size_t) coverart_len);
        if (*data) {
            memcpy(*data, coverart, (size_t) coverart_len);
            *len = coverart_len;
            ok = true;
        }
    }
    pthread_mutex_unlock(&mutex);
    return ok;
}
