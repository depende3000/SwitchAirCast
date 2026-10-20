/*
 * SwitchAirCast - on-screen interface
 *
 * Layout coordinates are in a space whose shorter side is 720 units, scaled to the
 * output, so the same layout is sharp handheld (720p) and docked (1080p). Rotated
 * video (vertical mode) is drawn portrait into an offscreen texture and turned onto
 * the screen, so its overlays read correctly too.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL_ttf.h>

#include "i18n.h"
#include "platform.h"

#ifndef SWITCHCAST_VERSION
#define SWITCHCAST_VERSION "dev"
#endif

#define BASE_H 720.0f
#define TEXT_CACHE_SIZE 128
#define TEXT_MAX 192

typedef enum { F_TITLE, F_LARGE, F_HEADING, F_BODY, F_SMALL, F_HUGE, F_COUNT } font_id_t;
static const int font_base_px[F_COUNT] = { 46, 38, 28, 22, 17, 96 };

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

#define SWITCHCAST_AUTHOR "depende3000"

typedef struct {
    SDL_Color bg_top, bg_bottom, card, text, text_dim, divider, button;
} palette_t;

static const palette_t PALETTE_DARK = {
    { 12, 16, 27, 255 }, { 24, 30, 48, 255 }, { 32, 39, 60, 255 },
    { 240, 243, 248, 255 }, { 150, 161, 182, 255 }, { 255, 255, 255, 22 }, { 214, 220, 230, 255 },
};
/* true black lets OLED pixels switch off; cards stay just visible */
static const palette_t PALETTE_BLACK = {
    { 0, 0, 0, 255 }, { 0, 0, 0, 255 }, { 21, 21, 24, 255 },
    { 232, 232, 236, 255 }, { 138, 138, 148, 255 }, { 255, 255, 255, 26 }, { 196, 196, 204, 255 },
};
static const palette_t *pal = &PALETTE_DARK;

#define COL_BG_TOP (pal->bg_top)
#define COL_BG_BOTTOM (pal->bg_bottom)
#define COL_CARD (pal->card)
#define COL_TEXT (pal->text)
#define COL_TEXT_DIM (pal->text_dim)
#define COL_DIVIDER (pal->divider)
#define COL_BUTTON (pal->button)
static const SDL_Color COL_ACCENT = { 64, 156, 255, 255 };
static const SDL_Color COL_GREEN = { 52, 199, 89, 255 };
static const SDL_Color COL_AMBER = { 255, 176, 32, 255 };
static const SDL_Color COL_RED = { 255, 82, 72, 255 };
static const SDL_Color COL_BLACK = { 0, 0, 0, 255 };
static const SDL_Color COL_WHITE = { 255, 255, 255, 255 };

typedef struct {
    SDL_Texture *tex;
    font_id_t font;
    char text[TEXT_MAX];
    int w, h;
    unsigned last_used;
} text_entry_t;

static SDL_Renderer *ren = NULL;
static const void *font_data = NULL;
static size_t font_size = 0;
static TTF_Font *fonts[F_COUNT];
static int fonts_built_for = 0;
static float scale = 1.0f;
static int out_w = 0, out_h = 0;
static float base_w = 1280.0f, base_h = BASE_H;
/* small periodic offset of static screens so OLED pixels do not burn in */
static float shift_x = 0.0f, shift_y = 0.0f;
static text_entry_t cache[TEXT_CACHE_SIZE];
static unsigned frame_no = 0;
static SDL_Texture *rotate_target = NULL;
static int rotate_w = 0, rotate_h = 0;

/* ---- text ---- */

void ui_clear_text_cache(void) {
    for (int i = 0; i < TEXT_CACHE_SIZE; i++) {
        if (cache[i].tex) SDL_DestroyTexture(cache[i].tex);
        memset(&cache[i], 0, sizeof(cache[i]));
    }
}

static void close_fonts(void) {
    for (int i = 0; i < F_COUNT; i++) {
        if (fonts[i]) TTF_CloseFont(fonts[i]);
        fonts[i] = NULL;
    }
}

static bool build_fonts(void) {
    int key = (int) (scale * 1000.0f + 0.5f);
    if (fonts_built_for == key && fonts[0]) return true;
    ui_clear_text_cache();
    close_fonts();
    for (int i = 0; i < F_COUNT; i++) {
        int px = (int) (font_base_px[i] * scale + 0.5f);
        fonts[i] = TTF_OpenFontRW(SDL_RWFromConstMem(font_data, (int) font_size), 1, px);
        if (!fonts[i]) {
            platform_log("ui: TTF_OpenFontRW failed: %s", TTF_GetError());
            return false;
        }
    }
    fonts_built_for = key;
    return true;
}

static text_entry_t *get_text(font_id_t f, const char *text) {
    if (!text || !*text || !fonts[f]) return NULL;
    text_entry_t *slot = NULL;
    for (int i = 0; i < TEXT_CACHE_SIZE; i++) {
        text_entry_t *e = &cache[i];
        if (e->tex && e->font == f && strncmp(e->text, text, TEXT_MAX - 1) == 0) {
            e->last_used = frame_no;
            return e;
        }
        if (!e->tex) {
            if (!slot || slot->tex) slot = e;
        } else if (!slot || (slot->tex && e->last_used < slot->last_used)) {
            slot = e;
        }
    }
    if (slot->tex) SDL_DestroyTexture(slot->tex);
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->text, sizeof(slot->text), "%s", text);
    SDL_Surface *s = TTF_RenderUTF8_Blended(fonts[f], slot->text, COL_WHITE);
    if (!s) return NULL;
    slot->tex = SDL_CreateTextureFromSurface(ren, s);
    slot->w = s->w;
    slot->h = s->h;
    SDL_FreeSurface(s);
    if (!slot->tex) return NULL;
    SDL_SetTextureBlendMode(slot->tex, SDL_BLENDMODE_BLEND);
    slot->font = f;
    slot->last_used = frame_no;
    return slot;
}

