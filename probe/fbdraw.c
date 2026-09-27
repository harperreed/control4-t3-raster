/* ABOUTME: Pure framebuffer drawing for the TT7 probe: pixel packing, rects, 8x8 text, test pattern.
 * ABOUTME: No syscalls here; tt7probe.c maps /dev/fb0 and hands the memory in. */
#include "fbdraw.h"

#include <string.h>

#include "font8x8_basic.h" /* third_party/mmkeypad/init, public domain */

const uint8_t fbd_bars[FBD_NBARS][3] = {
    {255, 255, 255}, /* white   */
    {255, 255, 0},   /* yellow  */
    {0, 255, 255},   /* cyan    */
    {0, 255, 0},     /* green   */
    {255, 0, 255},   /* magenta */
    {255, 0, 0},     /* red     */
    {0, 0, 255},     /* blue    */
    {128, 128, 128}, /* grey    */
};

int fbd_init(struct fbd_surface *s, uint8_t *mem, uint32_t width, uint32_t height,
             uint32_t stride, uint32_t bpp, struct fbd_chan red, struct fbd_chan green,
             struct fbd_chan blue, struct fbd_chan transp) {
    if (bpp != 16 && bpp != 24 && bpp != 32) return -1;
    s->mem = mem;
    s->width = width;
    s->height = height;
    s->stride = stride;
    s->bpp = bpp;
    s->red = red;
    s->green = green;
    s->blue = blue;
    s->transp = transp;
    if (red.length == 0 && green.length == 0 && blue.length == 0) {
        struct fbd_chan none = {0, 0};
        if (bpp == 16) {
            s->red = (struct fbd_chan){11, 5};
            s->green = (struct fbd_chan){5, 6};
            s->blue = (struct fbd_chan){0, 5};
        } else {
            s->red = (struct fbd_chan){16, 8};
            s->green = (struct fbd_chan){8, 8};
            s->blue = (struct fbd_chan){0, 8};
        }
        s->transp = none;
    }
    return 0;
}

/* Scale an 8-bit value to `len` bits by keeping its top bits. */
static uint32_t chan_bits(uint8_t v, uint32_t len) {
    if (len == 0) return 0;
    if (len <= 8) return (uint32_t)v >> (8 - len);
    return (uint32_t)v << (len - 8);
}

uint32_t fbd_pack(const struct fbd_surface *s, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t px = chan_bits(r, s->red.length) << s->red.offset;
    px |= chan_bits(g, s->green.length) << s->green.offset;
    px |= chan_bits(b, s->blue.length) << s->blue.offset;
    if (s->transp.length > 0 && s->transp.length < 32)
        px |= ((1u << s->transp.length) - 1) << s->transp.offset;
    return px;
}

static uint8_t *pixel_addr(const struct fbd_surface *s, int x, int y) {
    return s->mem + (size_t)y * s->stride + (size_t)x * (s->bpp / 8);
}

static int inside(const struct fbd_surface *s, int x, int y) {
    return x >= 0 && y >= 0 && (uint32_t)x < s->width && (uint32_t)y < s->height;
}

void fbd_put(const struct fbd_surface *s, int x, int y, uint32_t px) {
    if (!inside(s, x, y)) return;
    uint8_t *p = pixel_addr(s, x, y);
    /* Little-endian, as the RK3188 (and every host we test on) stores pixels. */
    for (uint32_t i = 0; i < s->bpp / 8; i++) p[i] = (uint8_t)(px >> (8 * i));
}

uint32_t fbd_get(const struct fbd_surface *s, int x, int y) {
    if (!inside(s, x, y)) return 0;
    const uint8_t *p = pixel_addr(s, x, y);
    uint32_t px = 0;
    for (uint32_t i = 0; i < s->bpp / 8; i++) px |= (uint32_t)p[i] << (8 * i);
    return px;
}

void fbd_fill(const struct fbd_surface *s, int x, int y, int w, int h, uint32_t px) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > (int)s->width) x1 = (int)s->width;
    if (y1 > (int)s->height) y1 = (int)s->height;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) fbd_put(s, xx, yy, px);
}

void fbd_border(const struct fbd_surface *s, uint32_t px) {
    int w = (int)s->width, h = (int)s->height;
    fbd_fill(s, 0, 0, w, 1, px);
    fbd_fill(s, 0, h - 1, w, 1, px);
    fbd_fill(s, 0, 0, 1, h, px);
    fbd_fill(s, w - 1, 0, 1, h, px);
}

int fbd_text_width(int scale, const char *str) {
    return (int)strlen(str) * 8 * scale;
}

void fbd_text(const struct fbd_surface *s, int x, int y, int scale, const char *str, uint32_t px) {
    for (; *str; str++, x += 8 * scale) {
        unsigned char ch = (unsigned char)*str;
        const char *glyph = font8x8_basic[ch < 128 ? ch : '?'];
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (glyph[row] & (1 << col)) /* bit n is column n, leftmost first */
                    fbd_fill(s, x + col * scale, y + row * scale, scale, scale, px);
    }
}

