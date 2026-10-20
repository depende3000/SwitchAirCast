/*
 * SwitchAirCast - video decoding
 *
 * Two modes share one decoder thread and one NVDEC device:
 *  - mirroring: Annex-B access units from the AirPlay mirror stream; decoded frames
 *    wait in a queue of a few frames, shown one per screen refresh, so frames that
 *    arrive in a burst (Wi-Fi jitter) are spread out instead of dropped.
 *  - stream:    timestamped packets from the media player (AirPlay video); decoded
 *    frames wait in a short queue until the playback clock reaches them.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "video.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include "platform.h"

/* consecutive failed packets before giving up on the hardware decoder */
#define HW_FAILURE_LIMIT 30
/* packets waiting for the decode thread */
#define QUEUE_MAX 512
/* mirroring: block the network thread sooner, about one second at 60 fps */
#define MIRROR_QUEUE_LIMIT 64
/* mirroring: decoded frames waiting for the screen; when full the oldest is dropped */
#define MIRROR_FRAMES_MAX 3
/* mirroring: a backlog that never clears for this long is cut by one frame (latency) */
#define MIRROR_BACKLOG_US 500000
/* stream mode: decoded frames waiting for their presentation time */
#define FRAME_FIFO_MAX 8
/* a frame this close to its time is shown now rather than on the next vsync */
#define PRESENT_EARLY_US 8000
/* a frame is skipped only once the one after it is this late (half a 60 Hz refresh), so
 * timing jitter shows a frame one refresh late instead of skipping it */
#define SKIP_LATE_US 8000
/* FFmpeg's H.264/HEVC decoders want more than libnx's default 128 KiB thread stack */
#define DECODE_STACK_SIZE (1024 * 1024)

typedef enum { MODE_MIRROR, MODE_STREAM } video_mode_t;

typedef struct {
    AVPacket *pkt;   /* NULL asks the decoder to drain (end of stream) */
    unsigned gen;
} queue_entry_t;

/* state_mutex guards the packet queue, both frame outputs, gen, mode and stats.
 * dec_mutex guards the codec context. Lock order: dec_mutex, then state_mutex. */
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t queue_not_full = PTHREAD_COND_INITIALIZER;
static pthread_cond_t fifo_not_full = PTHREAD_COND_INITIALIZER;
static queue_entry_t queue[QUEUE_MAX];
static int queue_head = 0;
static int queue_count = 0;
/* bumped by every flush; queued packets and decoded frames of older generations are dropped */
static unsigned gen = 0;
static bool worker_running = false;
static pthread_t worker;
static video_mode_t mode = MODE_MIRROR;
static int stream_session = 0;

static AVFrame *mirror_frames[MIRROR_FRAMES_MAX]; /* mirroring: frames in decode order */
static int mirror_head = 0;
static int mirror_count = 0;
static uint64_t mirror_backlog_since = 0;    /* mirroring: when more than one frame began waiting */
static AVFrame *fifo[FRAME_FIFO_MAX];        /* stream: frames in presentation order */
static int fifo_head = 0;
static int fifo_count = 0;
static int64_t skip_until = AV_NOPTS_VALUE;  /* stream: drop frames before this time (after a seek) */
static bool drained = false;                 /* stream: decoder emptied after end of stream */

static pthread_mutex_t dec_mutex = PTHREAD_MUTEX_INITIALIZER;
static AVBufferRef *hw_device = NULL;
static AVCodecContext *ctx = NULL;
static AVFrame *dec_frame = NULL;
static enum AVCodecID cur_codec = AV_CODEC_ID_NONE;
static bool cur_hw = false;
static int failures = 0;

static video_stats_t stats;
static uint64_t fps_window_start = 0;
static unsigned long fps_window_frames = 0;
static double decode_time_total = 0.0;
static unsigned long decode_time_count = 0;
/* shown_percent: frames decoded and dropped in each of the last few one-second windows */
#define SHOWN_WINDOWS 5
static unsigned long window_frames[SHOWN_WINDOWS], window_dropped[SHOWN_WINDOWS];
static int window_index = 0, windows_filled = 0;
static unsigned long window_dropped_start = 0;

static void *decode_thread(void *arg);

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}

static enum AVPixelFormat get_format(AVCodecContext *avctx, const enum AVPixelFormat *fmts) {
    const enum AVPixelFormat *p;
#ifdef __SWITCH__
    if (avctx->hw_device_ctx) {
        for (p = fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p == AV_PIX_FMT_NVTEGRA) return *p;
        }
        platform_log("video: decoder did not offer NVTEGRA, using software");
    }
