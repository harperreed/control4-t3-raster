/* ABOUTME: Anti-aliased TrueType text onto an RGBA buffer, through the vendored stb_truetype.
 * ABOUTME: Measures and draws UTF-8 strings with kerning; used by the fallback clock face. */
#ifndef TT7D_FONT_H
#define TT7D_FONT_H

#include <stddef.h>
#include <stdint.h>

struct font; /* opaque: wraps stbtt_fontinfo, which font.c keeps private */

/* Parse a TTF held in memory (not copied: it must outlive the font).
 * Returns NULL if it is not a usable font. */
struct font *font_load(const unsigned char *ttf, size_t len);
void font_free(struct font *f);

/* Ink bounds of a string, in pixels, relative to the pen start on the
 * baseline: y0 < 0 is above the baseline. An empty string gives all zeros. */
struct text_box {
    int x0, y0, x1, y1;
};

/* The ink box of `utf8` at a size of `px` pixels per em, and the pen advance
 * (where the next string would start). Either output may be NULL. */
void font_measure(const struct font *f, float px, const char *utf8, struct text_box *ink, int *advance);

/* Draw `utf8` with the pen starting at (pen_x, baseline), blending colour
 * rgb over the existing pixels by glyph coverage. Pixels outside the w x h
 * buffer are skipped. Alpha bytes are set to 255. */
void font_draw(const struct font *f, float px, const char *utf8, int pen_x, int baseline, const uint8_t rgb[3],
               uint8_t *rgba, int w, int h);

#endif
