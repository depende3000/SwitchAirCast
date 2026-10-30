/*
 * SwitchAirCast - media player for AirPlay video
 *
 * The phone sends a playback URL. For YouTube that is an HLS master playlist which
 * UxPlay's library has already fetched from the phone (with every media playlist) and
 * would serve on http://localhost:<port>/...; here those playlists are read from memory
 * through a custom io_open, and only the media segments come from the network (https).
 *
 * One thread demuxes (FFmpeg) and feeds the shared video decoder (NVDEC) and the audio
 * module; the main thread shows each decoded frame when the audio clock reaches it.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "player.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavutil/time.h>

#include "audio.h"
#include "platform.h"
#include "video.h"

/* the receiver advertises itself as an Apple TV; fetch segments like one */
#define USER_AGENT "AppleCoreMedia/1.0.0.21J354 (Apple TV; U; CPU OS 17_0 like Mac OS X; en_us)"
/* give up on a stalled connection after this long (microseconds) */
#define RW_TIMEOUT_US "15000000"
/* read ahead until this much audio and this many video packets are queued */
#define AUDIO_AHEAD_MS 1500
#define VIDEO_AHEAD_PACKETS 90
/* never let video run further ahead than this, even if audio is starved */
#define VIDEO_MAX_PACKETS 400
/* show video on a wall clock if no audio has arrived after this long */
#define AUDIO_CLOCK_WAIT_US 2000000
/* the steady video clock closes a gap with the audio clock over about this long... */
#define STEADY_CLOCK_US 500000
/* ...and jumps to it when they differ by more than this (an audio stall or queue reset) */
#define STEADY_RESYNC_US 100000
#define PLAYER_STACK_SIZE (512 * 1024)

typedef struct {
    char *path;
    char *data;
    size_t len;
    /* what FFmpeg is given: data, or a copy trimmed to start at a seek position */
    char *served;
    size_t served_len;
} playlist_t;

struct player_request_s {
    char *url;
    double start;
    char *local_prefix;
    playlist_t *playlists;
    int n_playlists;
};

typedef struct {
    const char *data;
    size_t len;
    size_t pos;
} mem_reader_t;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static bool thread_started = false;
static volatile int abort_request = 0;

/* set by player_start before the thread starts; read-only while it runs */
static player_request_t *req = NULL;
static player_options_t options;
static int (*default_io_open)(AVFormatContext *s, AVIOContext **pb, const char *url, int flags,
                              AVDictionary **opts) = NULL;
static int (*default_io_close2)(AVFormatContext *s, AVIOContext *pb) = NULL;

/* guarded by mutex */
static player_state_t state = PLAYER_IDLE;
static bool failed = false;
static bool paused = false;
static bool seek_requested = false;
static double seek_target = 0.0;
static double duration = 0.0;
static double last_position = 0.0;
static bool eof = false;
static bool use_audio_clock = false;
static bool has_audio = false;
static bool has_video = false;
static bool completed = false;
static int64_t paused_at = AV_NOPTS_VALUE;
static int64_t wall_base_pts = AV_NOPTS_VALUE;
static int64_t wall_base_time = 0;
static int64_t audio_wait_since = 0;
static int64_t freerun_base = AV_NOPTS_VALUE;
static int64_t freerun_time = 0;
static int64_t steady_base = AV_NOPTS_VALUE;
static int64_t steady_time = 0;

/* ---- requests ---- */

player_request_t *player_request_new(const char *url, double start_seconds, const char *local_prefix) {
    player_request_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->url = strdup(url ? url : "");
    r->local_prefix = strdup(local_prefix ? local_prefix : "");
    r->start = start_seconds > 0 ? start_seconds : 0;
    return r;
}

void player_request_add_playlist(player_request_t *r, const char *path, const char *data, size_t len) {
    playlist_t *grown = realloc(r->playlists, sizeof(playlist_t) * (size_t) (r->n_playlists + 1));
    if (!grown) return;
    r->playlists = grown;
    playlist_t *pl = &r->playlists[r->n_playlists];
    pl->path = strdup(path);
    pl->data = malloc(len + 1);
    if (!pl->path || !pl->data) {
        free(pl->path);
        free(pl->data);
        return;
    }
    memcpy(pl->data, data, len);
    pl->data[len] = '\0';
    pl->len = len;
    pl->served = NULL;
    pl->served_len = 0;
    r->n_playlists++;
}

