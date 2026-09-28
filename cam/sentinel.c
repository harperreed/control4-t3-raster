/* ABOUTME: Counts NV12 rows that still hold the 0xAA sentinel after a capture.
 * ABOUTME: All rows untouched = no capture; some = partial capture; none = a real frame. */
#include "sentinel.h"

#include <string.h>

void sentinel_fill(uint8_t *nv12, int w, int h) {
    memset(nv12, CAM_SENTINEL, (size_t)w * h * 3 / 2);
}

static int row_is_sentinel(const uint8_t *row, int w) {
    for (int i = 0; i < w; i++)
        if (row[i] != CAM_SENTINEL) return 0;
    return 1;
}

void sentinel_check(const uint8_t *nv12, int w, int h, struct sentinel_report *out) {
    int y_rows = 0, uv_rows = 0;
    for (int r = 0; r < h; r++) y_rows += row_is_sentinel(nv12 + (size_t)r * w, w);
    const uint8_t *uv = nv12 + (size_t)w * h;
    for (int r = 0; r < h / 2; r++) uv_rows += row_is_sentinel(uv + (size_t)r * w, w);

    out->y_rows_sentinel = y_rows;
    out->uv_rows_sentinel = uv_rows;
    if (y_rows == h && uv_rows == h / 2)
        out->verdict = FRAME_UNTOUCHED;
    else if (y_rows || uv_rows)
        out->verdict = FRAME_PARTIAL;
    else
        out->verdict = FRAME_CAPTURED;
}

const char *frame_verdict_name(enum frame_verdict v) {
    switch (v) {
    case FRAME_CAPTURED: return "captured";
    case FRAME_UNTOUCHED: return "untouched";
    case FRAME_PARTIAL: return "partial";
    }
    return "?";
}

void luma_stats(const uint8_t *luma, size_t n, struct luma_stats *out) {
    uint8_t lo = 255, hi = 0;
    uint64_t sum = 0;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        uint8_t v = luma[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sum += v;
        hash = (hash ^ v) * 16777619u;
    }
    out->min = n ? lo : 0;
    out->max = n ? hi : 0;
    out->mean = n ? (double)sum / (double)n : 0.0;
    out->hash = hash;
}

int luma_is_flat(const struct luma_stats *s) { return s->max - s->min <= LUMA_FLAT_RANGE; }
