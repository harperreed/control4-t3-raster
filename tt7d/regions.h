/* ABOUTME: The body of PATCH /api/v1/frame: a small binary container of PNG regions (SPEC §10.1 has the layout).
 * ABOUTME: Parse and validate it, decode every region before touching the frame, and name the patched frame's SHA-256. */
#ifndef TT7D_REGIONS_H
#define TT7D_REGIONS_H

#include <stddef.h>
#include <stdint.h>

#define REGIONS_CONTENT_TYPE "application/x-tt7-regions"
#define REGIONS_VERSION 1
#define REGIONS_MAX 16    /* regions in one batch */
#define REGIONS_HEAD 8    /* "TT7R", version, reserved, count */
#define REGIONS_RECORD 12 /* x, y, w, h (u16 each), png_len (u32), big-endian */

/* One region: a PNG of exactly w x h pixels for the logical rect at (x, y). */
struct region {
    uint32_t x, y, w, h;
    const uint8_t *png; /* points into the request body */
    size_t png_len;
};

/* Why a batch was refused, for the HTTP reply. */
struct regions_error {
    int status;          /* 400 or 422 */
    const char *code;    /* stable error code */
    const char *message;
    int index;           /* the region at fault, or -1 for the container itself */
    uint32_t expected_w, expected_h, received_w, received_h; /* region_size_mismatch */
    const char *detail;  /* invalid_image: lodepng's reason */
};

/* Check the container against a logical_w x logical_h screen and fill out[]
 * (at most REGIONS_MAX) and *n. Nothing is decoded. Returns 0, or -1 with e. */
int regions_parse(const uint8_t *body, size_t len, uint32_t logical_w, uint32_t logical_h, struct region *out,
                  int *n, struct regions_error *e);

/* Decode every region and check its size first; only when all are good,
 * copy them in order onto rgba (logical_w x logical_h RGBA; a later region
 * wins where two overlap). Returns 0, or -1 with e and rgba untouched. */
int regions_apply(const struct region *r, int n, uint8_t *rgba, uint32_t logical_w, uint32_t logical_h,
                  struct regions_error *e);

/* The patched frame's SHA-256: sha256 of the base frame's SHA-256 as 64
 * lowercase hex digits followed by the whole request body. Returns 0, or -1
 * when out of memory. */
int regions_frame_sha(const char *base_sha, const uint8_t *body, size_t len, char out[65]);

#endif
