/* ABOUTME: Host unit tests for fbdraw.c: pixel packing and test-pattern pixels in memory buffers.
 * ABOUTME: Build and run with `make test-host`; exits non-zero on the first failed check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fbdraw.h"

static int failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

#define GUARD 256
#define CANARY 0xA5

/* A framebuffer in memory with canary bytes before, after, and in the
 * per-line padding, so tests catch any write outside the visible window. */
struct testfb {
    uint8_t *alloc;
    struct fbd_surface s;
    size_t size;
};

static const struct fbd_chan NONE = {0, 0};

static void testfb_make(struct testfb *t, uint32_t w, uint32_t h, uint32_t stride, uint32_t bpp,
                        struct fbd_chan r, struct fbd_chan g, struct fbd_chan b, struct fbd_chan a) {
    t->size = (size_t)stride * h;
    t->alloc = malloc(t->size + 2 * GUARD);
    if (!t->alloc) { perror("malloc"); exit(2); }
    memset(t->alloc, CANARY, t->size + 2 * GUARD);
    int rc = fbd_init(&t->s, t->alloc + GUARD, w, h, stride, bpp, r, g, b, a);
    CHECK(rc == 0, "fbd_init rc=%d", rc);
}

static void testfb_check_canaries(struct testfb *t, const char *what) {
    for (size_t i = 0; i < GUARD; i++) {
        CHECK(t->alloc[i] == CANARY, "%s: write before buffer at -%zu", what, GUARD - i);
        CHECK(t->alloc[GUARD + t->size + i] == CANARY, "%s: write after buffer at +%zu", what, i);
        if (t->alloc[i] != CANARY || t->alloc[GUARD + t->size + i] != CANARY) return;
    }
    uint32_t row_bytes = t->s.width * (t->s.bpp / 8);
    for (uint32_t y = 0; y < t->s.height; y++)
        for (uint32_t x = row_bytes; x < t->s.stride; x++)
            if (t->s.mem[(size_t)y * t->s.stride + x] != CANARY) {
                CHECK(0, "%s: write into line padding at y=%u byte=%u", what, y, x);
                return;
            }
}

static void testfb_free(struct testfb *t) { free(t->alloc); }

static const struct fbd_chan R565 = {11, 5}, G565 = {5, 6}, B565 = {0, 5};
static const struct fbd_chan R8 = {16, 8}, G8 = {8, 8}, B8 = {0, 8}, A8 = {24, 8};

static void test_pack_rgb565(void) {
    struct testfb t;
    testfb_make(&t, 4, 4, 8, 16, R565, G565, B565, NONE);
    CHECK(fbd_pack(&t.s, 255, 0, 0) == 0xF800, "red=%#x", fbd_pack(&t.s, 255, 0, 0));
    CHECK(fbd_pack(&t.s, 0, 255, 0) == 0x07E0, "green=%#x", fbd_pack(&t.s, 0, 255, 0));
    CHECK(fbd_pack(&t.s, 0, 0, 255) == 0x001F, "blue=%#x", fbd_pack(&t.s, 0, 0, 255));
    CHECK(fbd_pack(&t.s, 255, 255, 255) == 0xFFFF, "white=%#x", fbd_pack(&t.s, 255, 255, 255));
    CHECK(fbd_pack(&t.s, 0, 0, 0) == 0x0000, "black");
    /* 0x80 keeps its top bits: 5-bit 0x10, 6-bit 0x20. */
    CHECK(fbd_pack(&t.s, 0x80, 0x80, 0x80) == ((0x10 << 11) | (0x20 << 5) | 0x10), "grey=%#x",
          fbd_pack(&t.s, 0x80, 0x80, 0x80));
    /* Little-endian byte order in memory. */
    fbd_put(&t.s, 1, 2, 0xF800);
    CHECK(t.s.mem[2 * 8 + 2] == 0x00 && t.s.mem[2 * 8 + 3] == 0xF8, "rgb565 bytes %02x %02x",
          t.s.mem[2 * 8 + 2], t.s.mem[2 * 8 + 3]);
    CHECK(fbd_get(&t.s, 1, 2) == 0xF800, "readback");
    testfb_check_canaries(&t, "rgb565 put");
    testfb_free(&t);
}

