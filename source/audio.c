/*
 * SwitchAirCast - AirPlay audio playback
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "audio.h"

#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include "platform.h"

#define OUT_RATE 44100
#define OUT_CHANNELS 2
#define BYTES_PER_SEC (OUT_RATE * OUT_CHANNELS * 2)
/* silence queued in front of a stream so Wi-Fi jitter does not cause dropouts */
#define CUSHION_MS 80
/* if more than this is queued the output has drifted behind; drop it and resync */
#define MAX_QUEUE_MS_MIRROR 300
#define MAX_QUEUE_MS_MUSIC 700

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID device = 0;
static AVCodecContext *ctx = NULL;
static SwrContext *swr = NULL;
static AVFrame *frame = NULL;
static AVPacket *packet = NULL;
static unsigned char cur_ct = 0;
static float volume_db = 0.0f;
static float gain = 1.0f;
static int16_t *out_buf = NULL;
static int out_buf_samples = 0;
static uint64_t last_push_us = 0;
static unsigned long frames_decoded = 0;

/* stream mode (media playback): timestamps drive an audio clock for A/V sync */
static bool stream_mode = false;
static int stream_session = 0;
static int64_t clock_end_us = AV_NOPTS_VALUE;   /* pts just after the last queued sample */
static int64_t skip_until_us = AV_NOPTS_VALUE;  /* drop audio before this (after a seek) */
static int64_t device_latency_us = 0;           /* audio in the device buffer, not in SDL's queue */

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}

bool audio_init(void) {
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = OUT_CHANNELS;
    want.samples = 1024;
    want.callback = NULL; /* SDL_QueueAudio; SDL converts to the device format */
    device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!device) {
        platform_log("audio: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    device_latency_us = (int64_t) have.samples * 1000000 / (have.freq > 0 ? have.freq : OUT_RATE);
    SDL_PauseAudioDevice(device, 0);
    platform_log("audio: device open (%d Hz, %d ch)", have.freq, have.channels);
    return true;
}

static void close_decoder_locked(void) {
    avcodec_free_context(&ctx);
    swr_free(&swr);
    cur_ct = 0;
}

void audio_shutdown(void) {
    pthread_mutex_lock(&mutex);
    close_decoder_locked();
    av_frame_free(&frame);
    av_packet_free(&packet);
    av_free(out_buf);
    out_buf = NULL;
    out_buf_samples = 0;
    if (device) SDL_CloseAudioDevice(device);
    device = 0;
    pthread_mutex_unlock(&mutex);
}

static const uint8_t alac_cookie[36] = {
    0x00, 0x00, 0x00, 0x24, 'a', 'l', 'a', 'c', 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x60,   /* frame length 352 */
    0x00, 0x10, 0x28, 0x0a,   /* version 0, 16 bit, pb 40, mb 10 */
    0x0e, 0x02, 0x00, 0xff,   /* kb 14, 2 channels, max run 255 */
    0x00, 0x00, 0x00, 0x00,   /* max frame bytes */
    0x00, 0x00, 0x00, 0x00,   /* average bitrate */
    0x00, 0x00, 0xac, 0x44,   /* 44100 Hz */
};

static int open_decoder_locked(unsigned char ct, unsigned short spf) {
    close_decoder_locked();
    enum AVCodecID id;
    uint8_t asc[4];
    const uint8_t *extra = NULL;
    int extra_len = 0;
    switch (ct) {
    case 1:
        id = AV_CODEC_ID_PCM_S16LE;
        break;
    case 2:
        id = AV_CODEC_ID_ALAC;
        extra = alac_cookie;
        extra_len = sizeof(alac_cookie);
        break;
    case 4:
        /* AudioSpecificConfig: AAC-LC, 44100 Hz, stereo */
        id = AV_CODEC_ID_AAC;
        asc[0] = 0x12; asc[1] = 0x10;
        extra = asc;
        extra_len = 2;
        break;
    case 8:
        /* AudioSpecificConfig: ER AAC-ELD, 44100 Hz, stereo, 480 (or 512) samples per frame */
        id = AV_CODEC_ID_AAC;
        asc[0] = 0xf8; asc[1] = 0xe8; asc[2] = (spf == 512) ? 0x40 : 0x50; asc[3] = 0x00;
        extra = asc;
        extra_len = 4;
        break;
    default:
        platform_log("audio: unsupported compression type %d", ct);
        return -1;
    }
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) {
        platform_log("audio: decoder for type %d missing", ct);
        return -1;
    }
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) return -1;
    ctx->sample_rate = OUT_RATE;
    av_channel_layout_default(&ctx->ch_layout, OUT_CHANNELS);
    if (id == AV_CODEC_ID_PCM_S16LE) ctx->sample_fmt = AV_SAMPLE_FMT_S16;
    if (extra) {
        ctx->extradata = av_mallocz((size_t) extra_len + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(ctx->extradata, extra, (size_t) extra_len);
        ctx->extradata_size = extra_len;
    }
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        platform_log("audio: avcodec_open2 failed for type %d", ct);
        avcodec_free_context(&ctx);
        return -1;
    }
    cur_ct = ct;
    platform_log("audio: decoder ready (type %d, %d samples/frame)", ct, spf);
    return 0;
}

