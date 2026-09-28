/* ABOUTME: Host unit tests for render.c: RGBA -> RGB565/XRGB8888 values and all four rotations.
 * ABOUTME: Expected native layouts are written out by hand, not computed with render_map. */
#include <string.h>

#include "render.h"
#include "test_common.h"

static const struct fbd_chan NONE = {0, 0};
static const struct fbd_chan R565 = {11, 5}, G565 = {5, 6}, B565 = {0, 5};
static const struct fbd_chan R8 = {16, 8}, G8 = {8, 8}, B8 = {0, 8};

/* Logical 4x2 image whose pixel i has red = i << 3, so its RGB565 value is
 * i << 11 and each native pixel says which logical pixel landed there. */
static void make_indexed(uint8_t rgba[4 * 2 * 4]) {
    for (int i = 0; i < 8; i++) {
        rgba[4 * i + 0] = (uint8_t)(i << 3);
        rgba[4 * i + 1] = 0;
        rgba[4 * i + 2] = 0;
        rgba[4 * i + 3] = 255;
    }
}

/* Render the indexed 4x2 image and compare the native grid (as logical
 * indices, row-major) to `want`. stride_px > native width leaves padding
 * that must stay untouched. */
static void check_rotation(int rotation, uint32_t nw, uint32_t nh, const int *want) {
    uint32_t stride_px = nw + 1;
    uint8_t mem[64];
    memset(mem, 0xA5, sizeof mem);
    struct fbd_surface s;
    fbd_init(&s, mem, nw, nh, stride_px * 2, 16, R565, G565, B565, NONE);

    uint32_t lw, lh;
    render_logical_size(nw, nh, rotation, &lw, &lh);
    CHECK(lw == 4 && lh == 2, "rotation %d: logical %ux%u", rotation, lw, lh);

    uint8_t rgba[32];
    make_indexed(rgba);
    render_rgba(&s, rotation, rgba, 4, 2);
    for (uint32_t y = 0; y < nh; y++) {
        for (uint32_t x = 0; x < nw; x++) {
            uint32_t got = fbd_get(&s, (int)x, (int)y);
            uint32_t exp = (uint32_t)want[y * nw + x] << 11;
            CHECK(got == exp, "rotation %d native (%u,%u): got %#x want logical %d", rotation, x, y, got,
                  want[y * nw + x]);
        }
        CHECK(mem[y * stride_px * 2 + nw * 2] == 0xA5 && mem[y * stride_px * 2 + nw * 2 + 1] == 0xA5,
              "rotation %d: padding of row %u written", rotation, y);
    }
    for (size_t i = nh * stride_px * 2; i < sizeof mem; i++)
        CHECK(mem[i] == 0xA5, "rotation %d: byte %zu past the surface written", rotation, i);

    /* render_map agrees with the hand-written layout. */
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t nx, ny;
        render_map(rotation, 4, 2, i % 4, i / 4, &nx, &ny);
        CHECK(nx < nw && ny < nh && want[ny * nw + nx] == (int)i, "rotation %d: render_map(%u) -> (%u,%u)",
              rotation, i, nx, ny);
    }
}

/* Logical 4x2:      0 1 2 3
 *                   4 5 6 7            */
static void test_rotations(void) {
    static const int r0[] = {0, 1, 2, 3, 4, 5, 6, 7};
    /* 90 clockwise: the logical top row becomes the native right column. */
    static const int r90[] = {4, 0, 5, 1, 6, 2, 7, 3};
    static const int r180[] = {7, 6, 5, 4, 3, 2, 1, 0};
    /* 270 clockwise: the logical top row becomes the native left column, bottom-up. */
    static const int r270[] = {3, 7, 2, 6, 1, 5, 0, 4};
    check_rotation(0, 4, 2, r0);
    check_rotation(90, 2, 4, r90);
    check_rotation(180, 4, 2, r180);
    check_rotation(270, 2, 4, r270);
}

/* Only the pixels of the logical rect (1,0) 2x2 -- indices 1, 2, 5, 6 -- are
 * drawn; every other native pixel keeps its marker. The same hand-written
 * layouts as check_rotation. */