static void test_pack_32bpp(void) {
    struct testfb t;
    testfb_make(&t, 4, 4, 16, 32, R8, G8, B8, NONE);
    CHECK(fbd_pack(&t.s, 255, 0, 0) == 0x00FF0000, "xrgb red=%#x", fbd_pack(&t.s, 255, 0, 0));
    CHECK(fbd_pack(&t.s, 0, 0, 255) == 0x000000FF, "xrgb blue");
    testfb_free(&t);

    testfb_make(&t, 4, 4, 16, 32, R8, G8, B8, A8);
    CHECK(fbd_pack(&t.s, 255, 0, 0) == 0xFFFF0000, "argb red opaque=%#x", fbd_pack(&t.s, 255, 0, 0));
    CHECK(fbd_pack(&t.s, 0, 0, 0) == 0xFF000000, "argb black opaque");
    fbd_put(&t.s, 3, 3, 0xFFFF0000);
    uint8_t *p = t.s.mem + 3 * 16 + 3 * 4;
    CHECK(p[0] == 0x00 && p[1] == 0x00 && p[2] == 0xFF && p[3] == 0xFF, "argb bytes %02x%02x%02x%02x",
          p[0], p[1], p[2], p[3]);
    testfb_check_canaries(&t, "argb put");
    testfb_free(&t);

    /* ABGR (Android's usual RGBA_8888 layout): red in the low byte. */
    struct fbd_chan rl = {0, 8}, bl = {16, 8};
    testfb_make(&t, 4, 4, 16, 32, rl, G8, bl, A8);
    CHECK(fbd_pack(&t.s, 255, 0, 0) == 0xFF0000FF, "abgr red=%#x", fbd_pack(&t.s, 255, 0, 0));
    testfb_free(&t);
}

static void test_pack_24bpp(void) {
    struct testfb t;
    testfb_make(&t, 3, 2, 12, 24, R8, G8, B8, NONE);
    fbd_put(&t.s, 2, 1, fbd_pack(&t.s, 0x11, 0x22, 0x33));
    uint8_t *p = t.s.mem + 12 + 6;
    CHECK(p[0] == 0x33 && p[1] == 0x22 && p[2] == 0x11, "rgb888 bytes %02x %02x %02x", p[0], p[1], p[2]);
    CHECK(fbd_get(&t.s, 2, 1) == 0x112233, "rgb888 readback %#x", fbd_get(&t.s, 2, 1));
    testfb_check_canaries(&t, "rgb888 put");
    testfb_free(&t);
}

static void test_fallback_formats(void) {
    struct testfb t;
    testfb_make(&t, 2, 2, 4, 16, NONE, NONE, NONE, NONE);
    CHECK(fbd_pack(&t.s, 255, 0, 0) == 0xF800, "16bpp fallback is rgb565");
    testfb_free(&t);
    testfb_make(&t, 2, 2, 8, 32, NONE, NONE, NONE, NONE);
    CHECK(fbd_pack(&t.s, 255, 0, 0) == 0x00FF0000, "32bpp fallback is xrgb8888");
    testfb_free(&t);

    struct fbd_surface s;
    uint8_t buf[16];
    CHECK(fbd_init(&s, buf, 2, 2, 8, 8, R8, G8, B8, NONE) == -1, "8bpp rejected");
}

static void test_clipping(void) {
    struct testfb t;
    testfb_make(&t, 10, 10, 24, 16, R565, G565, B565, NONE);
    fbd_put(&t.s, -1, 0, 0xFFFF);
    fbd_put(&t.s, 0, -1, 0xFFFF);
    fbd_put(&t.s, 10, 0, 0xFFFF);
    fbd_put(&t.s, 0, 10, 0xFFFF);
    fbd_fill(&t.s, -5, -5, 30, 30, 0x1234);
    fbd_dot(&t.s, 0, 0, 0xFFFF);
    fbd_dot(&t.s, 9, 9, 0xFFFF);
    fbd_text(&t.s, 5, 5, 3, "clip me", 0xFFFF);
    testfb_check_canaries(&t, "clipping");
    CHECK(fbd_get(&t.s, 5, 5) != 0xA5A5, "fill covered the window");
    CHECK(fbd_get(&t.s, -1, 0) == 0 && fbd_get(&t.s, 10, 0) == 0, "out-of-range get is 0");
    testfb_free(&t);
}