/* width of text in layout units */
static float text_width(font_id_t f, const char *text) {
    int w = 0, h = 0;
    if (!text || !*text || !fonts[f] || TTF_SizeUTF8(fonts[f], text, &w, &h)) return 0.0f;
    return (float) w / scale;
}

/* Draws text with its top-left (or top-center / top-right) at (x, y). Returns its width. */
static float draw_text(font_id_t f, const char *text, float x, float y, SDL_Color c, int align) {
    text_entry_t *e = get_text(f, text);
    if (!e) return 0.0f;
    float w = (float) e->w / scale;
    if (align == ALIGN_CENTER) x -= w / 2.0f;
    else if (align == ALIGN_RIGHT) x -= w;
    SDL_Rect dst = { (int) ((x + shift_x) * scale + 0.5f), (int) ((y + shift_y) * scale + 0.5f), e->w, e->h };
    SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(e->tex, c.a);
    SDL_RenderCopy(ren, e->tex, NULL, &dst);
    return w;
}

/* text vertically centered on cy */
static float draw_text_mid(font_id_t f, const char *text, float x, float cy, SDL_Color c, int align) {
    return draw_text(f, text, x, cy - (float) font_base_px[f] * 0.62f, c, align);
}

/* Shortens text with an ellipsis so it fits max_w layout units. */
static void fit_text(font_id_t f, const char *in, float max_w, char *out, size_t out_len) {
    snprintf(out, out_len, "%s", in ? in : "");
    if (text_width(f, out) <= max_w) return;
    size_t n = strlen(out);
    while (n > 0) {
        /* step back one UTF-8 character */
        do { n--; } while (n > 0 && ((unsigned char) out[n] & 0xC0) == 0x80);
        char tmp[TEXT_MAX + 4];
        snprintf(tmp, sizeof(tmp), "%.*s\xE2\x80\xA6", (int) n, out);
        if (text_width(f, tmp) <= max_w || n == 0) {
            snprintf(out, out_len, "%s", tmp);
            return;
        }
    }
}

/* ---- shapes ---- */

static SDL_Vertex vtx(float x, float y, SDL_Color c) {
    SDL_Vertex v;
    v.position.x = (x + shift_x) * scale;
    v.position.y = (y + shift_y) * scale;
    v.color = c;
    v.tex_coord.x = 0.0f;
    v.tex_coord.y = 0.0f;
    return v;
}

static void fill_rect(float x, float y, float w, float h, SDL_Color c) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_FRect r = { (x + shift_x) * scale, (y + shift_y) * scale, w * scale, h * scale };
    SDL_RenderFillRectF(ren, &r);
}

#define CORNER_SEGMENTS 8

static void fill_round_rect(float x, float y, float w, float h, float r, SDL_Color c) {
    if (r * 2.0f > w) r = w / 2.0f;
    if (r * 2.0f > h) r = h / 2.0f;
    SDL_Vertex v[4 * (CORNER_SEGMENTS + 1) + 1];
    int indices[4 * (CORNER_SEGMENTS + 1) * 3];
    const float cx[4] = { x + w - r, x + r, x + r, x + w - r };
    const float cy[4] = { y + r, y + r, y + h - r, y + h - r };
    int n = 0;
    v[n++] = vtx(x + w / 2.0f, y + h / 2.0f, c);
    for (int k = 0; k < 4; k++) {
        for (int s = 0; s <= CORNER_SEGMENTS; s++) {
            float a = (float) M_PI / 2.0f * ((float) k + (float) s / CORNER_SEGMENTS);
            v[n++] = vtx(cx[k] + cosf(a) * r, cy[k] - sinf(a) * r, c);
        }
    }
    int outline = n - 1, ni = 0;
    for (int i = 0; i < outline; i++) {
        indices[ni++] = 0;
        indices[ni++] = 1 + i;
        indices[ni++] = 1 + (i + 1) % outline;
    }
    SDL_RenderGeometry(ren, NULL, v, n, indices, ni);
}

static void fill_circle(float cx, float cy, float r, SDL_Color c) {
    fill_round_rect(cx - r, cy - r, 2.0f * r, 2.0f * r, r, c);
}

static void fill_triangle(float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color c) {
    SDL_Vertex v[3] = { vtx(x1, y1, c), vtx(x2, y2, c), vtx(x3, y3, c) };
    SDL_RenderGeometry(ren, NULL, v, 3, NULL, 0);
}

static void clear_black(void) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
}

static void draw_background(void) {
    if (pal == &PALETTE_BLACK) {
        clear_black();
        return;
    }
    /* the gradient covers the whole output; the pixel shift does not apply to it */
    float sx = shift_x, sy = shift_y;
    shift_x = shift_y = 0.0f;
    SDL_Vertex v[4] = {
        vtx(0, 0, COL_BG_TOP), vtx(base_w, 0, COL_BG_TOP),
        vtx(base_w, base_h, COL_BG_BOTTOM), vtx(0, base_h, COL_BG_BOTTOM),
    };
    int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(ren, NULL, v, 4, idx, 6);
    shift_x = sx;
    shift_y = sy;
}

static SDL_Color with_alpha(SDL_Color c, float a) {
    c.a = (Uint8) (c.a * (a < 0 ? 0 : (a > 1 ? 1 : a)));
    return c;
}

/* AirPlay-style mark: a screen with a triangle at its base, on an accent tile */
/* Console grip: rounded on the outer side, square where it meets the screen. */
static void fill_grip(float x, float y, float w, float h, bool left, SDL_Color c) {
    fill_round_rect(x, y, w, h, w * 0.5f, c);
    fill_rect(left ? x + w * 0.5f : x, y, w * 0.5f, h, c);
}

