/*
 * SwitchAirCast - video decoding (FFmpeg; NVDEC hardware decoding on Switch)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SWITCHCAST_VIDEO_H
#define SWITCHCAST_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/frame.h>

typedef struct {
    int width;
    int height;
    float fps;              /* decoded frames per second over the last second */
    bool hw;                /* hardware decoder in use */
    const char *codec;      /* FFmpeg codec name, e.g. "h264" */
    unsigned long frames;   /* frames decoded since the stream started */
    unsigned long dropped;  /* frames decoded but never shown (screen behind) */
    float decode_ms;        /* average decode + copy time per frame */
} video_stats_t;

bool video_init(bool allow_hw);
void video_shutdown(void);

/* Drops queued packets, decoded frames and decoder state. */
void video_flush(void);

void video_get_stats(video_stats_t *out);

/* ---- screen mirroring ---- */

/* Selects the mirror stream codec and (re)opens the decoder. Returns 0 on success. */
int video_set_codec(bool h265);

/* Queues one Annex-B access unit (start-code delimited NAL units) for the decode
 * thread. Called from the AirPlay mirroring thread; blocks if the queue is full. */
void video_decode(const uint8_t *data, int len);

/* Main thread: returns the newest decoded frame (NV12 or YUV420P), or NULL if no new
 * frame arrived. The caller owns the frame and frees it with av_frame_free(). */
AVFrame *video_take_frame(void);

/* ---- media playback (AirPlay video) ---- */

/* Opens a decoder for a demuxed stream. Returns a session id for video_send_packet,
 * or -1 on failure. Any mirroring in progress stops being decoded. */
int video_open_stream(const AVCodecParameters *par);

/* Ends media playback: drops its packets and frames and refuses its later packets.
 * Does nothing if mirroring has taken over the decoder. */
void video_end_stream(void);

/* Queues a packet whose pts/dts are in microseconds; NULL drains the decoder at the end
 * of the stream. Blocks while the queue is full. Returns false if the packet was
 * discarded (flush, a newer session, or shutdown). */
bool video_send_packet(int session, const AVPacket *pkt);

/* Like video_flush, and also discards decoded frames earlier than skip_before_us
 * (the target of a seek, which lands on the keyframe before it). */
void video_flush_to(int64_t skip_before_us);

/* Main thread: returns the frame that should be on screen at clock_us, skipping frames
 * that are already late, or NULL if the next frame is not due yet. */
AVFrame *video_take_due_frame(int64_t clock_us);

/* pts of the next frame waiting to be shown, or AV_NOPTS_VALUE */
int64_t video_next_frame_pts(void);

int video_queued_packets(void);
int video_queued_frames(void);

/* True once the end-of-stream drain has finished and every frame has been taken. */
bool video_stream_finished(void);

#endif