void player_request_free(player_request_t *r) {
    if (!r) return;
    for (int i = 0; i < r->n_playlists; i++) {
        free(r->playlists[i].path);
        free(r->playlists[i].data);
        free(r->playlists[i].served);
    }
    free(r->playlists);
    free(r->url);
    free(r->local_prefix);
    free(r);
}

static playlist_t *find_playlist(player_request_t *r, const char *path) {
    for (int i = 0; i < r->n_playlists; i++) {
        if (!strcmp(r->playlists[i].path, path)) return &r->playlists[i];
    }
    /* the library matches by substring; do the same as a fallback */
    for (int i = 0; i < r->n_playlists; i++) {
        if (strstr(r->playlists[i].path, path) || strstr(path, r->playlists[i].path)) return &r->playlists[i];
    }
    return NULL;
}

/* ---- HLS master playlist: keep one video variant ---- */

static bool attr_value(const char *line, size_t line_len, const char *key, char *out, size_t out_len) {
    size_t key_len = strlen(key);
    for (const char *p = line; p + key_len <= line + line_len; p++) {
        /* match whole attribute names only (BANDWIDTH= must not match AVERAGE-BANDWIDTH=) */
        if (strncmp(p, key, key_len) || (p > line && p[-1] != ':' && p[-1] != ',')) continue;
        p += key_len;
        size_t n = 0;
        if (*p == '"') {
            p++;
            while (p < line + line_len && *p != '"' && n + 1 < out_len) out[n++] = *p++;
        } else {
            while (p < line + line_len && *p != ',' && n + 1 < out_len) out[n++] = *p++;
        }
        out[n] = '\0';
        return true;
    }
    return false;
}

typedef struct {
    const char *inf;
    size_t inf_len;
    const char *uri;
    size_t uri_len;
    int height;
    double fps;
    long bandwidth;
    bool avc;
    char audio[64];
} variant_t;

static bool better_variant(const variant_t *a, const variant_t *b, int max_height) {
    /* is a better than b? */
    bool a_fits = a->height <= max_height, b_fits = b->height <= max_height;
    if (a_fits != b_fits) return a_fits;
    if (a->avc != b->avc) return a->avc;
    if (!a_fits) return a->height < b->height;  /* nothing fits: smallest */
    if (a->height != b->height) return a->height > b->height;
    if (a->fps != b->fps) return a->fps > b->fps;
    return a->bandwidth > b->bandwidth;
}

static const char *next_line(const char *p, const char **end) {
    const char *nl = strchr(p, '\n');
    *end = nl ? nl : p + strlen(p);
    return nl ? nl + 1 : NULL;
}

/* Returns a reduced copy of master (malloc'd), or NULL if it has no variants. */
static char *pick_variant(const char *master, int max_height) {
    variant_t best;
    bool have_best = false;
    const char *line = master, *end, *next;
    for (; line; line = next) {
        next = next_line(line, &end);
        if (strncmp(line, "#EXT-X-STREAM-INF:", 18) || !next) continue;
        variant_t v;
        memset(&v, 0, sizeof(v));
        v.inf = line;
        v.inf_len = (size_t) (end - line);
        const char *uri_end;
        const char *after = next_line(next, &uri_end);
        v.uri = next;
        v.uri_len = (size_t) (uri_end - next);
        char val[256];
        if (attr_value(v.inf, v.inf_len, "RESOLUTION=", val, sizeof(val))) {
            const char *x = strchr(val, 'x');
            v.height = x ? atoi(x + 1) : 0;
        }
        if (attr_value(v.inf, v.inf_len, "FRAME-RATE=", val, sizeof(val))) v.fps = atof(val);
        if (attr_value(v.inf, v.inf_len, "BANDWIDTH=", val, sizeof(val))) v.bandwidth = atol(val);
        if (attr_value(v.inf, v.inf_len, "CODECS=", val, sizeof(val))) v.avc = strstr(val, "avc1") != NULL;
        attr_value(v.inf, v.inf_len, "AUDIO=", v.audio, sizeof(v.audio));
        if (!have_best || better_variant(&v, &best, max_height)) {
            best = v;
            have_best = true;
        }
        next = after;
    }
    if (!have_best) return NULL;

    size_t len = strlen(master);
    char *out = malloc(len + 2);
    if (!out) return NULL;
    char *o = out;
    for (line = master; line; line = next) {
        next = next_line(line, &end);
        size_t n = (size_t) (end - line);
        bool keep = true;
        if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
            keep = (line == best.inf);
            if (next) {
                const char *uri_end;
                const char *after = next_line(next, &uri_end);
                if (keep) {
                    memcpy(o, line, n);
                    o += n;
                    *o++ = '\n';
                    memcpy(o, next, (size_t) (uri_end - next));
                    o += uri_end - next;
                    *o++ = '\n';
                }
                next = after;
                continue;
            }
        } else if (!strncmp(line, "#EXT-X-I-FRAME-STREAM-INF:", 26)) {
            keep = false;
        } else if (!strncmp(line, "#EXT-X-MEDIA:", 13)) {
            char type[32] = "", group[64] = "";
            attr_value(line, n, "TYPE=", type, sizeof(type));
            attr_value(line, n, "GROUP-ID=", group, sizeof(group));
            if (!strcmp(type, "AUDIO")) keep = !best.audio[0] || !strcmp(group, best.audio);
            else if (!strcmp(type, "SUBTITLES") || !strcmp(type, "CLOSED-CAPTIONS")) keep = false;
        }
        if (keep && n > 0) {
            memcpy(o, line, n);
            o += n;
            *o++ = '\n';
        }
    }
    *o = '\0';
    platform_log("player: using variant %dp%s%.0f %s (%ld bit/s)", best.height, best.fps > 0 ? " @" : "",
                 best.fps, best.avc ? "H.264" : "other codec", best.bandwidth);
    return out;
}