/* App logo: a console (screen between two grips) with the cast triangle below. */
static void draw_logo(float x, float y, float size) {
    fill_round_rect(x, y, size, size, size * 0.24f, COL_ACCENT);
    const float top = 0.23f, h = 0.34f, gw = 0.10f, gap = 0.022f, sw = 0.54f, t = 0.05f;
    const float lx = (1.0f - (2 * gw + 2 * gap + sw)) / 2.0f, sx = lx + gw + gap, rx = sx + sw + gap;
    fill_grip(x + lx * size, y + top * size, gw * size, h * size, true, COL_WHITE);
    fill_grip(x + rx * size, y + top * size, gw * size, h * size, false, COL_WHITE);
    fill_round_rect(x + sx * size, y + top * size, sw * size, h * size, 0.03f * size, COL_WHITE);
    fill_round_rect(x + (sx + t) * size, y + (top + t) * size, (sw - 2 * t) * size, (h - 2 * t) * size,
                    0.015f * size, COL_ACCENT);
    /* gap in the bottom edge where the triangle sits */
    fill_rect(x + 0.38f * size, y + (top + h - t) * size - 1.0f / scale, 0.24f * size, t * size + 2.0f / scale,
              COL_ACCENT);
    fill_triangle(x + 0.5f * size, y + (top + h - 0.05f) * size, x + 0.35f * size, y + (top + h + 0.21f) * size,
                  x + 0.65f * size, y + (top + h + 0.21f) * size, COL_WHITE);
}

static void draw_spinner(float cx, float cy, float r, unsigned long now_ms) {
    const int dots = 10;
    float phase = (float) (now_ms % 1000) / 1000.0f;
    for (int i = 0; i < dots; i++) {
        float a = 2.0f * (float) M_PI * (float) i / dots;
        float age = fmodf(phase - (float) i / dots + 1.0f, 1.0f);
        fill_circle(cx + cosf(a) * r, cy + sinf(a) * r, r * 0.16f, with_alpha(COL_TEXT, 1.0f - age * 0.85f));
    }
}

/* button glyph followed by a label; returns the x after it */
static float draw_hint_color(float x, float y, const char *button, const char *label, SDL_Color label_color) {
    const float rad = 13.0f;
    fill_circle(x + rad, y + rad, rad, COL_BUTTON);
    float bw = text_width(F_SMALL, button);
    draw_text_mid(F_SMALL, button, x + rad - bw / 2.0f, y + rad, COL_BLACK, ALIGN_LEFT);
    float w = draw_text_mid(F_BODY, label, x + 2 * rad + 10.0f, y + rad, label_color, ALIGN_LEFT);
    return x + 2 * rad + 10.0f + w + 34.0f;
}

static float draw_hint(float x, float y, const char *button, const char *label) {
    return draw_hint_color(x, y, button, label, COL_TEXT_DIM);
}

static float hint_width(const char *label) {
    return 26.0f + 10.0f + text_width(F_BODY, label) + 34.0f;
}

static void draw_footer(const ui_model_t *m, bool connected) {
    float y = base_h - 58.0f;
    float x = 80.0f;
    x = draw_hint(x, y, "+", tr(S_EXIT));
    if (m->screen == UI_SCREEN_READY) {
        x = draw_hint(x, y, "\xE2\x88\x92", tr(S_SETTINGS));
        x = draw_hint(x, y, "Y", tr(S_SCREEN_OFF));
        x = draw_hint(x, y, "X", m->show_stats ? tr(S_HIDE_STATS) : tr(S_STATS));
    }
    if (connected) x = draw_hint(x, y, "B", tr(S_DISCONNECT));
    if (m->screen == UI_SCREEN_ERROR) x = draw_hint(x, y, "A", tr(S_TRY_AGAIN));
    /* "v1.0.0 by depende3000", or just the version when the hints leave too little room */
    char version[64];
    snprintf(version, sizeof(version), tr(S_VERSION_BY), SWITCHCAST_VERSION, SWITCHCAST_AUTHOR);
    if (x >= base_w - 80.0f - text_width(F_SMALL, version)) snprintf(version, sizeof(version), "v%s", SWITCHCAST_VERSION);
    if (x < base_w - 80.0f - text_width(F_SMALL, version)) {
        draw_text(F_SMALL, version, base_w - 80.0f, y + 4.0f, COL_TEXT_DIM, ALIGN_RIGHT);
    }
}

static void draw_header(const char *subtitle) {
    draw_logo(80.0f, 60.0f, 62.0f);
    draw_text(F_TITLE, "SwitchAirCast", 162.0f, 56.0f, COL_TEXT, ALIGN_LEFT);
    draw_text(F_BODY, subtitle, 164.0f, 108.0f, COL_TEXT_DIM, ALIGN_LEFT);
}

