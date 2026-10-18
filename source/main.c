/*
 * SwitchAirCast - an AirPlay screen mirroring and audio receiver for the Nintendo Switch
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "audio.h"
#include "config.h"
#include "i18n.h"
#include "platform.h"
#include "player.h"
#include "receiver.h"
#include "ui.h"
#include "video.h"

/* SDL joystick button numbers on the Switch */
enum {
    JOY_A = 0, JOY_B = 1, JOY_X = 2, JOY_Y = 3,
    JOY_L = 6, JOY_R = 7,
    JOY_PLUS = 10, JOY_MINUS = 11,
    JOY_DLEFT = 12, JOY_DUP = 13, JOY_DRIGHT = 14, JOY_DDOWN = 15,
};

typedef enum {
    ACT_NONE, ACT_OTHER, ACT_QUIT, ACT_SETTINGS, ACT_STATS, ACT_SCREEN_OFF, ACT_ROTATE,
    ACT_BACK, ACT_SELECT, ACT_LEFT, ACT_RIGHT, ACT_UP, ACT_DOWN,
} action_t;

/* how long the playback controls stay up after a button press */
#define OSD_MS 3000
#define SEEK_STEP 10.0
#define SEEK_STEP_BIG 60.0

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;

static SDL_Texture *video_tex = NULL;
static int video_w = 0, video_h = 0;
static Uint32 video_fmt = 0;
static int video_range = -1;
static struct SwsContext *sws = NULL;
static AVFrame *converted = NULL;

static SDL_Texture *coverart_tex = NULL;
static unsigned coverart_serial = 0;

static unsigned long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long) ts.tv_sec * 1000UL + (unsigned long) (ts.tv_nsec / 1000000L);
}

static void destroy_video_texture(void) {
    if (video_tex) SDL_DestroyTexture(video_tex);
    video_tex = NULL;
    video_w = video_h = 0;
    video_fmt = 0;
    video_range = -1;
}

/* Converts any decoder output we cannot upload directly to YUV420P. */
static AVFrame *to_yuv420p(AVFrame *frame) {
    if (!converted) converted = av_frame_alloc();
    if (!converted) return NULL;
    if (converted->width != frame->width || converted->height != frame->height) {
        av_frame_unref(converted);
        converted->format = AV_PIX_FMT_YUV420P;
        converted->width = frame->width;
        converted->height = frame->height;
        if (av_frame_get_buffer(converted, 0) < 0) return NULL;
    }
    sws = sws_getCachedContext(sws, frame->width, frame->height, (enum AVPixelFormat) frame->format,
                               frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) return NULL;
    sws_scale(sws, (const uint8_t *const *) frame->data, frame->linesize, 0, frame->height,
              converted->data, converted->linesize);
    converted->color_range = frame->color_range;
    return converted;
}

static void upload_video_frame(AVFrame *frame) {
    AVFrame *src = frame;
    Uint32 fmt;
    if (frame->format == AV_PIX_FMT_NV12) {
        fmt = SDL_PIXELFORMAT_NV12;
    } else if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
        fmt = SDL_PIXELFORMAT_IYUV;
    } else {
        src = to_yuv420p(frame);
        if (!src) return;
        fmt = SDL_PIXELFORMAT_IYUV;
    }
    int full_range = (frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P);

    if (!video_tex || video_w != src->width || video_h != src->height || video_fmt != fmt ||
        video_range != full_range) {
        destroy_video_texture();
        /* SDL picks the YUV matrix when the texture is created */
        SDL_SetYUVConversionMode(full_range ? SDL_YUV_CONVERSION_JPEG : SDL_YUV_CONVERSION_BT709);
        video_tex = SDL_CreateTexture(renderer, fmt, SDL_TEXTUREACCESS_STREAMING, src->width, src->height);
        if (!video_tex) {
            platform_log("main: SDL_CreateTexture(%dx%d) failed: %s", src->width, src->height, SDL_GetError());
            return;
        }
        video_w = src->width;
        video_h = src->height;
        video_fmt = fmt;
        video_range = full_range;
        platform_log("main: video texture %dx%d %s %s range", video_w, video_h,
                     fmt == SDL_PIXELFORMAT_NV12 ? "NV12" : "I420", full_range ? "full" : "limited");
    }
    if (fmt == SDL_PIXELFORMAT_NV12) {
        SDL_UpdateNVTexture(video_tex, NULL, src->data[0], src->linesize[0], src->data[1], src->linesize[1]);
    } else {
        SDL_UpdateYUVTexture(video_tex, NULL, src->data[0], src->linesize[0], src->data[1], src->linesize[1],
                             src->data[2], src->linesize[2]);
    }
}

