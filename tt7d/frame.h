/* ABOUTME: The current frame: a PUT PNG or a PATCH of regions onto it, dedup, persistence, and its metadata.
 * ABOUTME: A frame reaches the screen only after it fully decoded (and persisted, if asked). */
#ifndef TT7D_FRAME_H
#define TT7D_FRAME_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "display.h"
#include "http.h"
#include "json.h"
#include "server.h"

struct frame_store {
    const char *data_dir;
    struct display *disp;

    int have;             /* a frame has been shown (it may be covered by the fallback clock) */
    int on_screen;        /* that frame is what the screen shows now; the fallback clears it */
    char id[129];         /* "" when unknown (a restored frame without its id file) */
    char sha256[65];      /* a PUT: sha256 of the PNG; a PATCH: regions_frame_sha (SPEC §10.1) */
    uint8_t *rgba;        /* the frame's logical RGBA pixels: what PATCH applies to */
    uint8_t *png;         /* GET /frame/image: the PNG as received, or NULL after a PATCH until encoded */
    size_t png_len;
    size_t update_bytes;  /* the body of the PUT or PATCH that made this frame */
    int regions;          /* regions in the PATCH that made this frame, -1 for a whole PNG */
    int received_known;   /* 0 for a frame restored from disk */
    struct timespec received_at;   /* wall clock of the last PUT of this frame */
    struct timespec received_mono; /* monotonic, for frame_age_s */
    struct timespec displayed_at;  /* wall clock of the last redraw */
    int persisted;        /* this frame is the one in last-frame.png */
    int deduplicated;     /* the last PUT matched and skipped the redraw */
    int restored;         /* shown from last-frame.png at startup */

    unsigned long accepted, dedup_count, rejected, region_updates;
    const char *last_error; /* the last rejection's error code, or NULL */
};

void frame_store_init(struct frame_store *fs, const char *data_dir, struct display *disp);

/* Show <data_dir>/last-frame.png if it exists and fits. Returns 1 if shown,
 * 0 if there is none, -1 (with err) if it exists but cannot be shown. */
int frame_restore(struct frame_store *fs, size_t max_bytes, char *err, size_t errlen);

/* Header checks for PUT /api/v1/frame, before the body is read: bearer
 * token, Content-Type, X-Frame-ID, X-Frame-SHA256 syntax, X-Persist.
 * Returns 0, or fills resp with an error. */
int frame_check_head(const char *token, const struct http_request *req, struct response *resp);

/* Header checks for PATCH /api/v1/frame: bearer token, Content-Type
 * application/x-tt7-regions, X-Base-Frame-ID (required), X-Frame-ID,
 * X-Frame-SHA256 syntax, X-Persist (true is refused). */
int frame_check_patch_head(const char *token, const struct http_request *req, struct response *resp);

/* PATCH /api/v1/frame: apply a batch of regions to the frame named by
 * X-Base-Frame-ID, all or nothing. covered_by is the id of what covers the
 * frame (the fallback clock), or NULL: a covered frame is not a base. */
void frame_patch(struct frame_store *fs, const struct http_request *req, const uint8_t *body, size_t len,
                 const char *covered_by, struct response *resp);

/* GET /api/v1/frame/image: the frame as PNG, encoded on the first request
 * after a PATCH and kept until the frame changes. */
void frame_image(struct frame_store *fs, struct response *resp);

/* Encode logical RGBA as an RGB PNG with fast settings (see frame.c).
 * Returns 0, or -1. The caller frees *png. */
int frame_encode_png(const uint8_t *rgba, unsigned w, unsigned h, uint8_t **png, size_t *len);

/* PUT /api/v1/frame with the whole body. */
void frame_put(struct frame_store *fs, const struct http_request *req, const uint8_t *body, size_t len,
               struct response *resp);

/* Validate, decode and show a PNG whose SHA-256 (hex) is already known:
 * the part of PUT /api/v1/frame after its headers, also used for the
 * built-in test pattern. The same pixels as on screen only update the id and
 * receipt time. Fills resp with the frame metadata (200) or an error. */
void frame_show(struct frame_store *fs, const uint8_t *png, size_t len, const char *sha, const char *id, int persist,
                struct response *resp);

/* The frame metadata object (GET /api/v1/frame and the PUT reply). */
void frame_json(const struct frame_store *fs, struct sbuf *sb);

#endif
