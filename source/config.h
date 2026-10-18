/*
 * SwitchAirCast - user settings (config.ini in the data directory)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SWITCHCAST_CONFIG_H
#define SWITCHCAST_CONFIG_H

#include <stdbool.h>

typedef enum { LANGUAGE_AUTO, LANGUAGE_EN, LANGUAGE_ES } ui_language_t;
typedef enum { THEME_AUTO, THEME_DARK, THEME_BLACK } theme_t;
/* how mirrored/cast video is turned for holding the console upright */
typedef enum { ROTATION_OFF, ROTATION_AUTO, ROTATION_LEFT, ROTATION_RIGHT } rotation_mode_t;

typedef struct {
    char name[64];      /* AirPlay name; empty = console nickname */
    int width;          /* resolution requested from the sender */
    int height;
    int max_fps;
    bool audio;         /* play AirPlay audio */
    bool video_casting; /* accept AirPlay video (the AirPlay button in YouTube etc.) */
    char video_language[32]; /* audio track for AirPlay video: "default", "system" or codes like "es:en" */
    bool hw_decode;     /* NVDEC hardware video decoding */
    bool show_stats;    /* start with the statistics overlay visible */
    int port;           /* AirPlay TCP port */
    bool debug_log;     /* verbose protocol logging to log.txt */
    /* settings menu */
    ui_language_t language;
    theme_t theme;
    int screen_off_minutes;  /* 0 = never */
    rotation_mode_t rotation;
} config_t;

void config_defaults(config_t *cfg);

/* Loads path over the defaults; writes a commented default file if path does not exist. */
void config_load(config_t *cfg, const char *path);

/* Writes every setting to path (with comments). Returns false on failure. */
bool config_save(const config_t *cfg, const char *path);

#endif