#endif
    /* FFmpeg calls this again without the hwaccel format if hardware init fails */
    for (p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}

static void close_decoder_locked(void) {
    avcodec_free_context(&ctx);
    cur_hw = false;
    cur_codec = AV_CODEC_ID_NONE;
}

/* par is NULL for mirroring (codec configured in-band from SPS/PPS) */
static int open_decoder_locked(enum AVCodecID id, const AVCodecParameters *par, bool use_hw) {
    close_decoder_locked();
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) {
        platform_log("video: no decoder for %s", avcodec_get_name(id));
        return -1;
    }
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) return -1;
    if (par && avcodec_parameters_to_context(ctx, par) < 0) {
        avcodec_free_context(&ctx);
        return -1;
    }
    /* mirroring streams have no B-frames; low delay must stay off for media files */
    if (!par) ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->pkt_timebase = (AVRational) { 1, 1000000 };
    ctx->get_format = get_format;
    if (use_hw && hw_device) {
        ctx->hw_device_ctx = av_buffer_ref(hw_device);
        ctx->thread_count = 1;
    } else {
        /* the A57 cores need frame threading to keep up with 1080p */
        ctx->thread_count = 3;
        ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        use_hw = false;
    }
    int ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0) {
        platform_log("video: avcodec_open2 failed (%d)", ret);
        avcodec_free_context(&ctx);
        return -1;
    }
    cur_codec = id;
    cur_hw = use_hw;
    failures = 0;
    platform_log("video: opened %s decoder (%s)", avcodec_get_name(id), use_hw ? "hardware" : "software");
    return 0;
}

bool video_init(bool allow_hw) {
    dec_frame = av_frame_alloc();
    if (!dec_frame) return false;
#ifdef __SWITCH__
    if (allow_hw) {
        int ret = av_hwdevice_ctx_create(&hw_device, AV_HWDEVICE_TYPE_NVTEGRA, NULL, NULL, 0);
        if (ret < 0) {
            platform_log("video: NVDEC unavailable (%d), using software decoding", ret);
            hw_device = NULL;
        }
    }
#else
    (void) allow_hw;
#endif
    pthread_mutex_lock(&dec_mutex);
    int ret = open_decoder_locked(AV_CODEC_ID_H264, NULL, hw_device != NULL);
    pthread_mutex_unlock(&dec_mutex);
    if (ret != 0) return false;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, DECODE_STACK_SIZE);
    worker_running = true;
    if (pthread_create(&worker, &attr, decode_thread, NULL) != 0) {
        platform_log("video: could not start the decode thread");
        worker_running = false;
    }
    pthread_attr_destroy(&attr);
    return worker_running;
}

static void reset_stats_locked(void) {
    memset(&stats, 0, sizeof(stats));
    stats.shown_percent = -1.0f;
    window_index = windows_filled = 0;
    window_dropped_start = 0;
    fps_window_start = 0;
    fps_window_frames = 0;
    decode_time_total = 0.0;
    decode_time_count = 0;
}

/* Drops everything queued or decoded; state_mutex must be held. */
static void clear_outputs_locked(int64_t new_skip_until) {
    gen++;
    while (queue_count > 0) {
        av_packet_free(&queue[queue_head].pkt);
        queue_head = (queue_head + 1) % QUEUE_MAX;
        queue_count--;
    }
    while (mirror_count > 0) {
        av_frame_free(&mirror_frames[mirror_head]);
        mirror_head = (mirror_head + 1) % MIRROR_FRAMES_MAX;
        mirror_count--;
    }
    mirror_backlog_since = 0;
    while (fifo_count > 0) {
        av_frame_free(&fifo[fifo_head]);
        fifo_head = (fifo_head + 1) % FRAME_FIFO_MAX;
        fifo_count--;
    }
    skip_until = new_skip_until;
    drained = false;
    reset_stats_locked();
    pthread_cond_broadcast(&queue_not_full);
    pthread_cond_broadcast(&fifo_not_full);
}

void video_shutdown(void) {
    pthread_mutex_lock(&state_mutex);
    bool was_running = worker_running;
    worker_running = false;
    clear_outputs_locked(AV_NOPTS_VALUE);
    pthread_cond_broadcast(&queue_not_empty);
    pthread_mutex_unlock(&state_mutex);
    if (was_running) pthread_join(worker, NULL);

    pthread_mutex_lock(&dec_mutex);
    close_decoder_locked();
    av_frame_free(&dec_frame);
    av_buffer_unref(&hw_device);
    pthread_mutex_unlock(&dec_mutex);
}

