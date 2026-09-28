/* ABOUTME: TrueType text for tt7d: UTF-8 decoding, kerning, measuring and coverage-blended drawing,
 * ABOUTME: on top of the vendored stb_truetype (third_party/stb/PROVENANCE). */
#include "font.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* stb_truetype is vendored unmodified; its static helpers we never call
 * would otherwise trip -Werror. */
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

struct font {
    stbtt_fontinfo info;
};

struct font *font_load(const unsigned char *ttf, size_t len) {
    if (!ttf || len < 12) return NULL;
    struct font *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    int offset = stbtt_GetFontOffsetForIndex(ttf, 0);
    if (offset < 0 || !stbtt_InitFont(&f->info, ttf, offset)) {
        free(f);
        return NULL;
    }
    return f;
}

void font_free(struct font *f) { free(f); }

/* Next code point of a UTF-8 string, advancing *s. Malformed input gives
 * U+FFFD (drawn as the font's missing-glyph box) and moves on one byte. */
static unsigned next_cp(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = p[0], cp;
    int extra;
    if (c < 0x80) {
        *s += 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0) cp = c & 0x1F, extra = 1;
    else if ((c & 0xF0) == 0xE0) cp = c & 0x0F, extra = 2;
    else if ((c & 0xF8) == 0xF0) cp = c & 0x07, extra = 3;
    else {
        *s += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s += 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += 1 + extra;
    return cp;
}

/* Walk the string's glyphs: for each, call `each` with its glyph index and
 * pen x (float, from the pen start). Returns the final pen x. */
typedef void (*glyph_fn)(const struct font *f, float scale, int glyph, float pen_x, void *ctx);

static float walk(const struct font *f, float scale, const char *utf8, glyph_fn each, void *ctx) {
    float x = 0;
    int prev = 0;
    while (*utf8) {
        int g = stbtt_FindGlyphIndex(&f->info, (int)next_cp(&utf8));
        if (prev) x += scale * (float)stbtt_GetGlyphKernAdvance(&f->info, prev, g);
        each(f, scale, g, x, ctx);
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);
        x += scale * (float)adv;
        prev = g;
    }
    return x;
}

struct measure_ctx {
    struct text_box box;
    int any;
};

static void measure_glyph(const struct font *f, float scale, int g, float pen_x, void *ctx) {
    struct measure_ctx *m = ctx;
    int ix = (int)floorf(pen_x), x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&f->info, g, scale, scale, pen_x - (float)ix, 0, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) return; /* a space: no ink */
    x0 += ix, x1 += ix;
    if (!m->any) {
        m->box = (struct text_box){x0, y0, x1, y1};
        m->any = 1;
        return;
    }
    if (x0 < m->box.x0) m->box.x0 = x0;
    if (y0 < m->box.y0) m->box.y0 = y0;
    if (x1 > m->box.x1) m->box.x1 = x1;
    if (y1 > m->box.y1) m->box.y1 = y1;
}

void font_measure(const struct font *f, float px, const char *utf8, struct text_box *ink, int *advance) {
    struct measure_ctx m = {0};
    float end = walk(f, stbtt_ScaleForMappingEmToPixels(&f->info, px), utf8, measure_glyph, &m);
    if (ink) *ink = m.box;
    if (advance) *advance = (int)lroundf(end);
}

struct draw_ctx {
    int pen_x, baseline;
    const uint8_t *rgb;
    uint8_t *rgba;
    int w, h;
};

static void draw_glyph(const struct font *f, float scale, int g, float pen_x, void *ctx) {
    struct draw_ctx *d = ctx;
    int ix = (int)floorf(pen_x), x0, y0, x1, y1;
    float frac = pen_x - (float)ix;
    stbtt_GetGlyphBitmapBoxSubpixel(&f->info, g, scale, scale, frac, 0, &x0, &y0, &x1, &y1);
    int gw = x1 - x0, gh = y1 - y0;
    if (gw <= 0 || gh <= 0) return;
    unsigned char *cov = malloc((size_t)gw * (size_t)gh);
    if (!cov) return;
    stbtt_MakeGlyphBitmapSubpixel(&f->info, cov, gw, gh, gw, scale, scale, frac, 0, g);
    int ox = d->pen_x + ix + x0, oy = d->baseline + y0;
    for (int y = 0; y < gh; y++) {
        int py = oy + y;
        if (py < 0 || py >= d->h) continue;
        for (int x = 0; x < gw; x++) {
            int px = ox + x;
            unsigned a = cov[y * gw + x];
            if (px < 0 || px >= d->w || !a) continue;
            uint8_t *o = d->rgba + ((size_t)py * (size_t)d->w + (size_t)px) * 4;
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)((d->rgb[c] * a + o[c] * (255 - a) + 127) / 255);
            o[3] = 255;
        }
    }
    free(cov);
}

void font_draw(const struct font *f, float px, const char *utf8, int pen_x, int baseline, const uint8_t rgb[3],
               uint8_t *rgba, int w, int h) {
    struct draw_ctx d = {pen_x, baseline, rgb, rgba, w, h};
    walk(f, stbtt_ScaleForMappingEmToPixels(&f->info, px), utf8, draw_glyph, &d);
}