static void check_rect(int rotation, uint32_t nw, uint32_t nh, const int *want) {
    uint8_t mem[16];
    struct fbd_surface s;
    fbd_init(&s, mem, nw, nh, nw * 2, 16, R565, G565, B565, NONE);
    const uint32_t marker = 0x001F; /* blue: no logical index has any blue */
    for (uint32_t i = 0; i < 8; i++) fbd_put(&s, (int)(i % nw), (int)(i / nw), marker);

    uint8_t rgba[32];
    make_indexed(rgba);
    render_rgba_rect(&s, rotation, rgba, 4, 2, 1, 0, 2, 2);
    for (uint32_t y = 0; y < nh; y++)
        for (uint32_t x = 0; x < nw; x++) {
            int idx = want[y * nw + x];
            int inside = idx == 1 || idx == 2 || idx == 5 || idx == 6;
            uint32_t exp = inside ? (uint32_t)idx << 11 : marker;
            CHECK(fbd_get(&s, (int)x, (int)y) == exp, "rect, rotation %d native (%u,%u): got %#x want %#x", rotation,
                  x, y, fbd_get(&s, (int)x, (int)y), exp);
        }

    /* The native bounding box of that rect is exactly the native pixels that changed. */
    uint32_t bx, by, bw, bh;
    render_native_rect(rotation, 4, 2, 1, 0, 2, 2, &bx, &by, &bw, &bh);
    for (uint32_t y = 0; y < nh; y++)
        for (uint32_t x = 0; x < nw; x++) {
            int idx = want[y * nw + x];
            int inside = idx == 1 || idx == 2 || idx == 5 || idx == 6;
            int in_box = x >= bx && x < bx + bw && y >= by && y < by + bh;
            CHECK(inside == in_box, "native box, rotation %d: (%u,%u) inside %d, in box %u,%u %ux%u", rotation, x, y,
                  inside, bx, by, bw, bh);
        }
}

static void test_rect_rotations(void) {
    static const int r0[] = {0, 1, 2, 3, 4, 5, 6, 7};
    static const int r90[] = {4, 0, 5, 1, 6, 2, 7, 3};
    static const int r180[] = {7, 6, 5, 4, 3, 2, 1, 0};
    static const int r270[] = {3, 7, 2, 6, 1, 5, 0, 4};
    check_rect(0, 4, 2, r0);
    check_rect(90, 2, 4, r90);
    check_rect(180, 4, 2, r180);
    check_rect(270, 2, 4, r270);
}

static void test_rotation_valid(void) {
    CHECK(render_rotation_valid(0) && render_rotation_valid(90) && render_rotation_valid(180) &&
              render_rotation_valid(270),
          "the four right angles");
    CHECK(!render_rotation_valid(45) && !render_rotation_valid(-90) && !render_rotation_valid(360), "others");
    uint32_t w, h;
    render_logical_size(800, 1280, 90, &w, &h);
    CHECK(w == 1280 && h == 800, "TT7 native 800x1280 at 90 -> %ux%u", w, h);
    render_logical_size(800, 1280, 0, &w, &h);
    CHECK(w == 800 && h == 1280, "TT7 native at 0 -> %ux%u", w, h);
}

static void test_rgb565_values(void) {
    uint8_t mem[2 * 2];
    struct fbd_surface s;
    fbd_init(&s, mem, 2, 1, 4, 16, R565, G565, B565, NONE);
    /* r5 = 0x12>>3 = 2, g6 = 0x34>>2 = 13, b5 = 0x56>>3 = 10; alpha 0 is ignored. */
    const uint8_t rgba[] = {0x12, 0x34, 0x56, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    render_rgba(&s, 0, rgba, 2, 1);
    CHECK(mem[0] == 0xAA && mem[1] == 0x11, "0x123456 -> bytes %02x %02x, want aa 11", mem[0], mem[1]);
    CHECK(mem[2] == 0xFF && mem[3] == 0xFF, "white -> %02x %02x", mem[2], mem[3]);
    CHECK(strcmp(render_format_name(&s), "rgb565") == 0, "format %s", render_format_name(&s));
}

static void test_xrgb8888_values(void) {
    uint8_t mem[4];
    struct fbd_surface s;
    fbd_init(&s, mem, 1, 1, 4, 32, R8, G8, B8, NONE);
    const uint8_t rgba[] = {0x12, 0x34, 0x56, 0x78};
    render_rgba(&s, 0, rgba, 1, 1);
    CHECK(fbd_get(&s, 0, 0) == 0x00123456, "xrgb %#x", fbd_get(&s, 0, 0));
    CHECK(strcmp(render_format_name(&s), "xrgb8888") == 0, "format %s", render_format_name(&s));
    struct fbd_surface bgr;
    fbd_init(&bgr, mem, 1, 1, 4, 32, B8, G8, R8, NONE);
    CHECK(strcmp(render_format_name(&bgr), "unknown") == 0, "bgr format %s", render_format_name(&bgr));
}

int main(void) {
    test_rotations();
    test_rect_rotations();
    test_rotation_valid();
    test_rgb565_values();
    test_xrgb8888_values();
    return test_finish("test_render");
}
