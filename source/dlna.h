/*
 * SwitchAirCast - DLNA / UPnP media renderer, so that Android apps (BubbleUPnP, Web Video
 * Caster, LocalCast...), Windows ("Cast to Device") and other control points can send
 * videos and music to the Switch.
 *
 * It answers SSDP discovery, serves the device and service descriptions, and handles the
 * AVTransport, RenderingControl and ConnectionManager services (SOAP control and GENA
 * events) on its own thread. Playback itself goes through the callbacks.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SWITCHCAST_DLNA_H
#define SWITCHCAST_DLNA_H

#include <stdbool.h>

typedef enum {
    DLNA_STOPPED,
    DLNA_TRANSITIONING,  /* opening the media */
    DLNA_PLAYING,
    DLNA_PAUSED,
} dlna_transport_t;

typedef struct {
    const char *url;
    double start;        /* seconds */
    /* from the DIDL-Lite metadata sent with the URL; empty strings if not given */
    const char *title;
    const char *artist;
    const char *album;
    const char *art_url; /* cover art (http), for music */
    const char *client;  /* the control point's name from its User-Agent, or its address */
} dlna_media_t;

typedef struct {
    dlna_transport_t state;  /* STOPPED once something else (e.g. AirPlay) replaced the media */
    bool ended;              /* played to the end (the queued next track may start) */
    bool failed;             /* could not be played */
    double position;         /* seconds */
    double duration;         /* seconds, 0 if unknown */
} dlna_playback_t;

/* Called on the DLNA thread. */
typedef struct {
    void (*play)(const dlna_media_t *media);
    void (*stop)(void);
    void (*set_paused)(bool paused);
    void (*seek)(double seconds);
    void (*get_playback)(dlna_playback_t *pb);
    int (*get_volume)(void);       /* 0..100 */
    void (*set_volume)(int volume);
    /* the cover art of the music being played (JPEG/PNG bytes) */
    void (*set_coverart)(const void *data, int len);
} dlna_callbacks_t;

/* Starts the renderer as `name`. device_id is the receiver's MAC-style id ("aa:bb:..."),
 * which keeps the renderer's UUID the same across runs. port 0 picks a free one. */
bool dlna_start(const char *name, const char *device_id, int port, const dlna_callbacks_t *cbs);

/* Says goodbye on the network and stops the thread. */
void dlna_stop(void);

/* The HTTP port in use, 0 if not running. */
int dlna_port(void);

#endif
