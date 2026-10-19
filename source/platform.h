/*
 * SwitchAirCast - platform layer (Nintendo Switch via libnx, or a Linux desktop
 * build used for development and testing).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SWITCHCAST_PLATFORM_H
#define SWITCHCAST_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Brings up sockets, network status, settings and font services. */
bool platform_init(void);
void platform_exit(void);

/* Current IPv4 address as dotted quad; false when not connected to a network. */
bool platform_get_ip(char *buf, size_t len);

/* Console nickname (e.g. "Alex's Switch"), or a generic fallback. */
void platform_get_device_name(char *buf, size_t len);

/* Directory for config.ini, the persistent device id and the log file (no trailing slash). */
const char *platform_data_dir(void);

/* A TrueType/OpenType font in memory for the UI. The data stays valid until platform_exit(). */
bool platform_get_font(const void **data, size_t *size);

/* Preferred languages for media playback as a ':'-separated list of ISO 639-1 codes,
 * system language first (e.g. "es:en"). */
void platform_get_languages(char *buf, size_t len);

/* System language tag such as "es-419" or "en-US". */
void platform_get_language_tag(char *buf, size_t len);

/* True on the OLED model. */
bool platform_is_oled(void);

/* True when started from the Album (applet mode), which leaves too little memory to
 * decode video. The desktop build pretends so when SWITCHCAST_APPLET is set. */
bool platform_is_applet_mode(void);

/* True when the console's own screen is the display (handheld mode). */
bool platform_is_handheld(void);

/* Turns the built-in screen's backlight off or on (handheld mode only). It comes back
 * on by itself when the app leaves the foreground, the console wakes from sleep or is
 * docked/undocked; platform_screen_wake_count() increases each time that happens. */
void platform_set_backlight(bool on);
unsigned platform_screen_wake_count(void);

/* How content should be rotated for the way the console is held, from the motion
 * sensor in handheld mode: 0, 90 (clockwise) or 270 degrees; -1 if unknown. */
int platform_motion_rotation(void);

/* Keeps the screen from dimming and the console from going to sleep. */
void platform_set_keep_awake(bool on);

/* Fills buf with cryptographically random bytes. */
void platform_random(void *buf, size_t len);

void platform_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Debug logging: also writes this run's log to logs/log-<date>_<time>.txt in the data
 * folder and deletes the oldest of those so that at most `keep` remain. */
void platform_keep_log_history(int keep);

#endif