static void leave_stream_mode_locked(void) {
    stream_mode = false;
    stream_session++;
    clock_end_us = AV_NOPTS_VALUE;
    skip_until_us = AV_NOPTS_VALUE;
    if (device) SDL_PauseAudioDevice(device, 0);
}

void audio_start(unsigned char ct, unsigned short spf) {
    pthread_mutex_lock(&mutex);
    leave_stream_mode_locked();
    open_decoder_locked(ct, spf);
    if (device) SDL_ClearQueuedAudio(device);
    pthread_mutex_unlock(&mutex);
}

void audio_stop(void) {
    pthread_mutex_lock(&mutex);
    leave_stream_mode_locked();
    close_decoder_locked();
    if (device) SDL_ClearQueuedAudio(device);
    last_push_us = 0;
    frames_decoded = 0;
    pthread_mutex_unlock(&mutex);
}

void audio_flush(void) {
    audio_flush_to(AV_NOPTS_VALUE);
}

void audio_flush_to(int64_t skip_before_us) {
    pthread_mutex_lock(&mutex);
    if (ctx) avcodec_flush_buffers(ctx);
    if (device) SDL_ClearQueuedAudio(device);
    clock_end_us = AV_NOPTS_VALUE;
    skip_until_us = skip_before_us;
    pthread_mutex_unlock(&mutex);
}

int audio_open_stream(const AVCodecParameters *par) {
    pthread_mutex_lock(&mutex);
    close_decoder_locked();
    leave_stream_mode_locked();
    int session = -1;
    const AVCodec *codec = device ? avcodec_find_decoder(par->codec_id) : NULL;
    if (codec && (ctx = avcodec_alloc_context3(codec)) && avcodec_parameters_to_context(ctx, par) >= 0) {
        ctx->pkt_timebase = (AVRational) { 1, 1000000 };
        if (avcodec_open2(ctx, codec, NULL) >= 0) {
            stream_mode = true;
            session = stream_session;
            platform_log("audio: playing %s stream (%d Hz, %d ch)", avcodec_get_name(par->codec_id),
                         par->sample_rate, par->ch_layout.nb_channels);
        }
    }
    if (session < 0) {
        platform_log("audio: cannot decode %s stream", avcodec_get_name(par->codec_id));
        close_decoder_locked();
    }
    if (device) SDL_ClearQueuedAudio(device);
    pthread_mutex_unlock(&mutex);
    return session;
}

void audio_end_stream(void) {
    pthread_mutex_lock(&mutex);
    if (stream_mode) {
        leave_stream_mode_locked();
        close_decoder_locked();
        if (device) SDL_ClearQueuedAudio(device);
    }
    pthread_mutex_unlock(&mutex);
}

void audio_set_paused(bool paused) {
    pthread_mutex_lock(&mutex);
    if (device) SDL_PauseAudioDevice(device, paused ? 1 : 0);
    pthread_mutex_unlock(&mutex);
}

int64_t audio_clock_us(void) {
    pthread_mutex_lock(&mutex);
    int64_t clock = AV_NOPTS_VALUE;
    if (stream_mode && device && clock_end_us != AV_NOPTS_VALUE) {
        int64_t queued_us = (int64_t) SDL_GetQueuedAudioSize(device) * 1000000 / BYTES_PER_SEC;
        clock = clock_end_us - queued_us - device_latency_us;
    }
    pthread_mutex_unlock(&mutex);
    return clock;
}

int audio_queued_ms(void) {
    pthread_mutex_lock(&mutex);
    int ms = device ? (int) ((uint64_t) SDL_GetQueuedAudioSize(device) * 1000 / BYTES_PER_SEC) : 0;
    pthread_mutex_unlock(&mutex);
    return ms;
}

void audio_set_volume(float db) {
    pthread_mutex_lock(&mutex);
    volume_db = db;
    if (db <= -144.0f) {
        gain = 0.0f;
    } else {
        if (db < -30.0f) db = -30.0f;
        if (db > 0.0f) db = 0.0f;
        gain = (db <= -30.0f) ? 0.0f : powf(10.0f, db / 20.0f);
    }
    pthread_mutex_unlock(&mutex);
}

float audio_get_volume(void) {
    pthread_mutex_lock(&mutex);
    float v = volume_db;
    pthread_mutex_unlock(&mutex);
    return v;
}

