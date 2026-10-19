/*
 * Renders every SwitchAirCast screen offscreen and writes them as BMP files, so the
 * layout can be checked without a Switch:
 *
 *   SDL_VIDEODRIVER=offscreen build/linux/ui_preview <outdir> [width height] [es|es-419] [black]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "i18n.h"
#include "platform.h"
#include "ui.h"

static SDL_Renderer *ren;

static void save(const char *dir, const char *name, int w, int h) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.bmp", dir, name);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    printf("wrote %s\n", path);
}

static SDL_Texture *gradient_texture(int w, int h, int phase) {
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    Uint32 *px = malloc((size_t) w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int r = (x * 255 / w + phase) & 255, g = (y * 255 / h) & 255, b = ((x + y) * 128 / (w + h) + 90) & 255;
            if (((x / 40) + (y / 40)) % 2 == 0) { r = r * 3 / 4; g = g * 3 / 4; }
            px[y * w + x] = 0xff000000u | (Uint32) (r << 16) | (Uint32) (g << 8) | (Uint32) b;
        }
    }
    SDL_UpdateTexture(t, NULL, px, w * 4);
    free(px);
    return t;
}

static void frame(const ui_model_t *m, const char *dir, const char *name, int w, int h) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    ui_draw(m);
    save(dir, name, w, h);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    int w = argc > 3 ? atoi(argv[2]) : 1280, h = argc > 3 ? atoi(argv[3]) : 720;
    for (int i = 4; i < argc; i++) {
        if (!strncmp(argv[i], "es", 2)) i18n_set_language(i18n_from_tag(argv[i]));
        if (!strcmp(argv[i], "black")) ui_set_black_theme(true);
    }
    platform_init();
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *win = SDL_CreateWindow("preview", 0, 0, w, h, SDL_WINDOW_HIDDEN);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren || !ui_init(ren)) {
        fprintf(stderr, "init failed: %s\n", SDL_GetError());
        return 1;
    }

    ui_model_t m;
    memset(&m, 0, sizeof(m));
    m.name = "Alex\xE2\x80\x99s Switch";
    m.ip = "192.168.1.42";
    m.port = 7000;
    m.dlna = true;
    m.now_ms = 123456;

    m.screen = UI_SCREEN_STARTING;
    frame(&m, dir, "1_starting", w, h);
    m.screen = UI_SCREEN_NO_NETWORK;
    frame(&m, dir, "2_no_network", w, h);
    m.screen = UI_SCREEN_ERROR;
    m.error = "Could not open a network port for AirPlay.";
    frame(&m, dir, "3_error", w, h);
    m.screen = UI_SCREEN_APPLET_MODE;
    frame(&m, dir, "3b_applet_mode", w, h);

    m.screen = UI_SCREEN_READY;
    m.rx.state = RECEIVER_WAITING;
    frame(&m, dir, "4_ready", w, h);
    m.dlna = false;
    frame(&m, dir, "4b_ready_no_dlna", w, h);
    m.dlna = true;

    m.rx.state = RECEIVER_CONNECTED;
    strcpy(m.rx.client_name, "Alex\xE2\x80\x99s iPhone");
    frame(&m, dir, "5_connected", w, h);

    m.rx.audio_playing = true;
    strcpy(m.rx.title, "A Very Long Song Title That Keeps Going And Going Forever");
    strcpy(m.rx.artist, "The Example Band");
    strcpy(m.rx.album, "Greatest Hits (Deluxe Edition)");
    m.rx.has_progress = true;
    m.rx.position = 83;
    m.rx.duration = 225;
    frame(&m, dir, "6_now_playing_noart", w, h);
    m.coverart_tex = gradient_texture(600, 600, 0);
    frame(&m, dir, "7_now_playing", w, h);
    m.coverart_tex = NULL;

    m.rx.state = RECEIVER_MIRRORING;
    m.rx.audio_playing = true;
    frame(&m, dir, "8_mirroring_wait", w, h);
    m.video_tex = gradient_texture(886, 1920, 60);
    m.video_w = 886;
    m.video_h = 1920;
    m.mirror_started_ms = m.now_ms - 1000;
    frame(&m, dir, "9_mirroring_portrait", w, h);
    m.video_tex = gradient_texture(1920, 886, 120);
    m.video_w = 1920;
    m.video_h = 886;
    m.mirror_started_ms = 0;
    m.show_stats = true;
    m.video.width = 1920;
    m.video.height = 886;
    m.video.fps = 59.8f;
    m.video.hw = true;
    m.video.decode_ms = 3.2f;
    m.video.frames = 12345;
    m.video.dropped = 12;
    m.video.shown_percent = 99.6f;
    frame(&m, dir, "10_mirroring_stats", w, h);

    /* AirPlay video */
    m.show_stats = false;
    m.rx.state = RECEIVER_VIDEO;
    m.rx.client_name[0] = '\0';
    m.video_tex = NULL;
    m.player.state = PLAYER_LOADING;
    frame(&m, dir, "11_video_loading", w, h);
    m.video_tex = gradient_texture(1920, 1080, 30);
    m.video_w = 1920;
    m.video_h = 1080;
    m.player.state = PLAYER_PLAYING;
    m.player.has_video = true;
    m.player.paused = true;
    m.player.position = 754;
    m.player.duration = 1265;
    m.mirror_started_ms = m.now_ms - 500;
    frame(&m, dir, "12_video_paused", w, h);
    m.player.state = PLAYER_ENDED;
    m.player.failed = true;
    frame(&m, dir, "13_video_failed", w, h);

    /* music sent over DLNA: no picture, the now-playing screen with the player's controls */
    m.player.failed = false;
    m.player.state = PLAYER_PLAYING;
    m.player.has_video = false;
    m.player.paused = true;
    m.player.position = 83;
    m.player.duration = 225;
    m.video_tex = NULL;
    m.mirror_started_ms = 0;
    strcpy(m.rx.client_name, "BubbleUPnP");
    m.rx.has_progress = true;
    m.rx.position = 83;
    m.rx.duration = 225;
    m.coverart_tex = gradient_texture(600, 600, 200);
    frame(&m, dir, "13b_dlna_music_paused", w, h);
    m.coverart_tex = NULL;
    m.rx.client_name[0] = '\0';

    /* settings menu over the ready screen */
    m.rx.state = RECEIVER_WAITING;
    m.video_tex = NULL;
    m.player.state = PLAYER_IDLE;
    m.settings_open = true;
    m.settings_index = 2;
    m.settings_labels[0] = tr(S_SET_LANGUAGE);
    m.settings_labels[1] = tr(S_SET_THEME);
    m.settings_labels[2] = tr(S_SET_SCREEN_OFF);
    m.settings_labels[3] = tr(S_SET_ROTATION);
    m.settings_labels[4] = tr(S_SET_RESOLUTION);
    m.settings_labels[5] = tr(S_SET_FRAME_RATE);
    snprintf(m.settings_values[0], 64, "%s", tr(S_VAL_AUTOMATIC));
    snprintf(m.settings_values[1], 64, "%s", tr(S_VAL_THEME_BLACK));
    snprintf(m.settings_values[2], 64, tr(S_VAL_MINUTES), 5);
    snprintf(m.settings_values[3], 64, "%s", tr(S_VAL_ROTATION_AUTO));
    snprintf(m.settings_values[4], 64, tr(S_VAL_RESOLUTION_AUTO), 1280, 720);
    snprintf(m.settings_values[5], 64, "60 fps");
    /* every row selected with its note (the same notes as main.c's settings menu) */
    static const str_id_t notes[UI_SETTINGS_ITEMS] = { S_SETTINGS_NOTE_LANGUAGE, S_SETTINGS_NOTE_THEME,
                                                       S_SETTINGS_NOTE, S_SETTINGS_NOTE_ROTATION,
                                                       S_SETTINGS_NOTE_RESOLUTION, S_SETTINGS_NOTE_VIDEO };
    for (int i = 0; i < UI_SETTINGS_ITEMS; i++) {
        char name[32];
        m.settings_index = i;
        m.settings_note = tr(notes[i]);
        snprintf(name, sizeof(name), "14_settings_%d", i);
        frame(&m, dir, name, w, h);
    }
    /* the longest values each row can show */
    snprintf(m.settings_values[0], 64, "%s", tr(S_VAL_SPANISH));
    snprintf(m.settings_values[1], 64, "%s", tr(S_VAL_THEME_DARK));
    snprintf(m.settings_values[2], 64, "%s", tr(S_VAL_NEVER));
    snprintf(m.settings_values[3], 64, "%s", tr(S_VAL_ROTATE_RIGHT));
    snprintf(m.settings_values[4], 64, tr(S_VAL_RESOLUTION_AUTO), 1920, 1080);
    snprintf(m.settings_values[5], 64, "30 fps");
    m.settings_index = 3;
    m.settings_note = tr(notes[3]);
    frame(&m, dir, "14_settings_long_values", w, h);
    m.settings_open = false;

    /* vertical mode: a portrait phone stream rotated to fill the screen */
    m.rx.state = RECEIVER_MIRRORING;
    strcpy(m.rx.client_name, "Alex\xE2\x80\x99s iPhone");
    m.video_tex = gradient_texture(498, 1080, 60);
    m.video_w = 498;
    m.video_h = 1080;
    m.rotation = 90;
    m.mirror_started_ms = m.now_ms - 1000;
    frame(&m, dir, "15_vertical_mirroring", w, h);
    m.rx.state = RECEIVER_VIDEO;
    m.video_tex = gradient_texture(1080, 1920, 20);
    m.video_w = 1080;
    m.video_h = 1920;
    m.rotation = 270;
    m.mirror_started_ms = 0;
    m.player.state = PLAYER_PLAYING;
    m.player.has_video = true;
    m.player.failed = false;
    m.player.paused = true;
    m.player.position = 42;
    m.player.duration = 58;
    frame(&m, dir, "16_vertical_video_paused", w, h);
    m.rotation = 0;

    m.screen_off = true;
    m.screen_off_ms = m.now_ms - 500;
    frame(&m, dir, "17_screen_off", w, h);
    m.screen_off = false;

    /* a Mac asking for its pairing code, over the mirrored picture */
    m.rx.state = RECEIVER_MIRRORING;
    strcpy(m.rx.client_name, "MacBook Air");
    m.video_tex = gradient_texture(1920, 1080, 90);
    m.video_w = 1920;
    m.video_h = 1080;
    strcpy(m.rx.pin, "3846");
    frame(&m, dir, "18_pin", w, h);
    m.rx.pin[0] = '\0';

    /* the console touched during a cast: the settings reminder, alone and under a toast */
    m.mirror_started_ms = 0;
    m.options_hint_ms = m.now_ms - 500;
    frame(&m, dir, "19_options_hint", w, h);
    m.mirror_started_ms = m.now_ms - 1000;
    frame(&m, dir, "20_options_hint_under_toast", w, h);

    /* every toast and start-up error, to catch text that does not fit */
    static const str_id_t toasts[] = { S_SCREEN_OFF_TOAST, S_ROTATION_TOAST_OFF, S_ROTATION_TOAST_LEFT,
                                       S_ROTATION_TOAST_RIGHT };
    m.options_hint_ms = 0;
    m.mirror_started_ms = 0;
    for (size_t i = 0; i < sizeof(toasts) / sizeof(toasts[0]); i++) {
        char name[32];
        m.toast = tr(toasts[i]);
        m.toast_ms = m.now_ms - 500;
        snprintf(name, sizeof(name), "22_toast_%zu", i);
        frame(&m, dir, name, w, h);
    }
    m.toast = NULL;
    static char errors[4][256];
    snprintf(errors[0], sizeof(errors[0]), tr(S_ERR_DISCOVERY), -65563);
    snprintf(errors[1], sizeof(errors[1]), "%s", tr(S_ERR_SERVER));
    snprintf(errors[2], sizeof(errors[2]), "%s", tr(S_ERR_PORT));
    snprintf(errors[3], sizeof(errors[3]), tr(S_ERR_ADVERTISE), -65563);
    m.rx.state = RECEIVER_WAITING;
    m.screen = UI_SCREEN_ERROR;
    for (int i = 0; i < 4; i++) {
        char name[32];
        m.error = errors[i];
        snprintf(name, sizeof(name), "23_error_%d", i);
        frame(&m, dir, name, w, h);
    }
    m.mirror_started_ms = 0;
    m.rotation = 90;
    m.video_tex = gradient_texture(498, 1080, 60);
    m.video_w = 498;
    m.video_h = 1080;
    frame(&m, dir, "21_options_hint_vertical", w, h);
    m.rotation = 0;
    m.options_hint_ms = 0;

    ui_shutdown();
    SDL_Quit();
    platform_exit();
    return 0;
}