/* ---- HLS media playlists: start at a position ---- */

/* FFmpeg's HLS seek fails on fragmented-MP4 segments (as YouTube uses): the stream
 * ends right after the seek. Opening a copy of each media playlist that starts at the
 * segment containing the target works and keeps absolute timestamps. */

static bool is_segment_state_tag(const char *line) {
    /* tags that stay in effect for the segments after them */
    return !strncmp(line, "#EXT-X-MAP:", 11) || !strncmp(line, "#EXT-X-KEY:", 11);
}

/* Returns a malloc'd copy of pl starting at the segment that contains target seconds,
 * or NULL if pl has no such segment. */
static char *trim_media_playlist(const char *pl, double target) {
    /* room for the copied init-section/key lines on top of the original */
    char *out = malloc(2 * strlen(pl) + 64);
    if (!out) return NULL;
    char *o = out;
    const char *line, *end, *next;
    const char *map_line = NULL, *key_line = NULL;   /* in effect at the current segment */
    size_t map_len = 0, key_len = 0;
    const char *block = NULL;                         /* first line of the current segment */
    double t = 0.0, seg_duration = 0.0;
    bool in_header = true, found = false;

#define EMIT(p, n) do { memcpy(o, (p), (n)); o += (n); *o++ = '\n'; } while (0)
    for (line = pl; line; line = next) {
        next = next_line(line, &end);
        size_t n = (size_t) (end - line);
        if (n && line[n - 1] == '\r') n--;
        if (found) {
            EMIT(line, n);
            continue;
        }
        bool segment_line = !strncmp(line, "#EXTINF:", 8) || (n && line[0] != '#') ||
                            !strncmp(line, "#EXT-X-DISCONTINUITY", 20) || !strncmp(line, "#EXT-X-BYTERANGE", 16) ||
                            is_segment_state_tag(line);
        if (in_header && !segment_line) {
            /* the original start sequence would only confuse; drop it */
            if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) && strncmp(line, "#EXT-X-DISCONTINUITY-SEQUENCE:", 30)) {
                EMIT(line, n);
            }
            continue;
        }
        in_header = false;
        if (!block) block = line;
        if (!strncmp(line, "#EXT-X-MAP:", 11)) {
            map_line = line;
            map_len = n;
        } else if (!strncmp(line, "#EXT-X-KEY:", 11)) {
            key_line = line;
            key_len = n;
        } else if (!strncmp(line, "#EXTINF:", 8)) {
            seg_duration = atof(line + 8);
        } else if (n && line[0] != '#') {
            if (t + seg_duration > target) {
                /* the target segment: the init section and key in effect, then the segment itself */
                if (map_line) EMIT(map_line, map_len);
                if (key_line) EMIT(key_line, key_len);
                for (const char *b = block; b;) {
                    const char *b_end;
                    const char *b_next = next_line(b, &b_end);
                    size_t bn = (size_t) (b_end - b);
                    if (bn && b[bn - 1] == '\r') bn--;
                    if (!is_segment_state_tag(b)) EMIT(b, bn);
                    if (b == line) break;
                    b = b_next;
                }
                found = true;
                continue;
            }
            t += seg_duration;
            seg_duration = 0.0;
            block = NULL;
        }
    }