static void format_time(char *buf, size_t len, double secs) {
    if (secs < 0) secs = 0;
    int s = (int) secs;
    if (s >= 3600) snprintf(buf, len, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(buf, len, "%d:%02d", s / 60, s % 60);
}

#define TOAST_MS 4000UL

static void draw_toast(const char *text, unsigned long age_ms) {
    if (!text || age_ms >= TOAST_MS) return;
    char line[TEXT_MAX];
    float a = age_ms < 3000 ? 1.0f : 1.0f - (float) (age_ms - 3000) / 1000.0f;
    fit_text(F_BODY, text, base_w - 120.0f, line, sizeof(line));
    float tw = text_width(F_BODY, line);
    float pw = tw + 56.0f, px = (base_w - pw) / 2.0f;
    fill_round_rect(px, 28.0f, pw, 46.0f, 23.0f, with_alpha((SDL_Color) { 0, 0, 0, 190 }, a));
    fill_circle(px + 24.0f, 51.0f, 5.0f, with_alpha(COL_GREEN, a));
    draw_text_mid(F_BODY, line, px + 38.0f, 51.0f, with_alpha(COL_TEXT, a), ALIGN_LEFT);
}

static bool toast_visible(const ui_model_t *m) {
    return (m->toast && m->now_ms - m->toast_ms < TOAST_MS) ||
           (m->mirror_started_ms && m->now_ms >= m->mirror_started_ms && m->now_ms - m->mirror_started_ms < TOAST_MS);
}

/* "Press (-) to open settings": shown for a few seconds when the console is touched during
 * a cast, under the toast if one is up, so people know the cast has options */
static void draw_options_hint(const ui_model_t *m) {
    if (!m->options_hint_ms || m->now_ms < m->options_hint_ms) return;
    unsigned long age = m->now_ms - m->options_hint_ms;
    if (age >= TOAST_MS) return;
    float a = age < 3000 ? 1.0f : 1.0f - (float) (age - 3000) / 1000.0f;
    const char *pre = tr(S_HINT_PRESS), *post = tr(S_HINT_OPEN_SETTINGS);
    const float rad = 13.0f, gap = 10.0f;
    float w_pre = text_width(F_BODY, pre), w_post = text_width(F_BODY, post);
    float pw = 28.0f + w_pre + gap + 2 * rad + gap + w_post + 28.0f, px = (base_w - pw) / 2.0f;
    float y = toast_visible(m) ? 84.0f : 28.0f, cy = y + 23.0f;
    fill_round_rect(px, y, pw, 46.0f, 23.0f, with_alpha((SDL_Color) { 0, 0, 0, 190 }, a));
    float x = px + 28.0f;
    draw_text_mid(F_BODY, pre, x, cy, with_alpha(COL_TEXT, a), ALIGN_LEFT);
    x += w_pre + gap;
    fill_circle(x + rad, cy, rad, with_alpha(COL_BUTTON, a));
    const char *minus = "\xE2\x88\x92";
    float bw = text_width(F_SMALL, minus);
    draw_text_mid(F_SMALL, minus, x + rad - bw / 2.0f, cy, with_alpha(COL_BLACK, a), ALIGN_LEFT);
    x += 2 * rad + gap;
    draw_text_mid(F_BODY, post, x, cy, with_alpha(COL_TEXT, a), ALIGN_LEFT);
}

static const char *client_or(const ui_model_t *m, str_id_t fallback) {
    return m->rx.client_name[0] ? m->rx.client_name : tr(fallback);
}

/* ---- screens ---- */

static void draw_message_screen(const ui_model_t *m, SDL_Color icon_color, const char *icon,
                                const char *title, const char *line1, const char *line2) {
    draw_background();
    draw_header(tr(S_APP_SUBTITLE));
    float cy = 300.0f;
    if (icon) {
        fill_circle(base_w / 2.0f, cy, 34.0f, icon_color);
        draw_text_mid(F_LARGE, icon, base_w / 2.0f, cy, COL_WHITE, ALIGN_CENTER);
    } else {
        draw_spinner(base_w / 2.0f, cy, 30.0f, m->now_ms);
    }
    char buf[TEXT_MAX];
    fit_text(F_HEADING, title, base_w - 160.0f, buf, sizeof(buf));
    draw_text(F_HEADING, buf, base_w / 2.0f, cy + 62.0f, COL_TEXT, ALIGN_CENTER);
    if (line1) {
        fit_text(F_BODY, line1, base_w - 120.0f, buf, sizeof(buf));
        draw_text(F_BODY, buf, base_w / 2.0f, cy + 108.0f, COL_TEXT_DIM, ALIGN_CENTER);
    }
    if (line2) {
        fit_text(F_BODY, line2, base_w - 120.0f, buf, sizeof(buf));
        draw_text(F_BODY, buf, base_w / 2.0f, cy + 140.0f, COL_TEXT_DIM, ALIGN_CENTER);
    }
    draw_footer(m, false);
}

static void draw_ready_screen(const ui_model_t *m) {
    char buf[TEXT_MAX], line[TEXT_MAX];
    const receiver_status_t *rx = &m->rx;
    bool connected = rx->state != RECEIVER_WAITING;

    draw_background();
    draw_header(tr(S_APP_SUBTITLE));

    float cx = 80.0f, cy = 172.0f, cw = base_w - 160.0f, ch = 352.0f;
    fill_round_rect(cx, cy, cw, ch, 22.0f, COL_CARD);

    if (connected) snprintf(buf, sizeof(buf), tr(S_CONNECTED_TO), client_or(m, S_A_DEVICE));
    else snprintf(buf, sizeof(buf), "%s", tr(S_READY_WAITING));
    fill_circle(cx + 40.0f, cy + 45.0f, 7.0f, connected ? COL_AMBER : COL_GREEN);
    fit_text(F_BODY, buf, cw - 100.0f, line, sizeof(line));
    draw_text(F_BODY, line, cx + 58.0f, cy + 31.0f, COL_TEXT_DIM, ALIGN_LEFT);

    fit_text(F_LARGE, m->name, cw - 64.0f, line, sizeof(line));
    draw_text(F_LARGE, line, cx + 32.0f, cy + 68.0f, COL_TEXT, ALIGN_LEFT);
    snprintf(buf, sizeof(buf), tr(S_IP_ADDRESS), m->ip);
    draw_text(F_BODY, buf, cx + 32.0f, cy + 122.0f, COL_TEXT_DIM, ALIGN_LEFT);

    fill_rect(cx + 32.0f, cy + 170.0f, cw - 64.0f, 1.5f, COL_DIVIDER);

    char choose[TEXT_MAX];
    snprintf(choose, sizeof(choose), tr(S_STEP_CHOOSE), m->name);
    const char *steps[3] = { tr(S_STEP_WIFI), tr(S_STEP_CONTROL_CENTER), choose };
    for (int i = 0; i < 3; i++) {
        float sy = cy + 196.0f + 50.0f * (float) i;
        fill_circle(cx + 46.0f, sy + 14.0f, 15.0f, COL_ACCENT);
        snprintf(buf, sizeof(buf), "%d", i + 1);
        draw_text_mid(F_SMALL, buf, cx + 46.0f, sy + 14.0f, COL_WHITE, ALIGN_CENTER);
        fit_text(F_BODY, steps[i], cw - 120.0f, line, sizeof(line));
        draw_text_mid(F_BODY, line, cx + 76.0f, sy + 14.0f, COL_TEXT, ALIGN_LEFT);
    }

    draw_footer(m, connected);
}

static void draw_now_playing(const ui_model_t *m) {
    char buf[TEXT_MAX], line[TEXT_MAX];
    const receiver_status_t *rx = &m->rx;

    draw_background();
    snprintf(buf, sizeof(buf), tr(S_PLAYING_FROM), client_or(m, S_A_DEVICE));
    fit_text(F_BODY, buf, base_w - 260.0f, line, sizeof(line));
    draw_header(line);

    const float art = 340.0f, ax = 110.0f, ay = 172.0f;
    if (m->coverart_tex) {
        fill_round_rect(ax - 6.0f, ay - 6.0f, art + 12.0f, art + 12.0f, 16.0f, COL_CARD);
        SDL_Rect dst = { (int) ((ax + shift_x) * scale), (int) ((ay + shift_y) * scale), (int) (art * scale),
                         (int) (art * scale) };
        SDL_RenderCopy(ren, m->coverart_tex, NULL, &dst);
    } else {
        fill_round_rect(ax, ay, art, art, 16.0f, COL_CARD);
        draw_logo(ax + art / 2.0f - 60.0f, ay + art / 2.0f - 60.0f, 120.0f);
    }

    float tx = ax + art + 56.0f, tw = base_w - tx - 80.0f;
    const char *title = rx->title[0] ? rx->title : tr(S_PLAYING_AUDIO);
    fit_text(F_LARGE, title, tw, line, sizeof(line));
    draw_text(F_LARGE, line, tx, ay + 40.0f, COL_TEXT, ALIGN_LEFT);
    if (rx->artist[0]) {
        fit_text(F_HEADING, rx->artist, tw, line, sizeof(line));
        draw_text(F_HEADING, line, tx, ay + 100.0f, COL_TEXT, ALIGN_LEFT);
    }
    if (rx->album[0]) {
        fit_text(F_BODY, rx->album, tw, line, sizeof(line));
        draw_text(F_BODY, line, tx, ay + 146.0f, COL_TEXT_DIM, ALIGN_LEFT);
    }
    if (rx->has_progress) {
        float py = ay + 250.0f;
        float frac = rx->duration > 0 ? (float) (rx->position / rx->duration) : 0.0f;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        fill_round_rect(tx, py, tw, 6.0f, 3.0f, COL_DIVIDER);
        if (frac > 0) fill_round_rect(tx, py, tw * frac, 6.0f, 3.0f, COL_ACCENT);
        format_time(buf, sizeof(buf), rx->position);
        draw_text(F_SMALL, buf, tx, py + 16.0f, COL_TEXT_DIM, ALIGN_LEFT);
        format_time(buf, sizeof(buf), rx->duration);
        draw_text(F_SMALL, buf, tx + tw, py + 16.0f, COL_TEXT_DIM, ALIGN_RIGHT);
    }
    draw_footer(m, true);
}

static const char *codec_label(const char *codec) {
    if (!codec) return "video";
    if (!strcmp(codec, "h264")) return "H.264";
    if (!strcmp(codec, "hevc")) return "H.265";
    if (!strcmp(codec, "vp9")) return "VP9";
    return codec;
}

static void draw_stats(const ui_model_t *m) {
    char lines[4][TEXT_MAX], ms[64], shown[16];
    const video_stats_t *v = &m->video;
    snprintf(lines[0], TEXT_MAX, "%dx%d  \xC2\xB7  %.0f fps", v->width, v->height, v->fps);
    snprintf(ms, sizeof(ms), tr(S_MS_PER_FRAME), v->decode_ms);
    snprintf(lines[1], TEXT_MAX, "%s %s  \xC2\xB7  %s", v->hw ? tr(S_HARDWARE) : tr(S_SOFTWARE),
             codec_label(v->codec), ms);
    /* the share shown over the last few seconds: a burst of skips fades out again */
    if (v->shown_percent < 0.0f) {
        snprintf(shown, sizeof(shown), "\xE2\x80\x94");
    } else if (v->shown_percent >= 99.95f) {
        snprintf(shown, sizeof(shown), "100%%");
    } else {
        snprintf(shown, sizeof(shown), "%.1f%%", v->shown_percent);
    }
    snprintf(lines[2], TEXT_MAX, tr(S_FRAMES_SHOWN), shown);
    snprintf(lines[3], TEXT_MAX, "%s  \xC2\xB7  %s", m->ip, m->rx.audio_playing ? tr(S_AUDIO_ON) : tr(S_AUDIO_OFF));
    float w = 0;
    for (int i = 0; i < 4; i++) {
        float tw = text_width(F_SMALL, lines[i]);
        if (tw > w) w = tw;
    }
    fill_round_rect(20.0f, 20.0f, w + 32.0f, 4 * 26.0f + 24.0f, 12.0f, (SDL_Color) { 0, 0, 0, 170 });
    for (int i = 0; i < 4; i++) {
        draw_text(F_SMALL, lines[i], 36.0f, 32.0f + 26.0f * (float) i, COL_WHITE, ALIGN_LEFT);
    }
}

static void draw_video_fit(const ui_model_t *m) {
    float sx = (float) out_w / (float) m->video_w, sy = (float) out_h / (float) m->video_h;
    float s = sx < sy ? sx : sy;
    int w = (int) ((float) m->video_w * s + 0.5f), h = (int) ((float) m->video_h * s + 0.5f);
    SDL_Rect dst = { (out_w - w) / 2, (out_h - h) / 2, w, h };
    SDL_RenderCopy(ren, m->video_tex, NULL, &dst);
}

static void draw_play_glyph(float cx, float cy, float size, bool paused) {
    if (paused) {
        /* paused: show "play" */
        fill_triangle(cx - size * 0.35f, cy - size * 0.5f, cx - size * 0.35f, cy + size * 0.5f, cx + size * 0.5f, cy,
                      COL_WHITE);
    } else {
        fill_round_rect(cx - size * 0.42f, cy - size * 0.5f, size * 0.3f, size, size * 0.06f, COL_WHITE);
        fill_round_rect(cx + size * 0.12f, cy - size * 0.5f, size * 0.3f, size, size * 0.06f, COL_WHITE);
    }
}

/* progress bar and hints along the bottom while paused or after a button press */
static void draw_playback_controls(const ui_model_t *m) {
    const player_status_t *p = &m->player;
    char buf[TEXT_MAX];
    SDL_Color text = { 240, 243, 248, 255 };
    /* fade to dark behind the controls so they read on any picture */
    SDL_Color clear = { 0, 0, 0, 0 }, half = { 0, 0, 0, 170 }, shade = { 0, 0, 0, 225 };
    SDL_Vertex v[6] = {
        vtx(0, base_h - 240.0f, clear), vtx(base_w, base_h - 240.0f, clear),
        vtx(base_w, base_h - 150.0f, half), vtx(0, base_h - 150.0f, half),
        vtx(base_w, base_h, shade), vtx(0, base_h, shade),
    };
    int idx[12] = { 0, 1, 2, 0, 2, 3, 3, 2, 4, 3, 4, 5 };
    SDL_RenderGeometry(ren, NULL, v, 6, idx, 12);

    float x = 60.0f, w = base_w - 120.0f, y = base_h - 96.0f;
    draw_play_glyph(x + 14.0f, y - 34.0f, 26.0f, p->paused);
    if (p->duration > 0) {
        float frac = (float) (p->position / p->duration);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        fill_round_rect(x, y, w, 6.0f, 3.0f, (SDL_Color) { 255, 255, 255, 60 });
        if (frac > 0) fill_round_rect(x, y, w * frac, 6.0f, 3.0f, COL_ACCENT);
        fill_circle(x + w * frac, y + 3.0f, 9.0f, COL_WHITE);
        format_time(buf, sizeof(buf), p->position);
        draw_text(F_SMALL, buf, x, y + 18.0f, text, ALIGN_LEFT);
        format_time(buf, sizeof(buf), p->duration);
        draw_text(F_SMALL, buf, x + w, y + 18.0f, text, ALIGN_RIGHT);
    } else {
        format_time(buf, sizeof(buf), p->position);
        draw_text(F_SMALL, buf, x, y + 18.0f, text, ALIGN_LEFT);
    }

    /* as many hints as fit (a rotated, portrait layout is narrow) */
    struct { const char *button, *label; } hints[] = {
        { "A", p->paused ? tr(S_PLAY) : tr(S_PAUSE) },
        { "B", tr(S_STOP) },
        { "R", tr(S_ROTATE) },
        { "\xE2\x86\x94", tr(S_SECONDS_10) },
        { "\xE2\x86\x95", tr(S_MINUTE_1) },
    };
    float hx = x + 60.0f;
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        if (hx + hint_width(hints[i].label) > base_w - 40.0f) break;
        hx = draw_hint_color(hx, y - 47.0f, hints[i].button, hints[i].label, text);
    }
}