static void test_text(void) {
    struct testfb t;
    testfb_make(&t, 32, 16, 64, 16, R565, G565, B565, NONE);
    fbd_fill(&t.s, 0, 0, 32, 16, 0);
    /* 'H' in font8x8_basic: rows 0-2 and 4-6 set columns 0,1 and 4,5 (bit n = column n);
     * row 3 sets columns 0-5; row 7 is empty. */
    fbd_text(&t.s, 0, 0, 1, "H", 0xFFFF);
    CHECK(fbd_get(&t.s, 0, 0) == 0xFFFF, "H top-left stem");
    CHECK(fbd_get(&t.s, 2, 0) == 0, "H gap between stems");
    CHECK(fbd_get(&t.s, 5, 0) == 0xFFFF, "H right stem");
    CHECK(fbd_get(&t.s, 3, 3) == 0xFFFF, "H crossbar");
    CHECK(fbd_get(&t.s, 0, 7) == 0, "H bottom row empty");
    /* Scale 2 doubles every font pixel. */
    fbd_fill(&t.s, 0, 0, 32, 16, 0);
    fbd_text(&t.s, 0, 0, 2, "H", 0xFFFF);
    CHECK(fbd_get(&t.s, 1, 1) == 0xFFFF && fbd_get(&t.s, 4, 0) == 0, "H at scale 2");
    CHECK(fbd_text_width(2, "abc") == 48, "width %d", fbd_text_width(2, "abc"));
    testfb_check_canaries(&t, "text");
    testfb_free(&t);
}

static void test_scale(void) {
    CHECK(fbd_scale(0, 0, 1023, 800) == 0, "min -> 0");
    CHECK(fbd_scale(1023, 0, 1023, 800) == 799, "max -> size-1");
    CHECK(fbd_scale(-50, 0, 1023, 800) == 0, "below min clamps");
    CHECK(fbd_scale(5000, 0, 1023, 800) == 799, "above max clamps");
    CHECK(fbd_scale(400, 0, 800, 1281) == 640, "mid %d", fbd_scale(400, 0, 800, 1281));
    CHECK(fbd_scale(10, 100, 100, 800) == -1, "empty range");
    CHECK(fbd_scale(0, -2048, 2047, 4096) == 2048, "signed range %d", fbd_scale(0, -2048, 2047, 4096));
}

/* Check the pattern's landmarks: border, gaps, corner blocks with their sizes,
 * bars and ramps. */
