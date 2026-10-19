/*
 * SwitchAirCast - user interface text in English and Spanish
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SWITCHCAST_I18N_H
#define SWITCHCAST_I18N_H

typedef enum {
    LANG_EN,
    LANG_ES,        /* Spain */
    LANG_ES_419,    /* Latin America */
} lang_t;

typedef enum {
    S_APP_SUBTITLE,
    S_EXIT,
    S_SETTINGS,
    S_SCREEN_OFF,
    S_STATS,
    S_HIDE_STATS,
    S_DISCONNECT,
    S_TRY_AGAIN,
    S_ROTATE,
    S_STARTING,
    S_NO_NETWORK_TITLE,
    S_NO_NETWORK_LINE1,
    S_NO_NETWORK_LINE2,
    S_START_ERROR_TITLE,
    S_APPLET_TITLE,
    S_APPLET_LINE1,
    S_APPLET_LINE2,
    S_ERR_DISCOVERY,          /* %d */
    S_ERR_SERVER,
    S_ERR_PORT,
    S_ERR_ADVERTISE,          /* %d */
    S_READY_WAITING,
    S_CONNECTED_TO,           /* %s */
    S_A_DEVICE,
    S_YOUR_DEVICE,
    S_IP_ADDRESS,             /* %s */
    S_STEP_WIFI,
    S_STEP_CONTROL_CENTER,
    S_STEP_DLNA,
    S_STEP_CHOOSE,            /* %s */
    S_PLAYING_FROM,           /* %s */
    S_PLAYING_AUDIO,
    S_STARTING_MIRROR_FROM,   /* %s */
    S_MIRRORING_FROM,         /* %s */
    S_LOADING_VIDEO,
    S_LOADING_VIDEO_FROM,     /* %s */
    S_PLAYING_VIDEO,
    S_PLAYING_VIDEO_FROM,     /* %s */
    S_VIDEO_FAILED_TITLE,
    S_VIDEO_FAILED_LINE1,
    S_VIDEO_FAILED_LINE2,
    S_PLAY,
    S_PAUSE,
    S_STOP,
    S_SECONDS_10,
    S_MINUTE_1,
    S_HARDWARE,
    S_SOFTWARE,
    S_MS_PER_FRAME,           /* %.1f */
    S_FRAMES_SHOWN,           /* %lu %s */
    S_AUDIO_ON,
    S_AUDIO_OFF,
    S_SET_LANGUAGE,
    S_SET_THEME,
    S_SET_SCREEN_OFF,
    S_SET_ROTATION,
    S_SET_RESOLUTION,
    S_SET_FRAME_RATE,
    S_VAL_AUTOMATIC,
    S_VAL_ENGLISH,
    S_VAL_SPANISH,
    S_VAL_THEME_DARK,
    S_VAL_THEME_BLACK,
    S_VAL_NEVER,
    S_VAL_MINUTES,            /* %d */
    S_VAL_OFF,
    S_VAL_ROTATION_AUTO,
    S_VAL_ROTATE_LEFT,
    S_VAL_ROTATE_RIGHT,
    S_VAL_RESOLUTION_AUTO,    /* %d %d */
    S_CHANGE,
    S_CLOSE,
    S_SETTINGS_NOTE,
    S_SETTINGS_NOTE_LANGUAGE,
    S_SETTINGS_NOTE_THEME,
    S_SETTINGS_NOTE_ROTATION,
    S_SETTINGS_NOTE_RESOLUTION,
    S_SETTINGS_NOTE_VIDEO,
    S_SCREEN_OFF_TOAST,
    S_ROTATION_TOAST_OFF,
    S_ROTATION_TOAST_LEFT,
    S_ROTATION_TOAST_RIGHT,
    S_PIN_TITLE,
    S_PIN_HINT,
    S_HINT_PRESS,
    S_HINT_OPEN_SETTINGS,
    S_VERSION_BY,             /* %s version, %s author */
    S_COUNT
} str_id_t;

void i18n_set_language(lang_t lang);
lang_t i18n_language(void);

/* Language from a system language list such as "es:en" or a tag like "es-419". */
lang_t i18n_from_tag(const char *tag);

const char *tr(str_id_t id);

#endif
