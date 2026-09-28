/* ABOUTME: PATCH /api/v1/frame's region container: bounds and length checks, then decode all regions with lodepng
 * ABOUTME: before any pixel of the frame changes, so a bad batch leaves the frame exactly as it was. */
#include "regions.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "lodepng.h"
#include "sha256.h"

static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return get16(p) << 16 | get16(p + 2); }

static int fail(struct regions_error *e, int status, const char *code, int index, const char *message) {
    e->status = status;
    e->code = code;
    e->index = index;
    e->message = message;
    return -1;
}

int regions_parse(const uint8_t *body, size_t len, uint32_t logical_w, uint32_t logical_h, struct region *out,
                  int *n, struct regions_error *e) {
    if (len < REGIONS_HEAD || memcmp(body, "TT7R", 4) != 0)
        return fail(e, 400, "invalid_regions", -1, "the body must start with the 8-byte TT7R header");
    if (body[4] != REGIONS_VERSION)
        return fail(e, 400, "unsupported_regions_version", -1, "only container version 1 is supported");
    if (body[5] != 0) return fail(e, 400, "invalid_regions", -1, "header byte 5 is reserved and must be 0");
    uint32_t count = get16(body + 6);
    if (count > REGIONS_MAX) return fail(e, 400, "too_many_regions", -1, "at most 16 regions per batch");
    size_t head = REGIONS_HEAD + (size_t)count * REGIONS_RECORD;
    if (len < head) return fail(e, 400, "invalid_regions", -1, "the body ends inside the region records");

    uint64_t area = 0;
    size_t off = head;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *rec = body + REGIONS_HEAD + (size_t)i * REGIONS_RECORD;
        struct region *r = &out[i];
        r->x = get16(rec);
        r->y = get16(rec + 2);
        r->w = get16(rec + 4);
        r->h = get16(rec + 6);
        r->png_len = get32(rec + 8);
        if (!r->w || !r->h || r->x >= logical_w || r->y >= logical_h || r->w > logical_w - r->x ||
            r->h > logical_h - r->y)
            return fail(e, 422, "region_out_of_bounds", (int)i,
                        "each region must be at least 1x1 and lie inside the logical screen");
        area += (uint64_t)r->w * r->h;
        if (r->png_len > len - off)
            return fail(e, 400, "invalid_regions", (int)i, "a region's PNG runs past the end of the body");
        r->png = body + off;
        off += r->png_len;
    }
    if (off != len) return fail(e, 400, "invalid_regions", -1, "bytes follow the last region's PNG");
    if (area > (uint64_t)logical_w * logical_h)
        return fail(e, 422, "regions_too_large", -1, "the regions cover more pixels than the screen has");
    *n = (int)count;
    return 0;
}

int regions_apply(const struct region *r, int n, uint8_t *rgba, uint32_t logical_w, uint32_t logical_h,
                  struct regions_error *e) {
    (void)logical_h; /* regions_parse already kept every rect inside it */
    uint8_t *pix[REGIONS_MAX] = {0};
    int rc = 0;
    for (int i = 0; i < n && rc == 0; i++) {
        unsigned w = 0, h = 0;
        LodePNGState st;
        lodepng_state_init(&st);
        unsigned err = lodepng_inspect(&w, &h, &st, r[i].png, r[i].png_len);
        lodepng_state_cleanup(&st);
        if (!err && (w != r[i].w || h != r[i].h)) {
            e->expected_w = r[i].w, e->expected_h = r[i].h, e->received_w = w, e->received_h = h;
            rc = fail(e, 422, "region_size_mismatch", i, "a region's PNG must be exactly its record's w x h");
            break;
        }
        if (!err) err = lodepng_decode32(&pix[i], &w, &h, r[i].png, r[i].png_len);
        if (err) {
            e->detail = lodepng_error_text(err);
            rc = fail(e, 422, "invalid_image", i, "a region is not a valid PNG");
        }
    }
    if (rc == 0) /* every region decoded: nothing below can fail */
        for (int i = 0; i < n; i++)
            for (uint32_t y = 0; y < r[i].h; y++)
                memcpy(rgba + ((size_t)(r[i].y + y) * logical_w + r[i].x) * 4, pix[i] + (size_t)y * r[i].w * 4,
                       (size_t)r[i].w * 4);
    for (int i = 0; i < n; i++) free(pix[i]);
    return rc;
}

int regions_frame_sha(const char *base_sha, const uint8_t *body, size_t len, char out[65]) {
    uint8_t *buf = malloc(64 + len);
    if (!buf) return -1;
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)tolower((unsigned char)base_sha[i]);
    memcpy(buf + 64, body, len);
    sha256_hex(buf, 64 + len, out);
    free(buf);
    return 0;
}