void fbd_dot(const struct fbd_surface *s, int x, int y, uint32_t px) {
    int r = (int)fbd_unit(s) / 5; /* 8 px on a 7" panel: big enough for a photo */
    if (r < 3) r = 3;
    fbd_fill(s, x - r, y - r, 2 * r + 1, 2 * r + 1, px);
}

int fbd_scale(int32_t value, int32_t min, int32_t max, uint32_t size) {
    if (max <= min || size == 0) return -1;
    if (value < min) value = min;
    if (value > max) value = max;
    return (int)(((int64_t)value - min) * (int64_t)(size - 1) / ((int64_t)max - min));
}

uint32_t fbd_unit(const struct fbd_surface *s) {
    uint32_t m = s->width < s->height ? s->width : s->height;
    return m / 20;
}

void fbd_bar_band(const struct fbd_surface *s, int *y0, int *h) {
    *y0 = (int)(s->height * 45 / 100);
    *h = (int)(s->height / 8);
}

void fbd_ramp_band(const struct fbd_surface *s, int *y0, int *h) {
    int by, bh;
    fbd_bar_band(s, &by, &bh);
    *y0 = by + bh + (int)fbd_unit(s) / 2;
    *h = (int)(s->height / 8);
}

/* Text scale: 8x8 glyphs at 3x on a 7" panel (unit 40), never below 1. */
static int text_scale(const struct fbd_surface *s) {
    int ts = (int)fbd_unit(s) / 13;
    return ts < 1 ? 1 : ts;
}

static void text_centred(const struct fbd_surface *s, int y, int ts, const char *str, uint32_t px) {
    int tw = fbd_text_width(ts, str);
    fbd_text(s, ((int)s->width - tw) / 2, y, ts, str, px);
}

void fbd_test_pattern(const struct fbd_surface *s, const char *const *info) {
    int w = (int)s->width, h = (int)s->height, u = (int)fbd_unit(s), i = FBD_INSET;
    int ts = text_scale(s), line = 8 * ts + 4 * ts;
    uint32_t black = fbd_pack(s, 0, 0, 0), white = fbd_pack(s, 255, 255, 255);

    fbd_fill(s, 0, 0, w, h, black);

    /* Corner blocks: a different colour AND size in each corner, so a photo
     * shows which memory corner ended up where on the glass. */
    int rs = FBD_RED_UNITS * u, gs = FBD_GREEN_UNITS * u, bs = FBD_BLUE_UNITS * u, ws = FBD_WHITE_UNITS * u;
    fbd_fill(s, i, i, rs, rs, fbd_pack(s, 255, 0, 0));
    fbd_fill(s, w - i - gs, i, gs, gs, fbd_pack(s, 0, 255, 0));
    fbd_fill(s, i, h - i - bs, bs, bs, fbd_pack(s, 0, 0, 255));
    fbd_fill(s, w - i - ws, h - i - ws, ws, ws, white);

    /* Labels sit beside each block, half a unit away. */
    int gap = u / 2, th = 8 * ts;
    fbd_text(s, i + rs + gap, i + gap, ts, "TL RED", white);
    fbd_text(s, w - i - gs - gap - fbd_text_width(ts, "TR GREEN"), i + gap, ts, "TR GREEN", white);
    fbd_text(s, i + bs + gap, h - i - gap - th, ts, "BL BLUE", white);
    fbd_text(s, w - i - ws - gap - fbd_text_width(ts, "BR WHITE"), h - i - gap - th, ts, "BR WHITE", white);
    text_centred(s, i + gap + line, ts, "^ TOP EDGE (y=0) ^", white);

    /* Colour bars. */
    int by, bh;
    fbd_bar_band(s, &by, &bh);
    for (int b = 0; b < FBD_NBARS; b++) {
        int x0 = b * w / FBD_NBARS, x1 = (b + 1) * w / FBD_NBARS;
        fbd_fill(s, x0, by, x1 - x0, bh, fbd_pack(s, fbd_bars[b][0], fbd_bars[b][1], fbd_bars[b][2]));
    }

    /* Ramps: R, G, B, grey from 0 at the left to 255 at the right. Banding
     * shows the real channel depth; a swapped channel shows the wrong colour. */
    int ry, rh;
    fbd_ramp_band(s, &ry, &rh);
    int strip = rh / 4;
    for (int x = 0; x < w && strip > 0; x++) {
        int v = x * 256 / w;
        uint8_t c = (uint8_t)(v > 255 ? 255 : v);
        fbd_fill(s, x, ry + 0 * strip, 1, strip, fbd_pack(s, c, 0, 0));
        fbd_fill(s, x, ry + 1 * strip, 1, strip, fbd_pack(s, 0, c, 0));
        fbd_fill(s, x, ry + 2 * strip, 1, strip, fbd_pack(s, 0, 0, c));
        fbd_fill(s, x, ry + 3 * strip, 1, strip, fbd_pack(s, c, c, c));
    }

    /* Info lines between the top blocks and the bars; stop before the bars. */
    int y = i + rs + gap;
    for (; info && *info && y + th < by - ts; info++, y += line) text_centred(s, y, ts, *info, white);
    text_centred(s, ry + rh + gap, ts, "TOUCH: DOT AT RAW ABS X,Y", white);

    fbd_border(s, white);
}