static void check_pattern(struct testfb *t, const char *name) {
    const struct fbd_surface *s = &t->s;
    int w = (int)s->width, h = (int)s->height, u = (int)fbd_unit(s);
    uint32_t white = fbd_pack(s, 255, 255, 255), black = fbd_pack(s, 0, 0, 0);
    uint32_t red = fbd_pack(s, 255, 0, 0), green = fbd_pack(s, 0, 255, 0), blue = fbd_pack(s, 0, 0, 255);

    CHECK(u == (w < h ? w : h) / 20, "%s: unit %d", name, u);

    /* 1-pixel white border on all four edges. */
    for (int x = 0; x < w; x++) {
        if (fbd_get(s, x, 0) != white || fbd_get(s, x, h - 1) != white) {
            CHECK(0, "%s: border row missing at x=%d", name, x);
            break;
        }
    }
    for (int y = 0; y < h; y++) {
        if (fbd_get(s, 0, y) != white || fbd_get(s, w - 1, y) != white) {
            CHECK(0, "%s: border column missing at y=%d", name, y);
            break;
        }
    }
    /* A black 1-pixel gap separates the border from each block. */
    CHECK(fbd_get(s, 1, 1) == black, "%s: TL gap", name);
    CHECK(fbd_get(s, w - 2, h - 2) == black, "%s: BR gap", name);

    /* Corner blocks: colour at both inner corners, black just past the edge. */
    int i = FBD_INSET;
    int rs = FBD_RED_UNITS * u, gs = FBD_GREEN_UNITS * u, bs = FBD_BLUE_UNITS * u, ws = FBD_WHITE_UNITS * u;
    CHECK(fbd_get(s, i, i) == red, "%s: red at TL", name);
    CHECK(fbd_get(s, i + rs - 1, i + rs - 1) == red, "%s: red spans %d", name, rs);
    CHECK(fbd_get(s, i + rs, i) == black, "%s: red ends at x=%d", name, i + rs);
    CHECK(fbd_get(s, i, i + rs) == black, "%s: red ends at y=%d", name, i + rs);

    CHECK(fbd_get(s, w - 1 - i, i) == green, "%s: green at TR", name);
    CHECK(fbd_get(s, w - i - gs, i + gs - 1) == green, "%s: green spans %d", name, gs);
    CHECK(fbd_get(s, w - i - gs - 1, i) == black, "%s: green ends", name);

    CHECK(fbd_get(s, i, h - 1 - i) == blue, "%s: blue at BL", name);
    CHECK(fbd_get(s, i + bs - 1, h - i - bs) == blue, "%s: blue spans %d", name, bs);
    CHECK(fbd_get(s, i + bs, h - 1 - i) == black, "%s: blue ends", name);

    CHECK(fbd_get(s, w - 1 - i, h - 1 - i) == white, "%s: white at BR", name);
    CHECK(fbd_get(s, w - i - ws, h - i - ws) == white, "%s: white spans %d", name, ws);
    CHECK(fbd_get(s, w - i - ws - 1, h - 1 - i) == black, "%s: white ends", name);

    /* Colour bars: sample the centre of each bar. */
    int by, bh;
    fbd_bar_band(s, &by, &bh);
    CHECK(bh > 0 && by > i + rs && by + bh < h - i - bs, "%s: bar band %d+%d clear of corners", name, by, bh);
    for (int b = 0; b < FBD_NBARS; b++) {
        int x = (2 * b + 1) * w / (2 * FBD_NBARS);
        uint32_t want = fbd_pack(s, fbd_bars[b][0], fbd_bars[b][1], fbd_bars[b][2]);
        CHECK(fbd_get(s, x, by + bh / 2) == want, "%s: bar %d at x=%d is %#x want %#x", name, b, x,
              fbd_get(s, x, by + bh / 2), want);
    }

    /* Ramps: four strips (R, G, B, grey), black at the left, full at the right. */
    int ry, rh;
    fbd_ramp_band(s, &ry, &rh);
    CHECK(rh >= 4 && ry >= by + bh && ry + rh < h - i - bs, "%s: ramp band %d+%d", name, ry, rh);
    int strip = rh / 4;
    const uint8_t full[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    for (int k = 0; k < 4; k++) {
        int y = ry + k * strip + strip / 2;
        CHECK(fbd_get(s, 1, y) == black, "%s: ramp %d starts black", name, k);
        CHECK(fbd_get(s, w - 2, y) == fbd_pack(s, full[k][0], full[k][1], full[k][2]),
              "%s: ramp %d ends full (%#x)", name, k, fbd_get(s, w - 2, y));
    }

    testfb_check_canaries(t, name);
}

static void test_pattern_portrait_rgb565(void) {
    struct testfb t;
    /* 800x1280 RGB565 with a padded stride to prove padding is never written. */
    testfb_make(&t, 800, 1280, 1664, 16, R565, G565, B565, NONE);
    const char *info[] = {"fb0 800x1280 16bpp", "stride 1664", NULL};
    fbd_test_pattern(&t.s, info);
    check_pattern(&t, "800x1280 rgb565");
    testfb_free(&t);
}

static void test_pattern_landscape_xrgb8888(void) {
    struct testfb t;
    testfb_make(&t, 1280, 800, 1280 * 4, 32, R8, G8, B8, NONE);
    fbd_test_pattern(&t.s, NULL);
    check_pattern(&t, "1280x800 xrgb8888");
    testfb_free(&t);
}

static void test_pattern_tiny(void) {
    /* Too small for text and blocks to fit; must still not scribble outside. */
    struct testfb t;
    testfb_make(&t, 7, 5, 16, 16, R565, G565, B565, NONE);
    const char *info[] = {"a long line that cannot fit", NULL};
    fbd_test_pattern(&t.s, info);
    testfb_check_canaries(&t, "tiny");
    testfb_free(&t);
}

int main(void) {
    test_pack_rgb565();
    test_pack_32bpp();
    test_pack_24bpp();
    test_fallback_formats();
    test_clipping();
    test_text();
    test_scale();
    test_pattern_portrait_rgb565();
    test_pattern_landscape_xrgb8888();
    test_pattern_tiny();
    if (failures) {
        fprintf(stderr, "test_fbdraw: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("test_fbdraw: all checks passed\n");
    return 0;
}