static void draw_video_playback(const ui_model_t *m) {
    char buf[TEXT_MAX], line[TEXT_MAX];
    const player_status_t *p = &m->player;
    clear_black();
    const char *who = m->rx.client_name[0] ? m->rx.client_name : NULL;

    if (p->state == PLAYER_ENDED && p->failed) {
        draw_message_screen(m, COL_RED, "!", tr(S_VIDEO_FAILED_TITLE), tr(S_VIDEO_FAILED_LINE1),
                            tr(S_VIDEO_FAILED_LINE2));
        return;
    }
    if (m->video_tex && m->video_w > 0 && m->video_h > 0) {
        draw_video_fit(m);
        if (p->buffering) draw_spinner(base_w / 2.0f, base_h / 2.0f, 30.0f, m->now_ms);
    } else {
        draw_spinner(base_w / 2.0f, base_h / 2.0f - 30.0f, 30.0f, m->now_ms);
        if (who) snprintf(buf, sizeof(buf), tr(S_LOADING_VIDEO_FROM), who);
        else snprintf(buf, sizeof(buf), "%s", tr(S_LOADING_VIDEO));
        fit_text(F_BODY, buf, base_w - 80.0f, line, sizeof(line));
        draw_text(F_BODY, line, base_w / 2.0f, base_h / 2.0f + 30.0f, (SDL_Color) { 150, 161, 182, 255 },
                  ALIGN_CENTER);
    }
    if (m->mirror_started_ms && m->now_ms >= m->mirror_started_ms && m->video_tex) {
        if (who) snprintf(buf, sizeof(buf), tr(S_PLAYING_VIDEO_FROM), who);
        else snprintf(buf, sizeof(buf), "%s", tr(S_PLAYING_VIDEO));
        draw_toast(buf, m->now_ms - m->mirror_started_ms);
    }
    if (p->state == PLAYER_PLAYING && (p->paused || m->now_ms < m->osd_until_ms)) {
        draw_playback_controls(m);
    }
    if (m->show_stats) draw_stats(m);
}

