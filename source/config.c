/*
 * SwitchAirCast - user settings
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "platform.h"

static const char *const language_names[] = { "auto", "en", "es" };
static const char *const theme_names[] = { "auto", "dark", "black" };
static const char *const rotation_names[] = { "off", "auto", "left", "right" };
static const char *const resolution_names[] = { "auto", "1080p", "720p", "custom" };

void config_defaults(config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->resolution = RESOLUTION_AUTO;
    cfg->width = 1920;
    cfg->height = 1080;
    cfg->max_fps = 60;
    cfg->audio = true;
    cfg->video_casting = true;
    cfg->dlna = true;
    snprintf(cfg->video_language, sizeof(cfg->video_language), "default");
    cfg->hw_decode = true;
    cfg->show_stats = false;
    cfg->port = 7000;
    cfg->language = LANGUAGE_AUTO;
    cfg->theme = THEME_AUTO;
    cfg->screen_off_minutes = 5;
    cfg->rotation = ROTATION_OFF;
}

void config_video_size(const config_t *cfg, bool handheld, int *width, int *height) {
    resolution_mode_t r = cfg->resolution;
    if (r == RESOLUTION_AUTO) r = handheld ? RESOLUTION_720P : RESOLUTION_1080P;
    switch (r) {
    case RESOLUTION_720P: *width = 1280; *height = 720; break;
    case RESOLUTION_CUSTOM: *width = cfg->width; *height = cfg->height; break;
    default: *width = 1920; *height = 1080; break;
    }
}

static char *trim(char *s) {
    while (isspace((unsigned char) *s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char) end[-1])) *--end = '\0';
    return s;
}

static bool parse_bool(const char *v, bool fallback) {
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1")) return true;
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0")) return false;
    return fallback;
}

static int parse_int(const char *v, int fallback, int lo, int hi) {
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (!end || end == v || *end != '\0' || n < lo || n > hi) return fallback;
    return (int) n;
}

static int parse_name(const char *v, const char *const *names, int count, int fallback) {
    for (int i = 0; i < count; i++) {
        if (!strcasecmp(v, names[i])) return i;
    }
    return fallback;
}

#define COUNT(a) ((int) (sizeof(a) / sizeof((a)[0])))

void config_load(config_t *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        config_save(cfg, path);
        return;
    }
    char line[256];
    bool saw_resolution = false;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '#' || *s == ';' || *s == '\0') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        if (!strcasecmp(key, "name")) {
            /* the name becomes part of DNS labels; keep it printable */
            size_t n = 0;
            for (const char *p = val; *p && n < sizeof(cfg->name) - 1; p++) {
                if ((unsigned char) *p >= 0x20 && *p != '.' && *p != '@') cfg->name[n++] = *p;
            }
            cfg->name[n] = '\0';
        } else if (!strcasecmp(key, "resolution")) {
            cfg->resolution =
                (resolution_mode_t) parse_name(val, resolution_names, COUNT(resolution_names), cfg->resolution);
            saw_resolution = true;
        } else if (!strcasecmp(key, "width")) {
            cfg->width = parse_int(val, cfg->width, 320, 3840);
        } else if (!strcasecmp(key, "height")) {
            cfg->height = parse_int(val, cfg->height, 240, 2160);
        } else if (!strcasecmp(key, "max_fps")) {
            cfg->max_fps = parse_int(val, cfg->max_fps, 1, 60);
        } else if (!strcasecmp(key, "audio")) {
            cfg->audio = parse_bool(val, cfg->audio);
        } else if (!strcasecmp(key, "video_language")) {
            if (*val) snprintf(cfg->video_language, sizeof(cfg->video_language), "%s", val);
        } else if (!strcasecmp(key, "video_casting")) {
            cfg->video_casting = parse_bool(val, cfg->video_casting);
        } else if (!strcasecmp(key, "dlna")) {
            cfg->dlna = parse_bool(val, cfg->dlna);
        } else if (!strcasecmp(key, "hw_decode")) {
            cfg->hw_decode = parse_bool(val, cfg->hw_decode);
        } else if (!strcasecmp(key, "show_stats")) {
            cfg->show_stats = parse_bool(val, cfg->show_stats);
        } else if (!strcasecmp(key, "port")) {
            cfg->port = parse_int(val, cfg->port, 1024, 65535);
        } else if (!strcasecmp(key, "debug_log")) {
            cfg->debug_log = parse_bool(val, cfg->debug_log);
        } else if (!strcasecmp(key, "language")) {
            cfg->language = (ui_language_t) parse_name(val, language_names, COUNT(language_names), cfg->language);
        } else if (!strcasecmp(key, "theme")) {
            cfg->theme = (theme_t) parse_name(val, theme_names, COUNT(theme_names), cfg->theme);
        } else if (!strcasecmp(key, "screen_off_minutes")) {
            cfg->screen_off_minutes = parse_int(val, cfg->screen_off_minutes, 0, 600);
        } else if (!strcasecmp(key, "vertical_mode")) {
            cfg->rotation = (rotation_mode_t) parse_name(val, rotation_names, COUNT(rotation_names), cfg->rotation);
        } else {
            platform_log("config: unknown key '%s'", key);
        }
    }
    fclose(f);
    /* files from before the resolution setting: 1920x1080 was the default and becomes
     * auto; a size the user picked is kept */
    if (!saw_resolution) {
        if (cfg->width == 1280 && cfg->height == 720) cfg->resolution = RESOLUTION_720P;
        else if (cfg->width != 1920 || cfg->height != 1080) cfg->resolution = RESOLUTION_CUSTOM;
    }
}

