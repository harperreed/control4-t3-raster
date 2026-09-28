/* ABOUTME: Rotation mapping and RGBA -> native pixel conversion from the logical image to the framebuffer.
 * ABOUTME: Packing goes through fbdraw's fbd_pack/fbd_put, so any channel layout the driver reports works. */
#include "render.h"

#include <stddef.h>

int render_rotation_valid(int rotation) {
    return rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270;
}

void render_logical_size(uint32_t native_w, uint32_t native_h, int rotation, uint32_t *logical_w,
                         uint32_t *logical_h) {
    int swap = rotation == 90 || rotation == 270;
    *logical_w = swap ? native_h : native_w;
    *logical_h = swap ? native_w : native_h;
}

void render_map(int rotation, uint32_t logical_w, uint32_t logical_h, uint32_t x, uint32_t y, uint32_t *nx,
                uint32_t *ny) {
    switch (rotation) {
    case 90: /* clockwise: the top-left corner goes to the native top-right */
        *nx = logical_h - 1 - y;
        *ny = x;
        break;
    case 180:
        *nx = logical_w - 1 - x;
        *ny = logical_h - 1 - y;
        break;
    case 270: /* counter-clockwise: the top-left corner goes to the native bottom-left */
        *nx = y;
        *ny = logical_w - 1 - x;
        break;
    default:
        *nx = x;
        *ny = y;
    }
}

void render_rgba(const struct fbd_surface *dst, int rotation, const uint8_t *rgba, uint32_t logical_w,
                 uint32_t logical_h) {
    for (uint32_t y = 0; y < logical_h; y++) {
        const uint8_t *px = rgba + (size_t)y * logical_w * 4;
        for (uint32_t x = 0; x < logical_w; x++, px += 4) {
            uint32_t nx, ny;
            render_map(rotation, logical_w, logical_h, x, y, &nx, &ny);
            fbd_put(dst, (int)nx, (int)ny, fbd_pack(dst, px[0], px[1], px[2]));
        }
    }
}

static int chan_is(struct fbd_chan c, uint32_t offset, uint32_t length) {
    return c.offset == offset && c.length == length;
}

const char *render_format_name(const struct fbd_surface *s) {
    if (s->bpp == 16 && chan_is(s->red, 11, 5) && chan_is(s->green, 5, 6) && chan_is(s->blue, 0, 5))
        return "rgb565";
    if (s->bpp == 32 && chan_is(s->red, 16, 8) && chan_is(s->green, 8, 8) && chan_is(s->blue, 0, 8))
        return s->transp.length == 0 ? "xrgb8888" : "argb8888";
    return "unknown";
}