void video_flush_to(int64_t skip_before_us) {
    pthread_mutex_lock(&state_mutex);
    clear_outputs_locked(skip_before_us);
    pthread_mutex_unlock(&state_mutex);
    pthread_mutex_lock(&dec_mutex);
    if (ctx) avcodec_flush_buffers(ctx);
    pthread_mutex_unlock(&dec_mutex);
}

void video_flush(void) {
    video_flush_to(AV_NOPTS_VALUE);
}

int video_set_codec(bool h265) {
    enum AVCodecID id = h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    pthread_mutex_lock(&state_mutex);
    bool was_stream = (mode == MODE_STREAM);
    mode = MODE_MIRROR;
    stream_session++;
    clear_outputs_locked(AV_NOPTS_VALUE);
    pthread_mutex_unlock(&state_mutex);

    pthread_mutex_lock(&dec_mutex);
    int ret = 0;
    if (!ctx || cur_codec != id || was_stream) {
        ret = open_decoder_locked(id, NULL, hw_device != NULL);
    } else {
        avcodec_flush_buffers(ctx);
    }
    pthread_mutex_unlock(&dec_mutex);
    return ret;
}

int video_open_stream(const AVCodecParameters *par) {
    pthread_mutex_lock(&state_mutex);
    mode = MODE_STREAM;
    int session = ++stream_session;
    clear_outputs_locked(AV_NOPTS_VALUE);
    pthread_mutex_unlock(&state_mutex);

    pthread_mutex_lock(&dec_mutex);
    int ret = open_decoder_locked(par->codec_id, par, hw_device != NULL);
    pthread_mutex_unlock(&dec_mutex);
    return ret == 0 ? session : -1;
}

void video_end_stream(void) {
    pthread_mutex_lock(&state_mutex);
    bool active = (mode == MODE_STREAM);
    if (active) {
        /* invalidate the session: later video_send_packet calls are refused */
        stream_session++;
        clear_outputs_locked(AV_NOPTS_VALUE);
    }
    pthread_mutex_unlock(&state_mutex);
    if (active) {
        pthread_mutex_lock(&dec_mutex);
        if (ctx) avcodec_flush_buffers(ctx);
        pthread_mutex_unlock(&dec_mutex);
    }
}

static void update_stats_locked(const AVFrame *frame, uint64_t started) {
    uint64_t t = now_us();
    stats.frames++;
    stats.width = frame->width;
    stats.height = frame->height;
    stats.hw = cur_hw;
    stats.codec = avcodec_get_name(cur_codec);
    decode_time_total += (double) (t - started) / 1000.0;
    decode_time_count++;
    if (!fps_window_start) fps_window_start = t;
    fps_window_frames++;
    if (t - fps_window_start >= 1000000ULL) {
        stats.fps = (float) ((double) fps_window_frames * 1e6 / (double) (t - fps_window_start));
        window_frames[window_index] = fps_window_frames;
        window_dropped[window_index] = stats.dropped - window_dropped_start;
        window_dropped_start = stats.dropped;
        window_index = (window_index + 1) % SHOWN_WINDOWS;
        if (windows_filled < SHOWN_WINDOWS) windows_filled++;
        unsigned long f = 0, d = 0;
        for (int i = 0; i < windows_filled; i++) {
            f += window_frames[i];
            d += window_dropped[i];
        }
        if (d > f) d = f;
        stats.shown_percent = f ? (float) (100.0 * (double) (f - d) / (double) f) : -1.0f;
        stats.decode_ms = (float) (decode_time_total / (double) decode_time_count);
        fps_window_start = t;
        fps_window_frames = 0;
        decode_time_total = 0.0;
        decode_time_count = 0;
    }
}

