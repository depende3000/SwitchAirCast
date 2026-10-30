/*
 * SwitchAirCast - media player for AirPlay video (the AirPlay button in YouTube and
 * similar apps): HLS or plain http(s) URLs, demuxed by FFmpeg and decoded through the
 * shared video/audio modules, with video synchronized to the audio clock.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SWITCHCAST_PLAYER_H
#define SWITCHCAST_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PLAYER_IDLE,
    PLAYER_LOADING,   /* opening the stream */
    PLAYER_PLAYING,   /* playing or paused */
    PLAYER_ENDED,     /* reached the end, was stopped, or failed */
} player_state_t;

typedef struct {
    player_state_t state;
    bool failed;          /* ENDED because the stream could not be played */
    bool paused;
    bool buffering;       /* playing but waiting for data */
    double position;      /* seconds */
    double duration;      /* seconds, 0 if unknown (live) */
    double buffered;      /* seconds of audio/video queued ahead of the position */
    bool has_video;       /* PLAYING: the stream has a picture (false for music) */
    bool completed;       /* ENDED because the stream played to its end */
} player_status_t;

/* What to play. Playlists served under local_prefix (the URL prefix UxPlay's HLS
 * server uses) are read from memory instead of over the network. */
typedef struct player_request_s player_request_t;

player_request_t *player_request_new(const char *url, double start_seconds, const char *local_prefix);
void player_request_add_playlist(player_request_t *req, const char *path, const char *data, size_t len);
void player_request_free(player_request_t *req);

typedef struct {
    bool audio;           /* play the audio track */
    int max_height;       /* highest video variant to pick from an HLS master playlist */
} player_options_t;

/* Starts playing req (ownership passes to the player), stopping anything playing. */
void player_start(player_request_t *req, const player_options_t *opts);

/* Stops playback and waits for the player thread; the state becomes ENDED. */
void player_stop(void);

/* Forgets an ENDED player (state back to IDLE). */
void player_reset(void);

void player_set_paused(bool paused);
void player_seek(double seconds);
void player_get_status(player_status_t *st);

/* Main thread: the playback clock that video frames are shown against (microseconds),
 * or AV_NOPTS_VALUE while nothing should be shown yet. */
int64_t player_clock_us(void);

#endif
