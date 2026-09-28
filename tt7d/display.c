/* ABOUTME: Opens the framebuffer (device or file), maps it, and presents frames from a RAM back buffer.
 * ABOUTME: Reads geometry and channel layout from the fb ioctls; the pixel packing lives in fbdraw.c. */
#include "display.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "render.h"

/* Shared tail of both open paths: allocate the back buffer, describe it,
 * and derive the logical size. */
static int finish_open(struct display *d, uint32_t w, uint32_t h, uint32_t stride, uint32_t bpp, struct fbd_chan r,
                       struct fbd_chan g, struct fbd_chan b, struct fbd_chan a, int rotation, char *err,
                       size_t errlen) {
    d->frame_len = (size_t)stride * h;
    uint8_t *back = calloc(1, d->frame_len);
    if (!back) {
        snprintf(err, errlen, "cannot allocate a %zu-byte back buffer", d->frame_len);
        return -1;
    }
    if (fbd_init(&d->back, back, w, h, stride, bpp, r, g, b, a) != 0) {
        free(back);
        snprintf(err, errlen, "cannot draw %u bits per pixel", bpp);
        return -1;
    }
    d->rotation = rotation;
    render_logical_size(w, h, rotation, &d->logical_w, &d->logical_h);
    return 0;
}

int display_open_fb(struct display *d, const char *path, int rotation, char *err, size_t errlen) {
    memset(d, 0, sizeof *d);
    snprintf(d->device, sizeof d->device, "%s", path);
    d->fd = open(path, O_RDWR | O_CLOEXEC);
    if (d->fd < 0) {
        snprintf(err, errlen, "open %s: %s", path, strerror(errno));
        return -1;
    }
    struct fb_fix_screeninfo fix;
    if (ioctl(d->fd, FBIOGET_VSCREENINFO, &d->var) != 0 || ioctl(d->fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        snprintf(err, errlen, "%s: fb ioctls: %s", path, strerror(errno));
        return -1;
    }
    /* The TT7 driver puts Rockchip-private values in grayscale and nonstd
     * (hardware/discovery: grayscale 1342382080, nonstd 4); only the channel
     * bitfields describe the pixel layout, so those two are ignored. */
    const struct fb_var_screeninfo *v = &d->var;
    d->map_len = fix.smem_len;
    d->map = mmap(NULL, d->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (d->map == MAP_FAILED) {
        d->map = NULL;
        snprintf(err, errlen, "mmap %s (%zu bytes): %s", path, d->map_len, strerror(errno));
        return -1;
    }
    size_t off = (size_t)v->yoffset * fix.line_length + (size_t)v->xoffset * (v->bits_per_pixel / 8);
    if (off + (size_t)v->yres * fix.line_length > d->map_len) off = 0;
    if ((size_t)v->yres * fix.line_length > d->map_len) {
        snprintf(err, errlen, "%s: %u lines of %u bytes overrun smem_len %zu", path, v->yres, fix.line_length,
                 d->map_len);
        return -1;
    }
    d->window = d->map + off;
    if (ioctl(d->fd, FBIOBLANK, FB_BLANK_UNBLANK) != 0)
        fprintf(stderr, "tt7d: FBIOBLANK unblank (harmless if unsupported): %s\n", strerror(errno));
    struct fbd_chan r = {v->red.offset, v->red.length}, g = {v->green.offset, v->green.length};
    struct fbd_chan b = {v->blue.offset, v->blue.length}, a = {v->transp.offset, v->transp.length};
    return finish_open(d, v->xres, v->yres, fix.line_length, v->bits_per_pixel, r, g, b, a, rotation, err, errlen);
}

int display_open_file(struct display *d, const char *path, const char *geometry, uint32_t stride,
                      const char *format, int rotation, char *err, size_t errlen) {
    memset(d, 0, sizeof *d);
    d->is_file = 1;
    snprintf(d->device, sizeof d->device, "%s", path);
    unsigned w, h, bpp;
    char tail;
    if (!geometry || sscanf(geometry, "%ux%ux%u%c", &w, &h, &bpp, &tail) != 3 || !w || !h || (bpp != 16 && bpp != 32)) {
        snprintf(err, errlen, "--fb-geometry wants WxHxBPP with BPP 16 or 32, got '%s'", geometry ? geometry : "");
        return -1;
    }
    if (!stride) stride = w * bpp / 8;
    if (stride < w * bpp / 8) {
        snprintf(err, errlen, "--fb-stride %u is shorter than a %u-pixel line", stride, w);
        return -1;
    }
    struct fbd_chan r, g, b, none = {0, 0};
    if (!format) format = bpp == 16 ? "rgb565" : "xrgb8888";
    if (strcmp(format, "rgb565") == 0 && bpp == 16) {
        r = (struct fbd_chan){11, 5}, g = (struct fbd_chan){5, 6}, b = (struct fbd_chan){0, 5};
    } else if (strcmp(format, "xrgb8888") == 0 && bpp == 32) {
        r = (struct fbd_chan){16, 8}, g = (struct fbd_chan){8, 8}, b = (struct fbd_chan){0, 8};
    } else {
        snprintf(err, errlen, "--fb-format %s does not fit %u bpp (rgb565 = 16, xrgb8888 = 32)", format, bpp);
        return -1;
    }
    d->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    struct stat st;
    size_t need = (size_t)stride * h;
    if (d->fd < 0 || fstat(d->fd, &st) != 0 || ((size_t)st.st_size < need && ftruncate(d->fd, (off_t)need) != 0)) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    d->map_len = need;
    d->map = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (d->map == MAP_FAILED) {
        d->map = NULL;
        snprintf(err, errlen, "mmap %s: %s", path, strerror(errno));
        return -1;
    }
    d->window = d->map;
    return finish_open(d, w, h, stride, bpp, r, g, b, none, rotation, err, errlen);
}

void display_draw(struct display *d, const uint8_t *rgba) {
    render_rgba(&d->back, d->rotation, rgba, d->logical_w, d->logical_h);
}

void display_present(struct display *d) {
    memcpy(d->window, d->back.mem, d->frame_len);
    if (d->is_file) return;
    /* Some drivers latch new contents only on a pan (as tt7probe does). */
    static int pan_warned;
    if (ioctl(d->fd, FBIOPAN_DISPLAY, &d->var) != 0 && !pan_warned) {
        fprintf(stderr, "tt7d: FBIOPAN_DISPLAY (harmless if unsupported): %s\n", strerror(errno));
        pan_warned = 1;
    }
}
