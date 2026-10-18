/*
 * SwitchAirCast - AirPlay audio playback (AAC-ELD, AAC-LC, ALAC, PCM via FFmpeg + SDL audio)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SWITCHCAST_AUDIO_H
#define SWITCHCAST_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>

bool audio_init(void);
void audio_shutdown(void);

/* ct is the AirPlay compression type: 1 = PCM, 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD.
 * spf is samples per frame as announced by the sender. */
void audio_start(unsigned char ct, unsigned short spf);
void audio_stop(void);
void audio_flush(void);

/* AirPlay volume in dB: -30 (quietest) .. 0 (full), -144 = mute. */
void audio_set_volume(float db);
float audio_get_volume(void);

/* One decrypted audio frame from the AirPlay RTP thread. */
void audio_push(const uint8_t *data, int len, unsigned char ct);

/* True while audio was received during the last second. */
bool audio_active(void);

/* ---- media playback (AirPlay video) ---- */

/* Opens a decoder for a demuxed audio stream and switches to timestamped playback.
 * Returns a session id for audio_send_packet, or -1 if the stream cannot be played. */
int audio_open_stream(const AVCodecParameters *par);

/* Decodes and queues one packet (pts in microseconds). */
void audio_send_packet(int session, const AVPacket *pkt);

/* Ends media playback (no-op if AirPlay audio has taken over the output). */
void audio_end_stream(void);

/* Clears queued audio and decoder state; audio before skip_before_us is dropped. */
void audio_flush_to(int64_t skip_before_us);

void audio_set_paused(bool paused);

/* Playback position of the audio being heard (microseconds), or AV_NOPTS_VALUE before
 * any audio was queued. Stops advancing while paused or starved. */
int64_t audio_clock_us(void);

/* Milliseconds of audio queued for output. */
int audio_queued_ms(void);

#endif