#undef EMIT
    *o = '\0';
    if (!found) {
        free(out);
        return NULL;
    }
    return out;
}

/* Points every media playlist at the segment containing target (0 = from the start). */
static void serve_playlists_from(player_request_t *r, double target) {
    for (int i = 0; i < r->n_playlists; i++) {
        playlist_t *pl = &r->playlists[i];
        free(pl->served);
        pl->served = NULL;
        pl->served_len = 0;
        if (target <= 0.0 || !strcmp(pl->path, "/master.m3u8")) continue;
        pl->served = trim_media_playlist(pl->data, target);
        if (pl->served) pl->served_len = strlen(pl->served);
    }
}

/* ---- in-memory playlists for FFmpeg ---- */

static int mem_read(void *opaque, uint8_t *buf, int size) {
    mem_reader_t *r = opaque;
    size_t left = r->len - r->pos;
    if (left == 0) return AVERROR_EOF;
    if ((size_t) size > left) size = (int) left;
    memcpy(buf, r->data + r->pos, (size_t) size);
    r->pos += (size_t) size;
    return size;
}

static int64_t mem_seek(void *opaque, int64_t offset, int whence) {
    mem_reader_t *r = opaque;
    if (whence & AVSEEK_SIZE) return (int64_t) r->len;
    whence &= ~AVSEEK_FORCE;
    int64_t pos;
    if (whence == SEEK_SET) pos = offset;
    else if (whence == SEEK_CUR) pos = (int64_t) r->pos + offset;
    else if (whence == SEEK_END) pos = (int64_t) r->len + offset;
    else return AVERROR(EINVAL);
    if (pos < 0 || pos > (int64_t) r->len) return AVERROR(EINVAL);
    r->pos = (size_t) pos;
    return pos;
}

static AVIOContext *mem_open(const char *data, size_t len) {
    mem_reader_t *r = calloc(1, sizeof(*r));
    unsigned char *buf = av_malloc(4096);
    AVIOContext *pb = (r && buf) ? avio_alloc_context(buf, 4096, 0, r, mem_read, NULL, mem_seek) : NULL;
    if (!pb) {
        free(r);
        av_free(buf);
        return NULL;
    }
    r->data = data;
    r->len = len;
    return pb;
}

static void mem_close(AVIOContext **pb) {
    if (!*pb) return;
    free((*pb)->opaque);
    av_freep(&(*pb)->buffer);
    avio_context_free(pb);
}

static const char *local_path(const char *url) {
    size_t n = req ? strlen(req->local_prefix) : 0;
    return (n && !strncmp(url, req->local_prefix, n)) ? url + n : NULL;
}

static int player_io_open(AVFormatContext *s, AVIOContext **pb, const char *url, int flags, AVDictionary **opts) {
    const char *path = local_path(url);
    if (path) {
        playlist_t *pl = find_playlist(req, path);
        if (!pl) {
            platform_log("player: no stored playlist for %s", path);
            return AVERROR(ENOENT);
        }
        *pb = pl->served ? mem_open(pl->served, pl->served_len) : mem_open(pl->data, pl->len);
        return *pb ? 0 : AVERROR(ENOMEM);
    }
    if (opts) {
        av_dict_set(opts, "user_agent", USER_AGENT, AV_DICT_DONT_OVERWRITE);
        av_dict_set(opts, "rw_timeout", RW_TIMEOUT_US, AV_DICT_DONT_OVERWRITE);
    }
    return default_io_open(s, pb, url, flags, opts);
}

static int player_io_close2(AVFormatContext *s, AVIOContext *pb) {
    if (pb && pb->read_packet == mem_read) {
        mem_close(&pb);
        return 0;
    }
    return default_io_close2(s, pb);
}

static int interrupt_cb(void *opaque) {
    (void) opaque;
    return abort_request;
}

/* ---- clock (mutex held) ---- */

static void reset_clock_locked(void) {
    wall_base_pts = AV_NOPTS_VALUE;
    freerun_base = AV_NOPTS_VALUE;
    steady_base = AV_NOPTS_VALUE;
    audio_wait_since = 0;
    use_audio_clock = has_audio;
}

/* SDL takes queued audio one device buffer at a time (1024 samples, 23 ms), so the audio
 * clock stands still and then jumps. Timed by it, 60 fps video had two frames come due in
 * one screen refresh and skipped one of them. Video follows a steady copy instead: it runs
 * on the system timer and is pulled gently toward the audio clock, or jumps to it when the
 * two drift far apart. */