static void draw_mirroring(const ui_model_t *m) {
    char buf[TEXT_MAX], line[TEXT_MAX];
    clear_black();
    if (m->video_tex && m->video_w > 0 && m->video_h > 0) {
        draw_video_fit(m);
    } else {
        draw_spinner(base_w / 2.0f, base_h / 2.0f - 30.0f, 30.0f, m->now_ms);
        snprintf(buf, sizeof(buf), tr(S_STARTING_MIRROR_FROM), client_or(m, S_YOUR_DEVICE));
        fit_text(F_BODY, buf, base_w - 80.0f, line, sizeof(line));
        draw_text(F_BODY, line, base_w / 2.0f, base_h / 2.0f + 30.0f, (SDL_Color) { 150, 161, 182, 255 },
                  ALIGN_CENTER);
    }
    if (m->mirror_started_ms && m->now_ms >= m->mirror_started_ms) {
        snprintf(buf, sizeof(buf), tr(S_MIRRORING_FROM), client_or(m, S_YOUR_DEVICE));
        draw_toast(buf, m->now_ms - m->mirror_started_ms);
    }
    if (m->show_stats) draw_stats(m);
}

static void draw_settings(const ui_model_t *m) {
    char line[TEXT_MAX];
    fill_rect(-shift_x, -shift_y, base_w, base_h, (SDL_Color) { 0, 0, 0, 180 });
    const float pw = 840.0f, ph = 576.0f, px = (base_w - pw) / 2.0f, py = (base_h - ph) / 2.0f - 10.0f;
    SDL_Color panel = pal == &PALETTE_BLACK ? (SDL_Color) { 14, 14, 16, 255 } : COL_CARD;
    fill_round_rect(px, py, pw, ph, 24.0f, panel);
    draw_text(F_HEADING, tr(S_SETTINGS), px + 40.0f, py + 30.0f, COL_TEXT, ALIGN_LEFT);

    for (int i = 0; i < UI_SETTINGS_ITEMS; i++) {
        float ry = py + 92.0f + 64.0f * (float) i;
        bool sel = i == m->settings_index;
        if (sel) fill_round_rect(px + 20.0f, ry, pw - 40.0f, 54.0f, 14.0f, with_alpha(COL_ACCENT, 0.28f));
        fit_text(F_BODY, m->settings_labels[i], pw * 0.42f, line, sizeof(line));
        draw_text_mid(F_BODY, line, px + 44.0f, ry + 27.0f, COL_TEXT, ALIGN_LEFT);
        float vx = px + pw - 44.0f;
        if (sel) {
            draw_text_mid(F_BODY, "\xE2\x80\xBA", vx, ry + 27.0f, COL_ACCENT, ALIGN_RIGHT);
            vx -= 26.0f;
        }
        fit_text(F_BODY, m->settings_values[i], pw * 0.45f, line, sizeof(line));
        float vw = draw_text_mid(F_BODY, line, vx, ry + 27.0f, sel ? COL_TEXT : COL_TEXT_DIM, ALIGN_RIGHT);
        if (sel) draw_text_mid(F_BODY, "\xE2\x80\xB9", vx - vw - 14.0f, ry + 27.0f, COL_ACCENT, ALIGN_RIGHT);
    }
    fit_text(F_SMALL, m->settings_note ? m->settings_note : "", pw - 80.0f, line, sizeof(line));
    draw_text(F_SMALL, line, px + 40.0f, py + ph - 100.0f, COL_TEXT_DIM, ALIGN_LEFT);
    float hx = px + 40.0f, hy = py + ph - 58.0f;
    hx = draw_hint(hx, hy, "A", tr(S_CHANGE));
    draw_hint(hx, hy, "B", tr(S_CLOSE));
}

