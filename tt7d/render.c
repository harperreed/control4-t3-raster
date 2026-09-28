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

void render_unmap(int rotation, uint32_t native_w, uint32_t native_h, uint32_t nx, uint32_t ny, uint32_t *x,
                  uint32_t *y) {
    /* Turning back by `rotation` is turning forward by 360 - rotation, with
     * the native framebuffer as the image being turned. */
    render_map((360 - rotation) % 360, native_w, native_h, nx, ny, x, y);
}

void render_rgba(const struct fbd_surface *dst, int rotation, const uint8_t *rgba, uint32_t logical_w,
                 uint32_t logical_h) {
    render_rgba_rect(dst, rotation, rgba, logical_w, logical_h, 0, 0, logical_w, logical_h);
}

void render_rgba_rect(const struct fbd_surface *dst, int rotation, const uint8_t *rgba, uint32_t logical_w,
                      uint32_t logical_h, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    for (uint32_t ly = y; ly < y + h; ly++) {
        const uint8_t *px = rgba + ((size_t)ly * logical_w + x) * 4;
        for (uint32_t lx = x; lx < x + w; lx++, px += 4) {
            uint32_t nx, ny;
            render_map(rotation, logical_w, logical_h, lx, ly, &nx, &ny);
            fbd_put(dst, (int)nx, (int)ny, fbd_pack(dst, px[0], px[1], px[2]));
        }
    }
}

void render_native_rect(int rotation, uint32_t logical_w, uint32_t logical_h, uint32_t x, uint32_t y, uint32_t w,
                        uint32_t h, uint32_t *nx, uint32_t *ny, uint32_t *nw, uint32_t *nh) {
    /* A right-angle turn maps a rect's opposite corners to opposite corners. */
    uint32_t ax, ay, bx, by;
    render_map(rotation, logical_w, logical_h, x, y, &ax, &ay);
    render_map(rotation, logical_w, logical_h, x + w - 1, y + h - 1, &bx, &by);
    *nx = ax < bx ? ax : bx;
    *ny = ay < by ? ay : by;
    *nw = (ax < bx ? bx - ax : ax - bx) + 1;
    *nh = (ay < by ? by - ay : ay - by) + 1;
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
