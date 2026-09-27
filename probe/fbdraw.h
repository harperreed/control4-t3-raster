/* ABOUTME: Pure framebuffer drawing for the TT7 probe: pixel packing, rects, 8x8 text, test pattern.
 * ABOUTME: Works on any memory buffer, so host unit tests run it without a device. */
#ifndef TT7_FBDRAW_H
#define TT7_FBDRAW_H

#include <stdint.h>

/* One colour channel's place in a pixel, as fb_var_screeninfo reports it. */
struct fbd_chan {
    uint32_t offset;
    uint32_t length;
};

/* A drawable surface: the visible xres*yres window of a framebuffer. */
struct fbd_surface {
    uint8_t *mem;       /* first byte of the visible window */
    uint32_t width;     /* pixels (xres) */
    uint32_t height;    /* pixels (yres) */
    uint32_t stride;    /* bytes per line (line_length) */
    uint32_t bpp;       /* 16, 24 or 32 */
    struct fbd_chan red, green, blue, transp;
};

/* Fill in a surface. If the driver left every channel length at zero, fall
 * back to RGB565 (16 bpp) or XRGB8888 (24/32 bpp). Returns 0, or -1 for a
 * bpp we cannot draw. */
int fbd_init(struct fbd_surface *s, uint8_t *mem, uint32_t width, uint32_t height,
             uint32_t stride, uint32_t bpp, struct fbd_chan red, struct fbd_chan green,
             struct fbd_chan blue, struct fbd_chan transp);

/* Pack 8-bit RGB into the surface's pixel format. Alpha bits, if any, are set
 * to opaque so a blending display controller cannot hide the pattern. */
uint32_t fbd_pack(const struct fbd_surface *s, uint8_t r, uint8_t g, uint8_t b);

/* Read back one packed pixel (tests and sanity checks). Out of range -> 0. */
uint32_t fbd_get(const struct fbd_surface *s, int x, int y);

/* Draw primitives. All clip to the surface. */
void fbd_put(const struct fbd_surface *s, int x, int y, uint32_t px);
void fbd_fill(const struct fbd_surface *s, int x, int y, int w, int h, uint32_t px);
void fbd_border(const struct fbd_surface *s, uint32_t px);
void fbd_text(const struct fbd_surface *s, int x, int y, int scale, const char *str, uint32_t px);
int fbd_text_width(int scale, const char *str);
void fbd_dot(const struct fbd_surface *s, int x, int y, uint32_t px); /* square, radius max(3, unit/5) */

/* Map a raw input axis value in [min,max] onto [0,size-1]. Clamps; returns
 * -1 if the range is empty. */
int fbd_scale(int32_t value, int32_t min, int32_t max, uint32_t size);

/* Test pattern geometry, shared by the drawing code and the tests.
 * unit = min(width, height) / 20. Corner blocks are squares inset by
 * FBD_INSET pixels so the 1-pixel border stays visible next to them. */
#define FBD_INSET 2
uint32_t fbd_unit(const struct fbd_surface *s);
#define FBD_RED_UNITS   4   /* top-left,     largest  */
#define FBD_GREEN_UNITS 3   /* top-right              */
#define FBD_BLUE_UNITS  2   /* bottom-left            */
#define FBD_WHITE_UNITS 1   /* bottom-right, smallest */

/* The colour bars, left to right, as 8-bit RGB. */
#define FBD_NBARS 8
extern const uint8_t fbd_bars[FBD_NBARS][3];

/* Vertical extent of the bar band and the ramp band (tests sample inside). */
void fbd_bar_band(const struct fbd_surface *s, int *y0, int *h);
void fbd_ramp_band(const struct fbd_surface *s, int *y0, int *h);

/* Draw the full test pattern. `info` lines (NULL-terminated, may be NULL)
 * are printed in the middle of the screen, e.g. geometry and pixel format. */
void fbd_test_pattern(const struct fbd_surface *s, const char *const *info);

#endif