static int64_t steady_clock_locked(int64_t audio, int64_t now) {
    if (steady_base != AV_NOPTS_VALUE) {
        int64_t elapsed = now - steady_time;
        int64_t clock = steady_base + elapsed;
        int64_t error = audio - clock;
        if (elapsed >= 0 && error > -STEADY_RESYNC_US && error < STEADY_RESYNC_US) {
            /* runs at most 20% (resync / smoothing time) fast or slow, so it also follows an
             * audio device that plays a little off its nominal rate */
            steady_base = clock + error * (elapsed < STEADY_CLOCK_US ? elapsed : STEADY_CLOCK_US) / STEADY_CLOCK_US;
            steady_time = now;
            return steady_base;
        }
    }
    steady_base = audio;
    steady_time = now;
    return audio;
}

static int64_t running_clock_locked(void) {
    int64_t now = av_gettime_relative();
    if (use_audio_clock) {
        int64_t a = audio_clock_us();
        if (a != AV_NOPTS_VALUE) {
            audio_wait_since = 0;
            if (eof && audio_queued_ms() == 0) {
                /* the audio track ended before the video: let time keep running */
                if (freerun_base == AV_NOPTS_VALUE) {
                    freerun_base = steady_base != AV_NOPTS_VALUE ? steady_base + (now - steady_time) : a;
                    freerun_time = now;
                }
                return freerun_base + (now - freerun_time);
            }
            return steady_clock_locked(a, now);
        }
        if (video_next_frame_pts() == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
        if (!audio_wait_since) audio_wait_since = now;
        if (now - audio_wait_since < AUDIO_CLOCK_WAIT_US) return AV_NOPTS_VALUE;
        platform_log("player: no audio arriving, timing video by the system clock");
        use_audio_clock = false;
    }
    if (wall_base_pts == AV_NOPTS_VALUE) {
        int64_t first = video_next_frame_pts();
        if (first == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
        wall_base_pts = first;
        wall_base_time = now;
    }
    return wall_base_pts + (now - wall_base_time);
}

/* ---- demux thread ---- */

static int pick_video_stream(AVFormatContext *fmt, int max_height) {
    int best = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters *par = fmt->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO || (fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            continue;
        }
        if (best < 0) {
            best = (int) i;
            continue;
        }
        AVCodecParameters *b = fmt->streams[best]->codecpar;
        bool fits = par->height <= max_height, b_fits = b->height <= max_height;
        bool avc = par->codec_id == AV_CODEC_ID_H264, b_avc = b->codec_id == AV_CODEC_ID_H264;
        if (fits != b_fits ? fits : avc != b_avc ? avc : fits ? par->height > b->height : par->height < b->height) {
            best = (int) i;
        }
    }
    return best;
}

static void finish(bool with_error, bool at_end) {
    pthread_mutex_lock(&mutex);
    state = PLAYER_ENDED;
    failed = with_error;
    completed = at_end;
    pthread_mutex_unlock(&mutex);
}

typedef struct {
    AVFormatContext *fmt;
    AVIOContext *master_pb;   /* our reader for an in-memory master playlist */
    int vidx;
    int aidx;
} input_t;

static void close_input(input_t *in) {
    if (in->fmt) avformat_close_input(&in->fmt);
    mem_close(&in->master_pb);
    in->vidx = in->aidx = -1;
}

/* A remote master playlist (Safari sends YouTube's), reduced to one variant; NULL if the
 * URL is not one. Owned by the player thread. */
static char *remote_master = NULL;

#define MASTER_MAX_BYTES (1024 * 1024)

/* Safari gives the URL of a master playlist listing every quality. Left alone, FFmpeg
 * downloads each quality's playlist and the start of its stream before playing (several
 * seconds over HTTPS on the console). Fetch the master here and keep one variant. */
static char *fetch_reduced_master(const char *url, int max_height) {
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return NULL;
    if (!strstr(url, ".m3u8")) return NULL;
    AVIOContext *pb = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "user_agent", USER_AGENT, 0);
    av_dict_set(&opts, "rw_timeout", RW_TIMEOUT_US, 0);
    AVIOInterruptCB cb = { interrupt_cb, NULL };
    int ret = avio_open2(&pb, url, AVIO_FLAG_READ, &cb, &opts);
    av_dict_free(&opts);
    if (ret < 0) return NULL;
    size_t cap = 64 * 1024, len = 0;
    char *buf = malloc(cap + 1);
    while (buf) {
        if (len == cap) {
            if (cap >= MASTER_MAX_BYTES) break;
            char *grown = realloc(buf, cap * 2 + 1);
            if (!grown) break;
            buf = grown;
            cap *= 2;
        }
        int n = avio_read(pb, (unsigned char *) buf + len, (int) (cap - len));
        if (n <= 0) break;
        len += (size_t) n;
        /* not a playlist: stop reading what may be a whole video file */
        if (len >= 7 && strncmp(buf, "#EXTM3U", 7)) break;
    }
    avio_closep(&pb);
    char *reduced = NULL;
    if (buf) {
        buf[len] = '\0';
        if (len >= 7 && !strncmp(buf, "#EXTM3U", 7) && strstr(buf, "#EXT-X-STREAM-INF:")) {
            reduced = pick_variant(buf, max_height);
        }
        free(buf);
    }
    return reduced;
}

