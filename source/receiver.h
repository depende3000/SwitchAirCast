/*
 * SwitchAirCast - AirPlay receiver (UxPlay's raop + dnssd, wired to video/audio playback)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SWITCHCAST_RECEIVER_H
#define SWITCHCAST_RECEIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef enum {
    RECEIVER_WAITING,    /* advertised on the network, nobody connected */
    RECEIVER_CONNECTED,  /* a device is connected (handshake, or audio only) */
    RECEIVER_MIRRORING,  /* screen mirroring stream is running */
    RECEIVER_VIDEO,      /* playing a video sent with AirPlay (e.g. from YouTube) */
} receiver_state_t;

typedef struct {
    receiver_state_t state;
    char client_name[128];
    char client_model[64];
    bool audio_playing;
    /* "Now playing" information when streaming music */
    char title[128];
    char artist[128];
    char album[128];
    /* progress in seconds; position is extrapolated from when it was reported */
    double position;
    double duration;
    bool has_progress;
    unsigned coverart_serial;   /* changes whenever the cover art changes */
    char pin[8];                /* code a Mac must type to pair (pair-pin-start), empty if none */
} receiver_status_t;

/* Starts advertising and serving AirPlay as `name`. On failure writes a message to err. */
bool receiver_start(const config_t *cfg, const char *name, char *err, size_t errlen);
void receiver_stop(void);

/* Changes the picture size and frame rate asked from devices that connect from now on. */
void receiver_set_video_size(int width, int height, int max_fps);

/* Main thread, once per frame: handles connection resets and the client watchdog. */
void receiver_poll(void);

void receiver_get_status(receiver_status_t *st);

/* Disconnects the current device. */
void receiver_disconnect(void);

/* Hides the pairing code (the user pressed a button). */
void receiver_dismiss_pin(void);

/* Stops the AirPlay video being played. */
void receiver_stop_video(void);

/* Copies the newest cover art image (JPEG/PNG bytes). Returns false if there is none.
 * The caller frees *data. */
bool receiver_get_coverart(uint8_t **data, int *len);

#endif