/* The code a Mac asks for before it sends video, one digit per box. */
static void draw_pin(const ui_model_t *m) {
    char line[TEXT_MAX];
    fill_rect(-shift_x, -shift_y, base_w, base_h, (SDL_Color) { 0, 0, 0, 170 });
    const float pw = 760.0f, ph = 350.0f, px = (base_w - pw) / 2.0f, py = (base_h - ph) / 2.0f;
    SDL_Color panel = pal == &PALETTE_BLACK ? (SDL_Color) { 14, 14, 16, 255 } : COL_CARD;
    fill_round_rect(px, py, pw, ph, 24.0f, panel);
    draw_text(F_HEADING, tr(S_PIN_TITLE), base_w / 2.0f, py + 34.0f, COL_TEXT, ALIGN_CENTER);

    int n = (int) strlen(m->rx.pin);
    const float bw = 120.0f, bh = 150.0f, gap = 24.0f;
    float bx = (base_w - (bw * (float) n + gap * (float) (n - 1))) / 2.0f, by = py + 96.0f;
    for (int i = 0; i < n; i++) {
        char digit[2] = { m->rx.pin[i], '\0' };
        float x = bx + (bw + gap) * (float) i;
        fill_round_rect(x, by, bw, bh, 18.0f, with_alpha(COL_ACCENT, 0.22f));
        draw_text_mid(F_HUGE, digit, x + bw / 2.0f, by + bh / 2.0f, COL_TEXT, ALIGN_CENTER);
    }
    fit_text(F_BODY, tr(S_PIN_HINT), pw - 80.0f, line, sizeof(line));
    draw_text(F_BODY, line, base_w / 2.0f, by + bh + 34.0f, COL_TEXT_DIM, ALIGN_CENTER);
}

static void draw_screen_off(const ui_model_t *m) {
    clear_black();
    /* a short reminder (visible docked; the panel is dark in handheld mode) */
    if (m->now_ms - m->screen_off_ms < 3000) {
        char line[TEXT_MAX];
        fit_text(F_BODY, tr(S_SCREEN_OFF_TOAST), base_w - 120.0f, line, sizeof(line));
        draw_text_mid(F_BODY, line, base_w / 2.0f, base_h / 2.0f, (SDL_Color) { 110, 110, 118, 255 }, ALIGN_CENTER);
    }
}