/* Opens req->url (serving in-memory playlists) and picks the streams to play. */
static int open_input(input_t *in, bool quick) {
    char err[128];
    in->fmt = avformat_alloc_context();
    in->master_pb = NULL;
    in->vidx = in->aidx = -1;
    if (!in->fmt) return AVERROR(ENOMEM);
    AVFormatContext *fmt = in->fmt;
    default_io_open = fmt->io_open;
    default_io_close2 = fmt->io_close2;
    fmt->io_open = player_io_open;
    fmt->io_close2 = player_io_close2;
    fmt->interrupt_callback.callback = interrupt_cb;
    fmt->probesize = 2 * 1024 * 1024;
    fmt->max_analyze_duration = (quick ? 1 : 3) * AV_TIME_BASE;

    const char *path = local_path(req->url);
    if (path) {
        /* custom I/O keeps avformat_close_input from treating our reader as a URL context */
        playlist_t *master = find_playlist(req, path);
        if (!master || !(in->master_pb = mem_open(master->data, master->len))) {
            platform_log("player: master playlist %s not available", path);
            close_input(in);
            return AVERROR(ENOENT);
        }
        fmt->pb = in->master_pb;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    } else if (remote_master) {
        /* relative variant URIs still resolve against req->url */
        if (!(in->master_pb = mem_open(remote_master, strlen(remote_master)))) {
            close_input(in);
            return AVERROR(ENOMEM);
        }
        fmt->pb = in->master_pb;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    }

    AVDictionary *open_opts = NULL;
    /* keep-alive would hand our in-memory playlist readers to the HTTP layer */
    av_dict_set(&open_opts, "http_persistent", "0", 0);
    av_dict_set(&open_opts, "user_agent", USER_AGENT, 0);
    av_dict_set(&open_opts, "rw_timeout", RW_TIMEOUT_US, 0);
    int ret = avformat_open_input(&in->fmt, req->url, NULL, &open_opts);
    av_dict_free(&open_opts);
    if (ret < 0) {
        av_strerror(ret, err, sizeof(err));
        platform_log("player: cannot open stream: %s", err);
        in->fmt = NULL;  /* freed by avformat_open_input */
        close_input(in);
        return ret;
    }
    fmt = in->fmt;
    if (abort_request) {
        close_input(in);
        return AVERROR_EXIT;
    }
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        av_strerror(ret, err, sizeof(err));
        platform_log("player: stream info incomplete: %s", err);
    }
    in->vidx = pick_video_stream(fmt, options.max_height);
    in->aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, in->vidx, NULL, 0);
    if (in->aidx < 0) in->aidx = -1;
    if (in->vidx < 0 && in->aidx < 0) {
        platform_log("player: no playable streams");
        close_input(in);
        return AVERROR_STREAM_NOT_FOUND;
    }
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        fmt->streams[i]->discard =
            ((int) i == in->vidx || (int) i == in->aidx) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
    return 0;
}

