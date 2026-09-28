/* ABOUTME: Maps the logical (landscape) image onto the native framebuffer: rotation plus RGBA -> native pixels.
 * ABOUTME: Pixel packing reuses probe/fbdraw.c, so tt7probe and tt7d share one format implementation. */
#ifndef TT7D_RENDER_H
#define TT7D_RENDER_H

#include <stdint.h>

#include "fbdraw.h"

/* `rotation` is how many degrees clockwise the logical image is turned to land
 * on the native framebuffer: 0, 90, 180 or 270. */
int render_rotation_valid(int rotation);

/* Logical size for a native size and rotation (90/270 swap the axes). */
void render_logical_size(uint32_t native_w, uint32_t native_h, int rotation, uint32_t *logical_w,
                         uint32_t *logical_h);

/* Where logical pixel (x, y) of a logical_w x logical_h image lands natively. */
void render_map(int rotation, uint32_t logical_w, uint32_t logical_h, uint32_t x, uint32_t y, uint32_t *nx,
                uint32_t *ny);

/* The inverse: which logical pixel shows at native pixel (nx, ny) of a
 * native_w x native_h framebuffer. Touch input uses it, so a touch maps back
 * through the same code that drew the frame. */
void render_unmap(int rotation, uint32_t native_w, uint32_t native_h, uint32_t nx, uint32_t ny, uint32_t *x,
                  uint32_t *y);

/* Draw a logical_w x logical_h RGBA8888 image (row-major, 4 bytes per pixel)
 * onto `dst`, rotated. The caller guarantees the logical size matches
 * render_logical_size(dst->width, dst->height, rotation). Alpha is ignored:
 * the display has no alpha, and the RGB values are used as sent. */
void render_rgba(const struct fbd_surface *dst, int rotation, const uint8_t *rgba, uint32_t logical_w,
                 uint32_t logical_h);

/* Draw only the rect (x, y, w, h) of a full logical_w x logical_h RGBA image,
 * rotated exactly as render_rgba would: a region update redraws its pixels
 * and leaves the rest of `dst` alone. The caller keeps the rect inside the
 * logical image. */
void render_rgba_rect(const struct fbd_surface *dst, int rotation, const uint8_t *rgba, uint32_t logical_w,
                      uint32_t logical_h, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* The native rect that the logical rect (x, y, w, h) lands on (w, h >= 1). */
void render_native_rect(int rotation, uint32_t logical_w, uint32_t logical_h, uint32_t x, uint32_t y, uint32_t w,
                        uint32_t h, uint32_t *nx, uint32_t *ny, uint32_t *nw, uint32_t *nh);

/* "rgb565", "xrgb8888", "argb8888", or "unknown" for any other channel layout. */
const char *render_format_name(const struct fbd_surface *s);

#endif
