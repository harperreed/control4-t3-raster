/* ABOUTME: Baseline JPEG encoding of RGB888 or NV12 frames into a heap buffer (stb_image_write).
 * ABOUTME: No file I/O here; callers write the bytes wherever they need them. */
#ifndef CAM_JPEG_H
#define CAM_JPEG_H

#include <stddef.h>
#include <stdint.h>

struct jpeg_buf {
    uint8_t *data; /* malloc'd; release with jpeg_buf_free */
    size_t len;
    size_t cap;
    int failed; /* set if a realloc failed mid-encode */
};

/* quality 1..100. Returns 0 on success, -1 on bad arguments or out of memory. */
int jpeg_encode_rgb(const uint8_t *rgb, int w, int h, int quality, struct jpeg_buf *out);

/* Converts with nv12_to_rgb (BT.601 limited range) and encodes. w, h even. */
int jpeg_encode_nv12(const uint8_t *nv12, int w, int h, int quality, struct jpeg_buf *out);

void jpeg_buf_free(struct jpeg_buf *b);

#endif