/* Hands a decoded frame to the screen; takes ownership of frame. */
static void output_frame(AVFrame *frame, uint64_t started, unsigned packet_gen) {
    pthread_mutex_lock(&state_mutex);
    if (packet_gen != gen) {
        pthread_mutex_unlock(&state_mutex);
        av_frame_free(&frame);
        return;
    }
    update_stats_locked(frame, started);
    if (mode == MODE_MIRROR) {
        if (mirror_count == MIRROR_FRAMES_MAX) {
            av_frame_free(&mirror_frames[mirror_head]);
            mirror_head = (mirror_head + 1) % MIRROR_FRAMES_MAX;
            mirror_count--;
            stats.dropped++;
        }
        mirror_frames[(mirror_head + mirror_count) % MIRROR_FRAMES_MAX] = frame;
        mirror_count++;
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    if (skip_until != AV_NOPTS_VALUE && frame->pts != AV_NOPTS_VALUE && frame->pts < skip_until) {
        pthread_mutex_unlock(&state_mutex);
        av_frame_free(&frame);
        return;
    }
    while (worker_running && packet_gen == gen && fifo_count == FRAME_FIFO_MAX) {
        pthread_cond_wait(&fifo_not_full, &state_mutex);
    }
    if (!worker_running || packet_gen != gen) {
        pthread_mutex_unlock(&state_mutex);
        av_frame_free(&frame);
        return;
    }
    fifo[(fifo_head + fifo_count) % FRAME_FIFO_MAX] = frame;
    fifo_count++;
    pthread_mutex_unlock(&state_mutex);
}

/* Moves every frame the decoder has ready to the screen. Returns frames received. */
static int receive_frames_locked(uint64_t started, unsigned packet_gen) {
    int count = 0;
    while (avcodec_receive_frame(ctx, dec_frame) == 0) {
        AVFrame *out = NULL;
        if (dec_frame->hw_frames_ctx) {
            out = av_frame_alloc();
            if (out && av_hwframe_transfer_data(out, dec_frame, 0) == 0) {
                av_frame_copy_props(out, dec_frame);
            } else {
                av_frame_free(&out);
            }
        } else {
            out = av_frame_clone(dec_frame);
        }
        if (out) out->pts = dec_frame->best_effort_timestamp;
        av_frame_unref(dec_frame);
        if (out) {
            output_frame(out, started, packet_gen);
            count++;
        }
    }
    return count;
}

static void decode_packet(AVPacket *pkt, unsigned packet_gen) {
    uint64_t started = now_us();
    pthread_mutex_lock(&dec_mutex);
    if (!ctx) {
        pthread_mutex_unlock(&dec_mutex);
        return;
    }
    int ret = avcodec_send_packet(ctx, pkt);
    if (ret == AVERROR(EAGAIN)) {
        receive_frames_locked(started, packet_gen);
        ret = avcodec_send_packet(ctx, pkt);
    }
    int got = receive_frames_locked(started, packet_gen);

    if (ret < 0 && got == 0) {
        failures++;
        if (cur_hw && failures >= HW_FAILURE_LIMIT && ctx->codec) {
            platform_log("video: hardware decoder keeps failing (%d), switching to software", ret);
            AVCodecParameters *par = avcodec_parameters_alloc();
            bool stream = par && ctx->extradata_size > 0 && avcodec_parameters_from_context(par, ctx) >= 0;
            av_buffer_unref(&hw_device);
            open_decoder_locked(cur_codec, stream ? par : NULL, false);
            avcodec_parameters_free(&par);
        }
    } else if (got > 0) {
        failures = 0;
    }
    pthread_mutex_unlock(&dec_mutex);
}

static void drain_decoder(unsigned packet_gen) {
    uint64_t started = now_us();
    pthread_mutex_lock(&dec_mutex);
    if (ctx) {
        avcodec_send_packet(ctx, NULL);
        receive_frames_locked(started, packet_gen);
        /* the decoder accepts packets again only after a flush */
        avcodec_flush_buffers(ctx);
    }
    pthread_mutex_unlock(&dec_mutex);
    pthread_mutex_lock(&state_mutex);
    if (packet_gen == gen) drained = true;
    pthread_mutex_unlock(&state_mutex);
}

static void *decode_thread(void *arg) {
    (void) arg;
    for (;;) {
        pthread_mutex_lock(&state_mutex);
        while (worker_running && queue_count == 0) {
            pthread_cond_wait(&queue_not_empty, &state_mutex);
        }
        if (!worker_running) {
            pthread_mutex_unlock(&state_mutex);
            break;
        }
        queue_entry_t entry = queue[queue_head];
        queue[queue_head].pkt = NULL;
        queue_head = (queue_head + 1) % QUEUE_MAX;
        queue_count--;
        pthread_cond_broadcast(&queue_not_full);
        pthread_mutex_unlock(&state_mutex);

        if (entry.pkt) {
            decode_packet(entry.pkt, entry.gen);
            av_packet_free(&entry.pkt);
        } else {
            drain_decoder(entry.gen);
        }
    }
    return NULL;
}

/* Queues pkt (ownership passes to the queue). Waits while more than limit packets are
 * queued. Returns false, freeing pkt, if a flush or shutdown happened meanwhile. */
static bool enqueue(AVPacket *pkt, int limit, int session) {
    pthread_mutex_lock(&state_mutex);
    unsigned my_gen = gen;
    while (worker_running && my_gen == gen && queue_count >= limit) {
        pthread_cond_wait(&queue_not_full, &state_mutex);
    }
    bool ok = worker_running && my_gen == gen &&
              (session < 0 ? mode == MODE_MIRROR : (mode == MODE_STREAM && session == stream_session));
    if (ok) {
        queue[(queue_head + queue_count) % QUEUE_MAX] = (queue_entry_t) { pkt, gen };
        queue_count++;
        pthread_cond_signal(&queue_not_empty);
    }
    pthread_mutex_unlock(&state_mutex);
    if (!ok) av_packet_free(&pkt);
    return ok;
}

void video_decode(const uint8_t *data, int len) {
    if (len <= 0) return;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt || av_new_packet(pkt, len) < 0) {
        av_packet_free(&pkt);
        return;
    }
    memcpy(pkt->data, data, (size_t) len);
    /* back-pressure: let the decoder catch up rather than dropping reference frames */
    enqueue(pkt, MIRROR_QUEUE_LIMIT, -1);
}

