/* ABOUTME: Wraps stb_image_write's JPEG encoder so it writes into a growable memory buffer.
 * ABOUTME: stb is compiled here, without its stdio helpers (third_party/stb/PROVENANCE). */
#include "jpeg.h"

#include <stdlib.h>
#include <string.h>

#include "yuv.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

static void append(void *ctx, void *data, int size) {
    struct jpeg_buf *b = ctx;
    if (b->failed || size <= 0) return;
    if (b->len + (size_t)size > b->cap) {
        size_t cap = b->cap ? b->cap : 64 * 1024;
        while (cap < b->len + (size_t)size) cap *= 2;
        uint8_t *grown = realloc(b->data, cap);
        if (!grown) {
            b->failed = 1;
            return;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, (size_t)size);
    b->len += (size_t)size;
}

int jpeg_encode_rgb(const uint8_t *rgb, int w, int h, int quality, struct jpeg_buf *out) {
    memset(out, 0, sizeof *out);
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535 || quality < 1 || quality > 100) return -1;
    int ok = stbi_write_jpg_to_func(append, out, w, h, 3, rgb, quality);
    if (!ok || out->failed) {
        jpeg_buf_free(out);
        return -1;
    }
    return 0;
}

int jpeg_encode_nv12(const uint8_t *nv12, int w, int h, int quality, struct jpeg_buf *out) {
    memset(out, 0, sizeof *out);
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) return -1;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return -1;
    nv12_to_rgb(nv12, w, h, rgb);
    int rc = jpeg_encode_rgb(rgb, w, h, quality, out);
    free(rgb);
    return rc;
}

void jpeg_buf_free(struct jpeg_buf *b) {
    free(b->data);
    memset(b, 0, sizeof *b);
}