static void queue_samples_locked(const int16_t *samples, int count) {
    int bytes = count * OUT_CHANNELS * 2;
    Uint32 queued = SDL_GetQueuedAudioSize(device);
    int max_ms = (cur_ct == 8) ? MAX_QUEUE_MS_MIRROR : MAX_QUEUE_MS_MUSIC;
    if (queued > (Uint32) (BYTES_PER_SEC / 1000 * max_ms)) {
        SDL_ClearQueuedAudio(device);
        queued = 0;
    }
    if (queued == 0) {
        static uint8_t silence[BYTES_PER_SEC / 1000 * CUSHION_MS];
        SDL_QueueAudio(device, silence, sizeof(silence));
    }
    SDL_QueueAudio(device, samples, (Uint32) bytes);
}

static void output_frame_locked(AVFrame *f, int64_t pts_us) {
    if (!swr) {
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, OUT_CHANNELS);
        if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, OUT_RATE,
                                &f->ch_layout, (enum AVSampleFormat) f->format, f->sample_rate, 0, NULL) < 0 ||
            swr_init(swr) < 0) {
            platform_log("audio: resampler setup failed");
            swr_free(&swr);
            return;
        }
    }
    int max_out = swr_get_out_samples(swr, f->nb_samples);
    if (max_out > out_buf_samples) {
        av_free(out_buf);
        out_buf = av_malloc((size_t) max_out * OUT_CHANNELS * sizeof(int16_t));
        out_buf_samples = out_buf ? max_out : 0;
        if (!out_buf) return;
    }
    uint8_t *out_planes[1] = { (uint8_t *) out_buf };
    int n = swr_convert(swr, out_planes, max_out, (const uint8_t **) f->extended_data, f->nb_samples);
    if (n <= 0) return;
    if (gain != 1.0f) {
        int total = n * OUT_CHANNELS;
        for (int i = 0; i < total; i++) {
            float s = (float) out_buf[i] * gain;
            out_buf[i] = (int16_t) (s > 32767.0f ? 32767.0f : (s < -32768.0f ? -32768.0f : s));
        }
    }
    if (!stream_mode) {
        queue_samples_locked(out_buf, n);
        return;
    }
    if (pts_us == AV_NOPTS_VALUE) pts_us = clock_end_us;
    int64_t end_us = pts_us == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : pts_us + (int64_t) n * 1000000 / OUT_RATE;
    int first = 0;
    if (skip_until_us != AV_NOPTS_VALUE && pts_us != AV_NOPTS_VALUE) {
        if (end_us <= skip_until_us) return;
        if (pts_us < skip_until_us) first = (int) ((skip_until_us - pts_us) * OUT_RATE / 1000000);
        skip_until_us = AV_NOPTS_VALUE;
    }
    if (first < n) {
        SDL_QueueAudio(device, out_buf + first * OUT_CHANNELS, (Uint32) ((n - first) * OUT_CHANNELS * 2));
    }
    if (end_us != AV_NOPTS_VALUE) clock_end_us = end_us;
}

void audio_push(const uint8_t *data, int len, unsigned char ct) {
    if (len <= 0) return;
    pthread_mutex_lock(&mutex);
    if (stream_mode) {
        /* media playback owns the output; AirPlay audio packets are not expected now */
        pthread_mutex_unlock(&mutex);
        return;
    }
    last_push_us = now_us();
    if (!device || (ct && ct != cur_ct)) {
        if (device && ct) open_decoder_locked(ct, 480);
        if (!device || !ctx) {
            pthread_mutex_unlock(&mutex);
            return;
        }
    }
    if (!ctx || av_new_packet(packet, len) < 0) {
        pthread_mutex_unlock(&mutex);
        return;
    }
    memcpy(packet->data, data, (size_t) len);
    if (avcodec_send_packet(ctx, packet) == 0) {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (!frames_decoded++) {
                platform_log("audio: first frame decoded (%d samples, %d Hz)", frame->nb_samples, frame->sample_rate);
            }
            output_frame_locked(frame, AV_NOPTS_VALUE);
            av_frame_unref(frame);
        }
    }
    av_packet_unref(packet);
    pthread_mutex_unlock(&mutex);
}

bool audio_active(void) {
    pthread_mutex_lock(&mutex);
    bool active = last_push_us && (now_us() - last_push_us) < 1000000ULL;
    pthread_mutex_unlock(&mutex);
    return active;
}

void audio_send_packet(int session, const AVPacket *pkt) {
    pthread_mutex_lock(&mutex);
    if (!stream_mode || session != stream_session || !ctx || !device) {
        pthread_mutex_unlock(&mutex);
        return;
    }
    last_push_us = now_us();
    if (avcodec_send_packet(ctx, pkt) == 0) {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            output_frame_locked(frame, frame->best_effort_timestamp);
            av_frame_unref(frame);
        }
    }
    pthread_mutex_unlock(&mutex);
}
