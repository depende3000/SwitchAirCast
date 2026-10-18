/*
 * SwitchAirCast - on-screen interface (SDL2 renderer + SDL_ttf)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SWITCHCAST_UI_H
#define SWITCHCAST_UI_H

#include <stdbool.h>

#include <SDL.h>

#include "player.h"
#include "receiver.h"
#include "video.h"

typedef enum {
    UI_SCREEN_STARTING,
    UI_SCREEN_NO_NETWORK,
    UI_SCREEN_ERROR,
    UI_SCREEN_READY,        /* waiting / connected / now playing / mirroring */
} ui_screen_t;

#define UI_SETTINGS_ITEMS 4

typedef struct {
    ui_screen_t screen;
    const char *name;            /* advertised AirPlay name */
    const char *ip;
    int port;
    const char *error;           /* UI_SCREEN_ERROR */
    receiver_status_t rx;
    video_stats_t video;
    bool show_stats;
    SDL_Texture *video_tex;      /* current mirrored frame, NULL if none yet */
    int video_w, video_h;
    SDL_Texture *coverart_tex;   /* music cover art, NULL if none */
    unsigned long mirror_started_ms;  /* when mirroring/video began (for the toast), 0 if not */
    unsigned long now_ms;
    player_status_t player;          /* AirPlay video playback */
    unsigned long osd_until_ms;      /* keep the playback controls visible until then */

    int rotation;                    /* 0, 90 or 270: how mirrored/cast video is turned */
    bool screen_off;                 /* show nothing (the backlight is off in handheld mode) */
    unsigned long screen_off_ms;     /* when the screen went off */
    const char *toast;               /* short message (e.g. after rotating), or NULL */
    unsigned long toast_ms;          /* when it was set */

    /* settings menu */
    bool settings_open;
    int settings_index;
    const char *settings_labels[UI_SETTINGS_ITEMS];
    char settings_values[UI_SETTINGS_ITEMS][64];
} ui_model_t;

bool ui_init(SDL_Renderer *renderer);
void ui_shutdown(void);

/* Pure black instead of the blue-grey gradient (OLED screens). */
void ui_set_black_theme(bool black);

/* Forgets rendered text (after the language changes). */
void ui_clear_text_cache(void);

void ui_draw(const ui_model_t *m);
void ui_draw_app_icon(int size);

#endif