/* Decodes JPEG/PNG cover art into an RGBA texture. */
static SDL_Texture *image_texture(const uint8_t *data, int len) {
    static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    bool png = len >= 8 && memcmp(data, png_sig, 8) == 0;
    const AVCodec *codec = avcodec_find_decoder(png ? AV_CODEC_ID_PNG : AV_CODEC_ID_MJPEG);
    if (!codec) return NULL;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    SDL_Texture *tex = NULL;
    if (!ctx || !pkt || !frame || avcodec_open2(ctx, codec, NULL) < 0 || av_new_packet(pkt, len) < 0) goto done;
    memcpy(pkt->data, data, (size_t) len);
    if (avcodec_send_packet(ctx, pkt) < 0 || avcodec_receive_frame(ctx, frame) < 0) goto done;

    struct SwsContext *conv = sws_getContext(frame->width, frame->height, (enum AVPixelFormat) frame->format,
                                             frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                             NULL, NULL, NULL);
    if (!conv) goto done;
    tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, frame->width, frame->height);
    uint8_t *rgba = malloc((size_t) frame->width * (size_t) frame->height * 4);
    if (tex && rgba) {
        uint8_t *dst[1] = { rgba };
        int dst_stride[1] = { frame->width * 4 };
        sws_scale(conv, (const uint8_t *const *) frame->data, frame->linesize, 0, frame->height, dst, dst_stride);
        SDL_UpdateTexture(tex, NULL, rgba, frame->width * 4);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    }
    free(rgba);
    sws_freeContext(conv);
done:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return tex;
}

#ifndef __SWITCH__
/* Desktop testing aid: SWITCHCAST_SNAPSHOT=<file.bmp> saves the screen once video
 * has been showing for two seconds. */
static void save_snapshot_if_requested(const ui_model_t *m) {
    static bool done = false;
    static unsigned long first_video_ms = 0;
    const char *path = getenv("SWITCHCAST_SNAPSHOT");
    if (done || !path || !m->video_tex) return;
    if (!first_video_ms) first_video_ms = m->now_ms;
    if (m->now_ms - first_video_ms < 2000) return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s && SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) {
        SDL_SaveBMP(s, path);
        platform_log("main: snapshot saved to %s", path);
    }
    SDL_FreeSurface(s);
    done = true;
}
#endif

static void update_coverart(const receiver_status_t *rx) {
    if (rx->coverart_serial == coverart_serial) return;
    coverart_serial = rx->coverart_serial;
    if (coverart_tex) SDL_DestroyTexture(coverart_tex);
    coverart_tex = NULL;
    uint8_t *data = NULL;
    int len = 0;
    if (receiver_get_coverart(&data, &len)) {
        coverart_tex = image_texture(data, len);
        free(data);
    }
}

/* ---- settings ---- */

static const int screen_off_choices[] = { 0, 1, 2, 5, 10, 15, 30 };
#define SCREEN_OFF_CHOICES ((int) (sizeof(screen_off_choices) / sizeof(screen_off_choices[0])))

static void apply_language(const config_t *cfg) {
    char tag[32];
    platform_get_language_tag(tag, sizeof(tag));
    lang_t system = i18n_from_tag(tag);
    switch (cfg->language) {
    case LANGUAGE_EN: i18n_set_language(LANG_EN); break;
    /* Spanish keeps the console's regional variant when it is Spanish already */
    case LANGUAGE_ES: i18n_set_language(system == LANG_ES_419 ? LANG_ES_419 : LANG_ES); break;
    default: i18n_set_language(system); break;
    }
}

static void apply_theme(const config_t *cfg) {
    bool black = cfg->theme == THEME_BLACK || (cfg->theme == THEME_AUTO && platform_is_oled());
    ui_set_black_theme(black);
}

