/* ABOUTME: Host test for sentinel.c: tells a captured NV12 frame from one the camera never wrote.
 * ABOUTME: Covers untouched, fully written, half written, chroma-only-missing and lucky-0xAA frames. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sentinel.h"
#include "test_common.h"

enum { W = 64, H = 32, SIZE = W * H * 3 / 2 };

/* Deterministic "camera" data: a gradient with noise, never a whole row of 0xAA. */
static void fill_scene(uint8_t *f) {
    uint32_t seed = 12345;
    for (int i = 0; i < SIZE; i++) {
        seed = seed * 1103515245u + 12345u;
        f[i] = (uint8_t)((i % W) * 3 + (seed >> 24) % 7);
    }
}

static struct sentinel_report run(const uint8_t *f) {
    struct sentinel_report r;
    sentinel_check(f, W, H, &r);
    return r;
}

int main(void) {
    uint8_t *f = malloc(SIZE);

    sentinel_fill(f, W, H);
    for (int i = 0; i < SIZE; i++)
        CHECK(f[i] == CAM_SENTINEL, "byte %d = 0x%02x after fill", i, f[i]);
    struct sentinel_report r = run(f);
    CHECK(r.verdict == FRAME_UNTOUCHED, "fresh fill: %s", frame_verdict_name(r.verdict));
    CHECK(r.y_rows_sentinel == H && r.uv_rows_sentinel == H / 2, "rows %d/%d", r.y_rows_sentinel,
          r.uv_rows_sentinel);

    fill_scene(f);
    r = run(f);
    CHECK(r.verdict == FRAME_CAPTURED, "scene: %s", frame_verdict_name(r.verdict));
    CHECK(r.y_rows_sentinel == 0 && r.uv_rows_sentinel == 0, "rows %d/%d", r.y_rows_sentinel, r.uv_rows_sentinel);

    /* Scattered 0xAA bytes are normal image data, not a missed write. */
    for (int i = 0; i < SIZE; i += 3) f[i] = CAM_SENTINEL;
    r = run(f);
    CHECK(r.verdict == FRAME_CAPTURED, "scattered 0xAA: %s", frame_verdict_name(r.verdict));

    /* DMA stopped halfway down the luma plane. */
    fill_scene(f);
    memset(f + W * (H / 2), CAM_SENTINEL, W * (H / 2));
    r = run(f);
    CHECK(r.verdict == FRAME_PARTIAL, "bottom half missing: %s", frame_verdict_name(r.verdict));
    CHECK(r.y_rows_sentinel == H / 2, "y rows %d", r.y_rows_sentinel);

    /* Luma written but the chroma plane never was (e.g. a wrong UV address). */
    fill_scene(f);
    memset(f + W * H, CAM_SENTINEL, W * H / 2);
    r = run(f);
    CHECK(r.verdict == FRAME_PARTIAL, "chroma missing: %s", frame_verdict_name(r.verdict));
    CHECK(r.uv_rows_sentinel == H / 2, "uv rows %d", r.uv_rows_sentinel);

    /* One untouched row is enough to call it partial. */
    fill_scene(f);
    memset(f + W * 5, CAM_SENTINEL, W);
    r = run(f);
    CHECK(r.verdict == FRAME_PARTIAL, "one row missing: %s", frame_verdict_name(r.verdict));

    CHECK(strcmp(frame_verdict_name(FRAME_UNTOUCHED), "untouched") == 0, "name");

    /* Luma stats: a written-but-flat frame is flagged, a real one is not. */
    struct luma_stats st;
    memset(f, 16, W * H);
    luma_stats(f, W * H, &st);
    CHECK(st.min == 16 && st.max == 16 && st.mean == 16.0 && luma_is_flat(&st), "flat black %u..%u %.1f", st.min,
          st.max, st.mean);
    uint32_t flat_hash = st.hash;
    fill_scene(f);
    luma_stats(f, W * H, &st);
    CHECK(!luma_is_flat(&st), "scene flagged flat: %u..%u", st.min, st.max);
    CHECK(st.hash != flat_hash, "hash did not change");
    uint8_t two[2] = {10, 30};
    luma_stats(two, 2, &st);
    CHECK(st.min == 10 && st.max == 30 && st.mean == 20.0, "two bytes %u..%u %.1f", st.min, st.max, st.mean);
    free(f);
    return test_finish("cam sentinel: untouched / partial / captured frames, luma stats");
}