static void *player_thread(void *arg) {
    (void) arg;
    input_t in = { NULL, NULL, -1, -1 };
    AVPacket *pkt = av_packet_alloc();
    int vsession = -1, asession = -1;
    int64_t start_us = 0;
    bool error = true;
    bool at_end = false;
    char err[128];
    int ret;
    /* HLS from the phone is in memory: seek by reopening from trimmed playlists */
    bool reopen_seek = local_path(req->url) != NULL;

    platform_log("player: opening %s (start at %.1f s)", req->url, req->start);
    if (!pkt) goto done;
    if (!reopen_seek) remote_master = fetch_reduced_master(req->url, options.max_height);
    if (open_input(&in, false) < 0) goto done;

    if (in.vidx >= 0) {
        AVCodecParameters *par = in.fmt->streams[in.vidx]->codecpar;
        platform_log("player: video %s %dx%d", avcodec_get_name(par->codec_id), par->width, par->height);
        vsession = video_open_stream(par);
        if (vsession < 0) goto done;
    }
    if (in.aidx >= 0 && options.audio) {
        asession = audio_open_stream(in.fmt->streams[in.aidx]->codecpar);
    }
    start_us = in.fmt->start_time != AV_NOPTS_VALUE ? in.fmt->start_time : 0;

    pthread_mutex_lock(&mutex);
    duration = in.fmt->duration > 0 ? (double) in.fmt->duration / AV_TIME_BASE : 0.0;
    has_audio = asession >= 0;
    has_video = vsession >= 0;
    reset_clock_locked();
    state = PLAYER_PLAYING;
    if (paused) {
        audio_set_paused(true);
        paused_at = (int64_t) (req->start * 1e6);
    }
    if (req->start > 0.5 && (duration == 0.0 || req->start < duration - 1.0)) {
        seek_requested = true;
        seek_target = req->start;
    }
    pthread_mutex_unlock(&mutex);
    platform_log("player: playing (duration %.1f s, %s)", duration, has_audio ? "with audio" : "no audio");

    while (!abort_request) {
        pthread_mutex_lock(&mutex);
        bool do_seek = seek_requested;
        double target = seek_target;
        bool at_eof = eof;
        seek_requested = false;
        pthread_mutex_unlock(&mutex);

        if (do_seek) {
            int64_t target_us = (int64_t) (target * 1e6);
            if (vsession >= 0) video_flush_to(target_us);
            if (asession >= 0) audio_flush_to(target_us);
            if (reopen_seek) {
                close_input(&in);
                serve_playlists_from(req, target);
                if (open_input(&in, true) < 0) {
                    platform_log("player: could not reopen the stream at %.1f s", target);
                    goto done;
                }
            } else {
                ret = avformat_seek_file(in.fmt, -1, INT64_MIN, target_us + start_us, INT64_MAX, 0);
                if (ret < 0) {
                    av_strerror(ret, err, sizeof(err));
                    platform_log("player: seek to %.1f s failed: %s", target, err);
                }
            }
            pthread_mutex_lock(&mutex);
            eof = false;
            reset_clock_locked();
            last_position = target;
            /* while paused, show the first frame at the new position */
            if (paused) paused_at = target_us + 50000;
            pthread_mutex_unlock(&mutex);
            continue;
        }

        if (at_eof) {
            bool video_done = vsession < 0 || video_stream_finished();
            bool audio_done = asession < 0 || audio_queued_ms() == 0;
            if (video_done && audio_done) {
                platform_log("player: reached the end");
                error = false;
                at_end = true;
                goto done;
            }
            usleep(20000);
            continue;
        }

        int vq = vsession >= 0 ? video_queued_packets() : 0;
        bool audio_enough = asession < 0 || audio_queued_ms() > AUDIO_AHEAD_MS;
        bool video_enough = vsession < 0 || vq > VIDEO_AHEAD_PACKETS;
        if ((audio_enough && video_enough) || vq > VIDEO_MAX_PACKETS) {
            usleep(10000);
            continue;
        }

        ret = av_read_frame(in.fmt, pkt);
        if (ret == AVERROR(EAGAIN)) {
            usleep(5000);
            continue;
        }
        if (ret < 0) {
            if (abort_request) break;
            if (ret != AVERROR_EOF) {
                av_strerror(ret, err, sizeof(err));
                platform_log("player: read error (%s), ending playback", err);
            }
            if (vsession >= 0) video_send_packet(vsession, NULL);
            pthread_mutex_lock(&mutex);
            eof = true;
            pthread_mutex_unlock(&mutex);
            continue;
        }
        if (pkt->stream_index == in.vidx || (pkt->stream_index == in.aidx && asession >= 0)) {
            av_packet_rescale_ts(pkt, in.fmt->streams[pkt->stream_index]->time_base, (AVRational) { 1, 1000000 });
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= start_us;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= start_us;
            if (pkt->stream_index == in.vidx) video_send_packet(vsession, pkt);
            else audio_send_packet(asession, pkt);
        }
        av_packet_unref(pkt);
    }
    error = false;  /* stopped */

done:
    av_packet_free(&pkt);
    close_input(&in);
    free(remote_master);
    remote_master = NULL;
    finish(error, at_end);
    return NULL;
}

/* ---- control ---- */