static void fill_settings(ui_model_t *m, const config_t *cfg) {
    static const str_id_t language_values[] = { S_VAL_AUTOMATIC, S_VAL_ENGLISH, S_VAL_SPANISH };
    static const str_id_t theme_values[] = { S_VAL_AUTOMATIC, S_VAL_THEME_DARK, S_VAL_THEME_BLACK };
    static const str_id_t rotation_values[] = { S_VAL_OFF, S_VAL_ROTATION_AUTO, S_VAL_ROTATE_LEFT, S_VAL_ROTATE_RIGHT };
    m->settings_labels[0] = tr(S_SET_LANGUAGE);
    m->settings_labels[1] = tr(S_SET_THEME);
    m->settings_labels[2] = tr(S_SET_SCREEN_OFF);
    m->settings_labels[3] = tr(S_SET_ROTATION);
    snprintf(m->settings_values[0], sizeof(m->settings_values[0]), "%s", tr(language_values[cfg->language]));
    snprintf(m->settings_values[1], sizeof(m->settings_values[1]), "%s", tr(theme_values[cfg->theme]));
    if (cfg->screen_off_minutes > 0) {
        snprintf(m->settings_values[2], sizeof(m->settings_values[2]), tr(S_VAL_MINUTES), cfg->screen_off_minutes);
    } else {
        snprintf(m->settings_values[2], sizeof(m->settings_values[2]), "%s", tr(S_VAL_NEVER));
    }
    snprintf(m->settings_values[3], sizeof(m->settings_values[3]), "%s", tr(rotation_values[cfg->rotation]));
}

/* Steps setting `item` forwards (dir 1) or backwards (dir -1). */
static void change_setting(config_t *cfg, int item, int dir) {
    switch (item) {
    case 0:
        cfg->language = (ui_language_t) ((cfg->language + 3 + dir) % 3);
        apply_language(cfg);
        break;
    case 1:
        cfg->theme = (theme_t) ((cfg->theme + 3 + dir) % 3);
        apply_theme(cfg);
        break;
    case 2: {
        int k = 0;
        while (k < SCREEN_OFF_CHOICES - 1 && screen_off_choices[k] < cfg->screen_off_minutes) k++;
        k = (k + SCREEN_OFF_CHOICES + dir) % SCREEN_OFF_CHOICES;
        cfg->screen_off_minutes = screen_off_choices[k];
        break;
    }
    case 3:
        cfg->rotation = (rotation_mode_t) ((cfg->rotation + 4 + dir) % 4);
        break;
    }
}

static int rotation_degrees(const config_t *cfg) {
    switch (cfg->rotation) {
    case ROTATION_LEFT: return 270;
    case ROTATION_RIGHT: return 90;
    case ROTATION_AUTO: {
        int r = platform_motion_rotation();
        return r > 0 ? r : 0;
    }
    default: return 0;
    }
}

static void show_toast(ui_model_t *m, const char *text, unsigned long now) {
    m->toast = text;
    m->toast_ms = now;
}

