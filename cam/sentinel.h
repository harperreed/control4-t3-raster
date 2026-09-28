/* ABOUTME: Sentinel fill and check that prove the camera really wrote an NV12 frame.
 * ABOUTME: Pure functions: fill 0xAA before QBUF, count untouched rows after, and summarise luma. */
#ifndef CAM_SENTINEL_H
#define CAM_SENTINEL_H

#include <stddef.h>
#include <stdint.h>

/* The RK CIF driver copies each frame into our buffer with ipp_blit_sync().
 * Without the rk29_ipp module that call is a stub returning 0, so DQBUF
 * succeeds and the buffer keeps whatever it held (MMKeypad CAMERA.md). */
#define CAM_SENTINEL 0xAA

enum frame_verdict {
    FRAME_CAPTURED,  /* no row is all sentinel */
    FRAME_UNTOUCHED, /* every row is still all sentinel: nothing was written */
    FRAME_PARTIAL,   /* some rows (luma or chroma) are all sentinel */
};

struct sentinel_report {
    enum frame_verdict verdict;
    int y_rows_sentinel;  /* luma rows still all 0xAA, of h */
    int uv_rows_sentinel; /* chroma rows still all 0xAA, of h/2 */
};

/* Fills the w*h*3/2 bytes of an NV12 frame with CAM_SENTINEL. */
void sentinel_fill(uint8_t *nv12, int w, int h);

/* A real sensor row of w bytes all equal to 0xAA does not happen in
 * practice (noise alone breaks it), so a whole-row match means "not written". */
void sentinel_check(const uint8_t *nv12, int w, int h, struct sentinel_report *out);

const char *frame_verdict_name(enum frame_verdict v);

/* Luma summary of a frame. A written but flat frame (range <= LUMA_FLAT_RANGE)
 * passes the sentinel check yet holds no picture: an IPP copy of an idle CIF
 * buffer, a sleeping sensor, or a covered lens. hash tells frames apart. */
#define LUMA_FLAT_RANGE 8
struct luma_stats {
    uint8_t min, max;
    double mean;
    uint32_t hash; /* FNV-1a over the luma bytes */
};
void luma_stats(const uint8_t *luma, size_t n, struct luma_stats *out);
int luma_is_flat(const struct luma_stats *s);

#endif