void player_start(player_request_t *r, const player_options_t *opts) {
    player_stop();
    if (!r) return;

    /* serve a single-variant master playlist so FFmpeg does not open every quality */
    playlist_t *master = r->local_prefix[0] ? find_playlist(r, "/master.m3u8") : NULL;
    if (master) {
        char *reduced = pick_variant(master->data, opts->max_height);
        if (reduced) {
            free(master->data);
            master->data = reduced;
            master->len = strlen(reduced);
        }
    }

    pthread_mutex_lock(&mutex);
    req = r;
    options = *opts;
    state = PLAYER_LOADING;
    failed = false;
    paused = false;
    seek_requested = false;
    eof = false;
    duration = 0.0;
    last_position = r->start;
    has_audio = false;
    has_video = false;
    completed = false;
    paused_at = AV_NOPTS_VALUE;
    reset_clock_locked();
    pthread_mutex_unlock(&mutex);

    abort_request = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PLAYER_STACK_SIZE);
    thread_started = pthread_create(&thread, &attr, player_thread, NULL) == 0;
    pthread_attr_destroy(&attr);
    if (!thread_started) {
        platform_log("player: could not start the player thread");
        finish(true, false);
    }
}

void player_stop(void) {
    if (thread_started) {
        abort_request = 1;
        /* unblock the thread if it waits on a full decode queue */
        video_end_stream();
        audio_end_stream();
        pthread_join(thread, NULL);
        thread_started = false;
        platform_log("player: stopped");
    }
    video_end_stream();
    audio_end_stream();
    pthread_mutex_lock(&mutex);
    if (state != PLAYER_IDLE) state = PLAYER_ENDED;
    paused = false;
    player_request_free(req);
    req = NULL;
    pthread_mutex_unlock(&mutex);
}

void player_reset(void) {
    pthread_mutex_lock(&mutex);
    if (state == PLAYER_ENDED) {
        state = PLAYER_IDLE;
        failed = false;
        completed = false;
    }
    pthread_mutex_unlock(&mutex);
}

void player_set_paused(bool p) {
    pthread_mutex_lock(&mutex);
    if (p != paused) {
        if (state == PLAYER_PLAYING) {
            int64_t now = av_gettime_relative();
            if (p) {
                paused_at = running_clock_locked();
                if (paused_at == AV_NOPTS_VALUE) paused_at = (int64_t) (last_position * 1e6);
            } else {
                /* continue the wall clock (if used) from where it stopped */
                wall_base_pts = paused_at;
                wall_base_time = now;
                /* the steady clock picks the audio clock up again where it resumes */
                steady_base = AV_NOPTS_VALUE;
                if (freerun_base != AV_NOPTS_VALUE) {
                    freerun_base = paused_at;
                    freerun_time = now;
                }
            }
            audio_set_paused(p);
        }
        paused = p;
    }
    pthread_mutex_unlock(&mutex);
}

void player_seek(double seconds) {
    pthread_mutex_lock(&mutex);
    if (state == PLAYER_LOADING || state == PLAYER_PLAYING) {
        if (seconds < 0) seconds = 0;
        if (duration > 2.0 && seconds > duration - 1.0) seconds = duration - 1.0;
        seek_requested = true;
        seek_target = seconds;
        last_position = seconds;
    }
    pthread_mutex_unlock(&mutex);
}

int64_t player_clock_us(void) {
    pthread_mutex_lock(&mutex);
    int64_t clock = AV_NOPTS_VALUE;
    if (state == PLAYER_PLAYING && !seek_requested) {
        clock = paused ? paused_at : running_clock_locked();
        if (clock != AV_NOPTS_VALUE) {
            double pos = (double) clock / 1e6;
            if (pos < 0) pos = 0;
            if (duration > 0 && pos > duration) pos = duration;
            last_position = pos;
        }
    }
    pthread_mutex_unlock(&mutex);
    return clock;
}

void player_get_status(player_status_t *st) {
    pthread_mutex_lock(&mutex);
    st->state = state;
    st->failed = failed;
    st->completed = state == PLAYER_ENDED && completed;
    st->has_video = has_video;
    st->paused = paused;
    st->position = seek_requested ? seek_target : last_position;
    st->duration = duration;
    st->buffered = has_audio ? audio_queued_ms() / 1000.0 : video_queued_packets() / 30.0;
    st->buffering = state == PLAYER_LOADING ||
                    (state == PLAYER_PLAYING && !paused && !eof && video_queued_frames() == 0 &&
                     (!has_audio || audio_queued_ms() == 0));
    pthread_mutex_unlock(&mutex);
}