bool config_save(const config_t *cfg, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f,
            "# SwitchAirCast settings. The settings menu (- button) saves this file; other\n"
            "# changes take effect when SwitchAirCast restarts.\n"
            "\n"
            "# Name shown in the Screen Mirroring list. Leave empty to use the console nickname.\n"
            "name = %s\n"
            "\n"
            "# Language: auto (console language), en or es.\n"
            "language = %s\n"
            "\n"
            "# Colours: auto (black on the OLED model), dark or black.\n"
            "theme = %s\n"
            "\n"
            "# Turn the screen off after this many minutes of waiting or playing music (0 = never).\n"
            "screen_off_minutes = %d\n"
            "\n"
            "# Turn mirrored and cast video for holding the console upright:\n"
            "# off, auto (motion sensor, handheld only), left or right.\n"
            "vertical_mode = %s\n"
            "\n"
            "# Picture size requested from the iPhone/iPad/Mac: auto (1280x720 handheld,\n"
            "# 1920x1080 docked), 1080p, 720p, or custom (uses width and height below).\n"
            "# 720p is lighter on Wi-Fi; 1080p looks best on a TV.\n"
            "resolution = %s\n"
            "width = %d\n"
            "height = %d\n"
            "\n"
            "# Frame rate requested from the iPhone/iPad/Mac (30 is lighter on Wi-Fi).\n"
            "max_fps = %d\n"
            "\n"
            "# Play sound through the Switch.\n"
            "audio = %s\n"
            "\n"
            "# Accept videos sent with the AirPlay button in apps such as YouTube; the Switch\n"
            "# then streams the video itself and the phone becomes a remote.\n"
            "video_casting = %s\n"
            "\n"
            "# Accept videos and music from Android apps (BubbleUPnP, Web Video Caster...),\n"
            "# Windows (\"Cast to Device\") and other DLNA / UPnP apps.\n"
            "dlna = %s\n"
            "\n"
            "# Audio track for AirPlay video when a video has several languages:\n"
            "#   default  = the video's original/default track (what the phone plays)\n"
            "#   system   = the console's language when available\n"
            "#   es:en    = a list of language codes in order of preference\n"
            "video_language = %s\n"
            "\n"
            "# Use the Switch's hardware video decoder (turn off only for troubleshooting).\n"
            "hw_decode = %s\n"
            "\n"
            "# Show resolution / frame rate / decoder info on screen (toggle with X).\n"
            "show_stats = %s\n"
            "\n"
            "# TCP port for the AirPlay service.\n"
            "port = %d\n"
            "\n"
            "# Write detailed protocol messages to log.txt (for troubleshooting). Also keeps the\n"
            "# last 10 runs as logs/log-<date>_<time>.txt.\n"
            "debug_log = %s\n",
            cfg->name, language_names[cfg->language], theme_names[cfg->theme], cfg->screen_off_minutes,
            rotation_names[cfg->rotation], resolution_names[cfg->resolution], cfg->width, cfg->height, cfg->max_fps, cfg->audio ? "true" : "false",
            cfg->video_casting ? "true" : "false", cfg->dlna ? "true" : "false", cfg->video_language, cfg->hw_decode ? "true" : "false",
            cfg->show_stats ? "true" : "false", cfg->port, cfg->debug_log ? "true" : "false");
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}
