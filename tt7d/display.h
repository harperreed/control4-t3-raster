/* ABOUTME: The framebuffer tt7d draws on: a real /dev/fbN or a file-backed fake for host tests,
 * ABOUTME: plus a RAM back buffer so a frame is converted in full before one copy to the screen. */
#ifndef TT7D_DISPLAY_H
#define TT7D_DISPLAY_H

#include <linux/fb.h>
#include <stddef.h>
#include <stdint.h>

#include "fbdraw.h"

struct display {
    int fd;
    int is_file;
    char device[256];
    uint8_t *map;
    size_t map_len;
    uint8_t *window;         /* the visible xres*yres window inside map */
    size_t frame_len;        /* stride * height */
    struct fbd_surface back; /* the back buffer (RAM), same layout as the window */
    struct fb_var_screeninfo var;
    int rotation;
    uint32_t logical_w, logical_h;
};

/* Open a real framebuffer device. Returns 0, or -1 with a message in err. */
int display_open_fb(struct display *d, const char *path, int rotation, char *err, size_t errlen);

/* Open a regular file as a framebuffer: geometry "WxHxBPP" (bpp 16 or 32),
 * stride in bytes (0 = W*BPP/8), format "rgb565" or "xrgb8888". The file is
 * created or grown to stride*H bytes. Returns 0, or -1 with a message. */
int display_open_file(struct display *d, const char *path, const char *geometry, uint32_t stride,
                      const char *format, int rotation, char *err, size_t errlen);

/* Convert a logical_w x logical_h RGBA image into the back buffer. */
void display_draw(struct display *d, const uint8_t *rgba);

/* Copy the back buffer to the screen in one pass. */
void display_present(struct display *d);

/* Convert only the logical rect (x, y, w, h) of a full logical_w x logical_h
 * RGBA image into the back buffer. */
void display_draw_rect(struct display *d, const uint8_t *rgba, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Copy only the native pixels under the logical rect (x, y, w, h) from the
 * back buffer to the screen. */
void display_present_rect(struct display *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

#endif