bool video_send_packet(int session, const AVPacket *pkt) {
    AVPacket *copy = NULL;
    if (pkt) {
        copy = av_packet_clone(pkt);
        if (!copy) return false;
    }
    return enqueue(copy, QUEUE_MAX, session);
}

AVFrame *video_take_frame(void) {
    AVFrame *frame = NULL;
    pthread_mutex_lock(&state_mutex);
    if (mirror_count > 1) {
        /* the screen keeps showing frames late: skip one to catch up with the phone */
        uint64_t t = now_us();
        if (!mirror_backlog_since) {
            mirror_backlog_since = t;
        } else if (t - mirror_backlog_since >= MIRROR_BACKLOG_US) {
            av_frame_free(&mirror_frames[mirror_head]);
            mirror_head = (mirror_head + 1) % MIRROR_FRAMES_MAX;
            mirror_count--;
            stats.dropped++;
            mirror_backlog_since = 0;
        }
    } else {
        mirror_backlog_since = 0;
    }
    if (mirror_count > 0) {
        frame = mirror_frames[mirror_head];
        mirror_frames[mirror_head] = NULL;
        mirror_head = (mirror_head + 1) % MIRROR_FRAMES_MAX;
        mirror_count--;
    }
    pthread_mutex_unlock(&state_mutex);
    return frame;
}

AVFrame *video_take_due_frame(int64_t clock_us) {
    AVFrame *frame = NULL;
    pthread_mutex_lock(&state_mutex);
    if (fifo_count > 0 && clock_us != AV_NOPTS_VALUE) {
        /* skip frames whose successor is already late: the screen fell behind */
        while (fifo_count >= 2) {
            AVFrame *next = fifo[(fifo_head + 1) % FRAME_FIFO_MAX];
            if (next->pts == AV_NOPTS_VALUE || next->pts > clock_us - SKIP_LATE_US) break;
            av_frame_free(&fifo[fifo_head]);
            fifo_head = (fifo_head + 1) % FRAME_FIFO_MAX;
            fifo_count--;
            stats.dropped++;
        }
        AVFrame *head = fifo[fifo_head];
        if (head->pts == AV_NOPTS_VALUE || head->pts <= clock_us + PRESENT_EARLY_US) {
            frame = head;
            fifo[fifo_head] = NULL;
            fifo_head = (fifo_head + 1) % FRAME_FIFO_MAX;
            fifo_count--;
        }
        pthread_cond_broadcast(&fifo_not_full);
    }
    pthread_mutex_unlock(&state_mutex);
    return frame;
}

int64_t video_next_frame_pts(void) {
    int64_t pts = AV_NOPTS_VALUE;
    pthread_mutex_lock(&state_mutex);
    if (fifo_count > 0) pts = fifo[fifo_head]->pts;
    pthread_mutex_unlock(&state_mutex);
    return pts;
}

int video_queued_packets(void) {
    pthread_mutex_lock(&state_mutex);
    int n = queue_count;
    pthread_mutex_unlock(&state_mutex);
    return n;
}

int video_queued_frames(void) {
    pthread_mutex_lock(&state_mutex);
    int n = fifo_count;
    pthread_mutex_unlock(&state_mutex);
    return n;
}

bool video_stream_finished(void) {
    pthread_mutex_lock(&state_mutex);
    bool done = drained && queue_count == 0 && fifo_count == 0;
    pthread_mutex_unlock(&state_mutex);
    return done;
}

void video_get_stats(video_stats_t *out) {
    pthread_mutex_lock(&state_mutex);
    *out = stats;
    if (!stats.frames) out->shown_percent = -1.0f;
    if (!stats.frames) {
        out->hw = cur_hw;
        out->codec = avcodec_get_name(cur_codec);
    }
    pthread_mutex_unlock(&state_mutex);
}