int main(int argc, char **argv) {
    if (!platform_init()) {
        return 1;
    }
    platform_log("SwitchAirCast %s starting", SWITCHCAST_VERSION);

    config_t cfg;
    config_defaults(&cfg);
    char path[600];
    snprintf(path, sizeof(path), "%s/config.ini", platform_data_dir());
    config_load(&cfg, path);
    apply_language(&cfg);
    apply_theme(&cfg);

    char name[128];
    if (cfg.name[0]) {
        snprintf(name, sizeof(name), "%s", cfg.name);
    } else {
        platform_get_device_name(name, sizeof(name));
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
        platform_log("SDL_Init failed: %s", SDL_GetError());
        platform_exit();
        return 1;
    }
#ifdef __SWITCH__
    window = SDL_CreateWindow("SwitchAirCast", 0, 0, 1920, 1080, SDL_WINDOW_FULLSCREEN);
#else
    window = SDL_CreateWindow("SwitchAirCast", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
#endif
    renderer = window ? SDL_CreateRenderer(window, -1,
                                           SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC |
                                               SDL_RENDERER_TARGETTEXTURE)
                      : NULL;
    if (!renderer && window) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        platform_log("SDL window/renderer failed: %s", SDL_GetError());
        SDL_Quit();
        platform_exit();
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    for (int i = 0; i < SDL_NumJoysticks(); i++) SDL_JoystickOpen(i);

    bool ui_ok = ui_init(renderer);
    if (!video_init(cfg.hw_decode)) platform_log("main: video decoder init failed");
    if (cfg.audio && !audio_init()) cfg.audio = false;

    ui_model_t model;
    memset(&model, 0, sizeof(model));
    model.name = name;
    model.port = cfg.port;
    model.show_stats = cfg.show_stats;
    model.screen = UI_SCREEN_STARTING;

    char ip[64] = "-";
    char error[256] = "";
    bool started = false;
    unsigned long next_start_try = 0, next_ip_check = 0;
    receiver_state_t last_state = RECEIVER_WAITING;
    bool running = true;
    unsigned long last_activity = now_ms();
    unsigned wake_seen = platform_screen_wake_count();

    platform_set_keep_awake(true);

    while (running) {
        SDL_Event ev;
        bool retry = false;
        unsigned long now = now_ms();
        while (SDL_PollEvent(&ev)) {
            action_t act = ACT_NONE;
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_JOYBUTTONDOWN:
                switch (ev.jbutton.button) {
                case JOY_PLUS: act = ACT_QUIT; break;
                case JOY_MINUS: act = ACT_SETTINGS; break;
                case JOY_X: act = ACT_STATS; break;
                case JOY_Y: act = ACT_SCREEN_OFF; break;
                case JOY_B: act = ACT_BACK; break;
                case JOY_A: act = ACT_SELECT; break;
                case JOY_R: act = ACT_ROTATE; break;
                case JOY_DLEFT: act = ACT_LEFT; break;
                case JOY_DRIGHT: act = ACT_RIGHT; break;
                case JOY_DUP: act = ACT_UP; break;
                case JOY_DDOWN: act = ACT_DOWN; break;
                default: act = ACT_OTHER; break;
                }
                break;
            case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: case SDLK_q: act = ACT_QUIT; break;
                case SDLK_m: case SDLK_MINUS: act = ACT_SETTINGS; break;
                case SDLK_s: case SDLK_x: act = ACT_STATS; break;
                case SDLK_o: case SDLK_y: act = ACT_SCREEN_OFF; break;
                case SDLK_d: case SDLK_b: act = ACT_BACK; break;
                case SDLK_a: case SDLK_RETURN: case SDLK_SPACE: act = ACT_SELECT; break;
                case SDLK_r: act = ACT_ROTATE; break;
                case SDLK_LEFT: act = ACT_LEFT; break;
                case SDLK_RIGHT: act = ACT_RIGHT; break;
                case SDLK_UP: act = ACT_UP; break;
                case SDLK_DOWN: act = ACT_DOWN; break;
                default: act = ACT_OTHER; break;
                }
                break;
            case SDL_JOYDEVICEADDED:
                SDL_JoystickOpen(ev.jdevice.which);
                break;
            default:
                break;
            }
            if (act == ACT_NONE) continue;
            last_activity = now;

            /* the first press only turns the screen back on */
            if (model.screen_off) {
                model.screen_off = false;
                platform_set_backlight(true);
                continue;
            }

            if (model.settings_open) {
                switch (act) {
                case ACT_QUIT: running = false; break;
                case ACT_SETTINGS: case ACT_BACK: model.settings_open = false; break;
                case ACT_UP: model.settings_index = (model.settings_index + UI_SETTINGS_ITEMS - 1) % UI_SETTINGS_ITEMS; break;
                case ACT_DOWN: model.settings_index = (model.settings_index + 1) % UI_SETTINGS_ITEMS; break;
                case ACT_LEFT: case ACT_RIGHT: case ACT_SELECT:
                    change_setting(&cfg, model.settings_index, act == ACT_LEFT ? -1 : 1);
                    if (!config_save(&cfg, path)) platform_log("main: could not save %s", path);
                    break;
                default: break;
                }
                continue;
            }

            bool video = started && model.rx.state == RECEIVER_VIDEO;
            bool picture = started && (model.rx.state == RECEIVER_VIDEO || model.rx.state == RECEIVER_MIRRORING);
            switch (act) {
            case ACT_QUIT:
                running = false;
                break;
            case ACT_SETTINGS:
                model.settings_open = true;
                model.settings_index = 0;
                break;
            case ACT_SCREEN_OFF:
                model.screen_off = true;
                model.screen_off_ms = now;
                platform_set_backlight(false);
                break;
            case ACT_STATS:
                model.show_stats = !model.show_stats;
                break;
            case ACT_ROTATE:
                if (picture) {
                    /* cycle off -> left -> right -> off (auto counts as off here) */
                    cfg.rotation = cfg.rotation == ROTATION_LEFT ? ROTATION_RIGHT
                                 : cfg.rotation == ROTATION_RIGHT ? ROTATION_OFF : ROTATION_LEFT;
                    config_save(&cfg, path);
                    show_toast(&model, tr(cfg.rotation == ROTATION_LEFT ? S_ROTATION_TOAST_LEFT
                                          : cfg.rotation == ROTATION_RIGHT ? S_ROTATION_TOAST_RIGHT
                                                                           : S_ROTATION_TOAST_OFF), now);
                }
                break;
            case ACT_BACK:
                if (video) receiver_stop_video();
                else if (started) receiver_disconnect();
                break;
            case ACT_SELECT:
                if (video) {
                    player_set_paused(!model.player.paused);
                    model.osd_until_ms = now + OSD_MS;
                } else {
                    retry = true;
                }
                break;
            case ACT_LEFT: case ACT_RIGHT: case ACT_UP: case ACT_DOWN:
                if (video && model.player.state == PLAYER_PLAYING) {
                    double step = (act == ACT_LEFT || act == ACT_RIGHT) ? SEEK_STEP : SEEK_STEP_BIG;
                    double dir = (act == ACT_LEFT || act == ACT_DOWN) ? -1.0 : 1.0;
                    player_seek(model.player.position + dir * step);
                    model.osd_until_ms = now + OSD_MS;
                }
                break;
            default:
                break;
            }
        }
        model.now_ms = now;

        if (now >= next_ip_check) {
            next_ip_check = now + 1000;
            platform_get_ip(ip, sizeof(ip));
        }
        model.ip = ip;

        if (!started) {
            if (!strcmp(ip, "-")) {
                model.screen = UI_SCREEN_NO_NETWORK;
            } else if (now >= next_start_try || retry) {
                if (receiver_start(&cfg, name, error, sizeof(error))) {
                    started = true;
                    model.screen = UI_SCREEN_READY;
                } else {
                    platform_log("main: %s", error);
                    model.screen = UI_SCREEN_ERROR;
                    model.error = error;
                    next_start_try = now + 10000;
                }
            }
        } else {
            receiver_poll();
            receiver_get_status(&model.rx);
            video_get_stats(&model.video);
            update_coverart(&model.rx);

            player_get_status(&model.player);

            if (model.rx.state == RECEIVER_MIRRORING) {
                if (last_state != RECEIVER_MIRRORING) model.mirror_started_ms = now;
                AVFrame *frame = video_take_frame();
                if (frame) {
                    upload_video_frame(frame);
                    av_frame_free(&frame);
                }
            } else if (model.rx.state == RECEIVER_VIDEO) {
                if (last_state != RECEIVER_VIDEO) {
                    /* a new video: drop the last picture of whatever played before */
                    destroy_video_texture();
                    model.mirror_started_ms = now;
                }
                AVFrame *frame = video_take_due_frame(player_clock_us());
                if (frame) {
                    upload_video_frame(frame);
                    av_frame_free(&frame);
                }
            } else if (video_tex) {
                destroy_video_texture();
                model.mirror_started_ms = 0;
            }

            if (model.rx.state != last_state) {
                last_activity = now;
                /* new mirroring or video: show it even if the screen was turned off */
                if (model.screen_off &&
                    (model.rx.state == RECEIVER_MIRRORING || model.rx.state == RECEIVER_VIDEO)) {
                    model.screen_off = false;
                    platform_set_backlight(true);
                }
            }
            last_state = model.rx.state;
        }

        /* the system turned the panel back on (HOME menu, wake-up, dock change) */
        unsigned wakes = platform_screen_wake_count();
        if (wakes != wake_seen) {
            wake_seen = wakes;
            model.screen_off = false;
            last_activity = now;
        }
        /* turn the screen off after a while of waiting or music, never during a picture */
        bool picture = model.rx.state == RECEIVER_MIRRORING || model.rx.state == RECEIVER_VIDEO;
        if (!model.screen_off && cfg.screen_off_minutes > 0 && !model.settings_open && !picture &&
            now - last_activity > (unsigned long) cfg.screen_off_minutes * 60000UL) {
            platform_log("main: turning the screen off after %d min", cfg.screen_off_minutes);
            model.screen_off = true;
            model.screen_off_ms = now;
            platform_set_backlight(false);
        }

        model.rotation = rotation_degrees(&cfg);
        if (model.settings_open) fill_settings(&model, &cfg);
        model.video_tex = video_tex;
        model.video_w = video_w;
        model.video_h = video_h;
        model.coverart_tex = coverart_tex;

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (ui_ok) ui_draw(&model);
#ifndef __SWITCH__
        save_snapshot_if_requested(&model);
#endif
        SDL_RenderPresent(renderer);
    }

    platform_log("SwitchAirCast exiting");
    platform_set_backlight(true);
    if (started) receiver_stop();
    audio_shutdown();
    video_shutdown();
    destroy_video_texture();
    if (coverart_tex) SDL_DestroyTexture(coverart_tex);
    av_frame_free(&converted);
    sws_freeContext(sws);
    if (ui_ok) ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    platform_exit();
    return 0;
}