/* ---- public ---- */

bool ui_init(SDL_Renderer *renderer) {
    ren = renderer;
    if (TTF_Init() < 0) {
        platform_log("ui: TTF_Init failed: %s", TTF_GetError());
        return false;
    }
    if (!platform_get_font(&font_data, &font_size)) {
        platform_log("ui: no font available");
        return false;
    }
    return true;
}

void ui_shutdown(void) {
    ui_clear_text_cache();
    close_fonts();
    if (rotate_target) SDL_DestroyTexture(rotate_target);
    rotate_target = NULL;
    TTF_Quit();
}

void ui_set_black_theme(bool black) {
    pal = black ? &PALETTE_BLACK : &PALETTE_DARK;
}

static void set_output(int w, int h) {
    out_w = w;
    out_h = h;
    scale = (float) (w < h ? w : h) / BASE_H;
    base_w = (float) w / scale;
    base_h = (float) h / scale;
}

static bool is_video_screen(const ui_model_t *m) {
    return m->screen == UI_SCREEN_READY && (m->rx.state == RECEIVER_VIDEO || m->rx.state == RECEIVER_MIRRORING);
}

static void draw_screen(const ui_model_t *m) {
    switch (m->screen) {
    case UI_SCREEN_STARTING:
        draw_message_screen(m, COL_ACCENT, NULL, tr(S_STARTING), NULL, NULL);
        break;
    case UI_SCREEN_NO_NETWORK:
        draw_message_screen(m, COL_RED, "!", tr(S_NO_NETWORK_TITLE), tr(S_NO_NETWORK_LINE1), tr(S_NO_NETWORK_LINE2));
        break;
    case UI_SCREEN_ERROR:
        draw_message_screen(m, COL_RED, "!", tr(S_START_ERROR_TITLE), m->error, NULL);
        break;
    case UI_SCREEN_APPLET_MODE:
        draw_message_screen(m, COL_RED, "!", tr(S_APPLET_TITLE), tr(S_APPLET_LINE1), tr(S_APPLET_LINE2));
        break;
    case UI_SCREEN_READY:
        if (m->rx.state == RECEIVER_VIDEO) {
            draw_video_playback(m);
        } else if (m->rx.state == RECEIVER_MIRRORING) {
            draw_mirroring(m);
        } else if (m->rx.state == RECEIVER_CONNECTED && (m->rx.audio_playing || m->rx.title[0])) {
            draw_now_playing(m);
        } else {
            draw_ready_screen(m);
        }
        break;
    }
}

/* Draws the mirrored/cast picture and its overlays portrait into a texture, then turns
 * it onto the landscape screen. */
static bool draw_rotated(const ui_model_t *m, int screen_w, int screen_h) {
    int tw = screen_h, th = screen_w;
    if (!rotate_target || rotate_w != tw || rotate_h != th) {
        if (rotate_target) SDL_DestroyTexture(rotate_target);
        rotate_target = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, tw, th);
        rotate_w = tw;
        rotate_h = th;
        if (!rotate_target) {
            platform_log("ui: cannot create rotation target: %s", SDL_GetError());
            return false;
        }
    }
    if (SDL_SetRenderTarget(ren, rotate_target) != 0) return false;
    set_output(tw, th);
    if (!build_fonts()) {
        SDL_SetRenderTarget(ren, NULL);
        return false;
    }
    draw_screen(m);
    if (m->toast) draw_toast(m->toast, m->now_ms - m->toast_ms);
    draw_options_hint(m);
    SDL_SetRenderTarget(ren, NULL);
    set_output(screen_w, screen_h);
    SDL_Rect dst = { (screen_w - tw) / 2, (screen_h - th) / 2, tw, th };
    SDL_RenderCopyEx(ren, rotate_target, NULL, &dst, (double) m->rotation, NULL, SDL_FLIP_NONE);
    return true;
}

void ui_draw(const ui_model_t *m) {
    int screen_w = 0, screen_h = 0;
    SDL_GetRendererOutputSize(ren, &screen_w, &screen_h);
    if (screen_w <= 0 || screen_h <= 0) return;
    set_output(screen_w, screen_h);
    if (!build_fonts()) return;
    frame_no++;

    if (m->screen_off) {
        shift_x = shift_y = 0.0f;
        draw_screen_off(m);
        return;
    }

    bool video = is_video_screen(m);
    if (video) {
        shift_x = shift_y = 0.0f;
    } else {
        /* move static screens by a few units every minute */
        static const float offsets[9][2] = { { 0, 0 }, { 3, 0 }, { 3, 3 }, { 0, 3 }, { -3, 3 },
                                             { -3, 0 }, { -3, -3 }, { 0, -3 }, { 3, -3 } };
        int k = (int) ((m->now_ms / 60000UL) % 9);
        shift_x = offsets[k][0];
        shift_y = offsets[k][1];
    }

    bool rotated = video && m->rotation && !m->settings_open && draw_rotated(m, screen_w, screen_h);
    if (!rotated) {
        draw_screen(m);
        if (m->toast) draw_toast(m->toast, m->now_ms - m->toast_ms);
        if (video) draw_options_hint(m);
    }
    if (m->settings_open) {
        shift_x = shift_y = 0.0f;
        draw_settings(m);
    }
    if (m->rx.pin[0]) {
        shift_x = shift_y = 0.0f;
        draw_pin(m);
    }
}

/* Square application icon (used to generate icon.jpg for the homebrew menu). */
void ui_draw_app_icon(int size) {
    set_output(size, size);
    if (!build_fonts()) return;
    frame_no++;
    shift_x = shift_y = 0.0f;
    draw_background();
    draw_logo(BASE_H * 0.08f, BASE_H * 0.08f, BASE_H * 0.84f);
}
