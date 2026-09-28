/* ABOUTME: Host unit tests for regions.c: the PATCH /frame container (valid, truncated, out of bounds, too many,
 * ABOUTME: size mismatch, overflowing lengths), decode-all-then-apply atomicity, the frame SHA-256, and the golden vector. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lodepng.h"
#include "regions.h"
#include "sha256.h"
#include "test_common.h"

#define LW 1280
#define LH 800

/* A container under construction: records and PNGs are appended in order. */
struct builder {
    uint8_t head[REGIONS_HEAD + REGIONS_MAX * REGIONS_RECORD + 64 * REGIONS_RECORD];
    uint8_t *pngs;
    size_t pngs_len;
    int n;
};

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, v >> 16);
    put16(p + 2, v & 0xFFFF);
}

static void b_init(struct builder *b) {
    memset(b, 0, sizeof *b);
    memcpy(b->head, "TT7R", 4);
    b->head[4] = 1;
}

/* Append a record claiming png_len bytes, and those bytes. */
static void b_add_raw(struct builder *b, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *png,
                      size_t len, uint32_t claimed_len) {
    uint8_t *r = b->head + REGIONS_HEAD + (size_t)b->n * REGIONS_RECORD;
    put16(r, x);
    put16(r + 2, y);
    put16(r + 4, w);
    put16(r + 6, h);
    put32(r + 8, claimed_len);
    b->pngs = realloc(b->pngs, b->pngs_len + len + 1);
    memcpy(b->pngs + b->pngs_len, png, len);
    b->pngs_len += len;
    b->n++;
}

/* A solid w x h PNG of one colour. */
static uint8_t *solid_png(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t bl, size_t *len) {
    uint8_t *rgba = malloc((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rgba[4 * i] = r;
        rgba[4 * i + 1] = g;
        rgba[4 * i + 2] = bl;
        rgba[4 * i + 3] = 255;
    }
    uint8_t *png = NULL;
    unsigned err = lodepng_encode32(&png, len, rgba, w, h);
    free(rgba);
    if (err) {
        fprintf(stderr, "lodepng_encode32: %s\n", lodepng_error_text(err));
        exit(2);
    }
    return png;
}

static void b_add(struct builder *b, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t r, uint8_t g,
                  uint8_t bl) {
    size_t len;
    uint8_t *png = solid_png(w, h, r, g, bl, &len);
    b_add_raw(b, x, y, w, h, png, len, (uint32_t)len);
    free(png);
}

/* The body: header with the count, records, PNGs. Caller frees. */
static uint8_t *b_body(struct builder *b, size_t *len) {
    put16(b->head + 6, (uint32_t)b->n);
    size_t head = REGIONS_HEAD + (size_t)b->n * REGIONS_RECORD;
    *len = head + b->pngs_len;
    uint8_t *out = malloc(*len);
    memcpy(out, b->head, head);
    memcpy(out + head, b->pngs, b->pngs_len);
    free(b->pngs);
    b->pngs = NULL;
    return out;
}

static int parse(const uint8_t *body, size_t len, struct region *r, int *n, struct regions_error *e) {
    memset(e, 0, sizeof *e);
    return regions_parse(body, len, LW, LH, r, n, e);
}

static void test_parse_valid(void) {
    struct builder b;
    b_init(&b);
    b_add(&b, 10, 20, 30, 40, 255, 0, 0);
    b_add(&b, 1270, 790, 10, 10, 0, 255, 0); /* touching the bottom-right corner */
    size_t len;
    uint8_t *body = b_body(&b, &len);
    struct region r[REGIONS_MAX];
    int n = -1;
    struct regions_error e;
    CHECK(parse(body, len, r, &n, &e) == 0, "valid container refused: %s %s", e.code, e.message);
    CHECK(n == 2, "n = %d", n);
    CHECK(r[0].x == 10 && r[0].y == 20 && r[0].w == 30 && r[0].h == 40, "region 0 rect");
    CHECK(r[1].x == 1270 && r[1].y == 790 && r[1].w == 10 && r[1].h == 10, "region 1 rect");
    CHECK(r[0].png == body + REGIONS_HEAD + 2 * REGIONS_RECORD, "region 0 PNG starts after the records");
    CHECK(r[1].png == r[0].png + r[0].png_len && r[1].png + r[1].png_len == body + len, "PNGs back to back");

    /* An empty batch is valid. */
    uint8_t empty[8] = {'T', 'T', '7', 'R', 1, 0, 0, 0};
    CHECK(parse(empty, sizeof empty, r, &n, &e) == 0 && n == 0, "empty batch: %s", e.code ? e.code : "");
    free(body);
}

static void expect_error(const uint8_t *body, size_t len, int status, const char *code, int index, const char *what) {
    struct region r[REGIONS_MAX];
    int n;
    struct regions_error e;
    int rc = parse(body, len, r, &n, &e);
    CHECK(rc != 0 && e.status == status && e.code && strcmp(e.code, code) == 0 && e.index == index,
          "%s: rc %d status %d code %s index %d, want %d %s %d", what, rc, e.status, e.code ? e.code : "(none)",
          e.index, status, code, index);
}

static void test_parse_refusals(void) {
    struct builder b;
    b_init(&b);
    b_add(&b, 0, 0, 8, 8, 1, 2, 3);
    size_t len;
    uint8_t *body = b_body(&b, &len);

    expect_error(body, 0, 400, "invalid_regions", -1, "empty body");
    expect_error(body, 7, 400, "invalid_regions", -1, "short header");
    expect_error(body, REGIONS_HEAD + 5, 400, "invalid_regions", -1, "truncated record");
    expect_error(body, len - 1, 400, "invalid_regions", 0, "truncated PNG data");

    uint8_t *longer = malloc(len + 1);
    memcpy(longer, body, len);
    longer[len] = 0;
    expect_error(longer, len + 1, 400, "invalid_regions", -1, "trailing byte");
    free(longer);

    uint8_t *m = malloc(len);
    memcpy(m, body, len);
    m[0] = 'X';
    expect_error(m, len, 400, "invalid_regions", -1, "bad magic");
    memcpy(m, body, len);
    m[4] = 2;
    expect_error(m, len, 400, "unsupported_regions_version", -1, "version 2");
    memcpy(m, body, len);
    m[5] = 1;
    expect_error(m, len, 400, "invalid_regions", -1, "reserved byte set");
    free(m);
    free(body);
}

static void test_bounds_and_counts(void) {
    struct { uint32_t x, y, w, h; const char *what; } out[] = {
        {1271, 0, 10, 10, "past the right edge"},  {0, 791, 10, 10, "past the bottom edge"},
        {0, 0, 0, 10, "zero width"},               {0, 0, 10, 0, "zero height"},
        {65535, 0, 2, 2, "x beyond the screen"},   {0, 0, 1281, 1, "wider than the screen"},
    };
    for (size_t i = 0; i < sizeof out / sizeof out[0]; i++) {
        struct builder b;
        b_init(&b);
        b_add(&b, 0, 0, 4, 4, 9, 9, 9);
        b_add_raw(&b, out[i].x, out[i].y, out[i].w, out[i].h, (const uint8_t *)"x", 1, 1);
        size_t len;
        uint8_t *body = b_body(&b, &len);
        expect_error(body, len, 422, "region_out_of_bounds", 1, out[i].what);
        free(body);
    }

    /* 17 regions: one too many. */
    struct builder b;
    b_init(&b);
    for (int i = 0; i < REGIONS_MAX + 1; i++) b_add_raw(&b, 0, 0, 1, 1, (const uint8_t *)"x", 1, 1);
    size_t len;
    uint8_t *body = b_body(&b, &len);
    expect_error(body, len, 400, "too_many_regions", -1, "17 regions");
    free(body);

    /* Regions whose total area exceeds the screen's: decoding them could take more than a frame of RAM. */
    b_init(&b);
    b_add_raw(&b, 0, 0, LW, LH, (const uint8_t *)"x", 1, 1);
    b_add_raw(&b, 0, 0, 1, 1, (const uint8_t *)"x", 1, 1);
    body = b_body(&b, &len);
    expect_error(body, len, 422, "regions_too_large", -1, "more pixels than the screen");
    free(body);
}

static void test_overflowing_lengths(void) {
    /* Lengths that wrap a 32-bit sum must be refused, not wrapped. */
    struct builder b;
    b_init(&b);
    b_add_raw(&b, 0, 0, 1, 1, (const uint8_t *)"ab", 2, 0xFFFFFFFFu);
    b_add_raw(&b, 0, 0, 1, 1, (const uint8_t *)"", 0, 3);
    size_t len;
    uint8_t *body = b_body(&b, &len);
    expect_error(body, len, 400, "invalid_regions", 0, "0xFFFFFFFF + 3");
    free(body);
}

static uint8_t *base_image(void) {
    uint8_t *rgba = malloc((size_t)LW * LH * 4);
    for (uint32_t y = 0; y < LH; y++)
        for (uint32_t x = 0; x < LW; x++) {
            uint8_t *p = rgba + ((size_t)y * LW + x) * 4;
            p[0] = (uint8_t)x;
            p[1] = (uint8_t)y;
            p[2] = (uint8_t)(x + y);
            p[3] = 255;
        }
    return rgba;
}

static int apply(const uint8_t *body, size_t len, uint8_t *rgba, struct regions_error *e) {
    struct region r[REGIONS_MAX];
    int n;
    memset(e, 0, sizeof *e);
    if (regions_parse(body, len, LW, LH, r, &n, e) != 0) return -1;
    return regions_apply(r, n, rgba, LW, LH, e);
}

static void test_apply_and_order(void) {
    struct builder b;
    b_init(&b);
    b_add(&b, 100, 50, 20, 10, 255, 0, 0);
    b_add(&b, 110, 55, 20, 10, 0, 0, 255); /* overlaps the first: later wins */
    size_t len;
    uint8_t *body = b_body(&b, &len);
    uint8_t *rgba = base_image(), *want = base_image();
    for (uint32_t y = 50; y < 60; y++)
        for (uint32_t x = 100; x < 120; x++) memcpy(want + ((size_t)y * LW + x) * 4, "\xff\x00\x00\xff", 4);
    for (uint32_t y = 55; y < 65; y++)
        for (uint32_t x = 110; x < 130; x++) memcpy(want + ((size_t)y * LW + x) * 4, "\x00\x00\xff\xff", 4);
    struct regions_error e;
    CHECK(apply(body, len, rgba, &e) == 0, "apply: %s %s", e.code, e.message);
    CHECK(memcmp(rgba, want, (size_t)LW * LH * 4) == 0, "composed pixels differ from the expected image");
    free(rgba);
    free(want);
    free(body);
}

static void test_atomic(void) {
    /* Region 0 is fine, region 1's PNG is 8x8 where the record says 8x9, region 2 is not a PNG. Either
     * failure must leave every pixel alone, including region 0's. */
    for (int bad = 0; bad < 2; bad++) {
        struct builder b;
        b_init(&b);
        b_add(&b, 0, 0, 8, 8, 255, 255, 255);
        if (bad == 0) {
            size_t plen;
            uint8_t *png = solid_png(8, 8, 1, 1, 1, &plen);
            b_add_raw(&b, 20, 20, 8, 9, png, plen, (uint32_t)plen);
            free(png);
        } else {
            b_add_raw(&b, 20, 20, 2, 2, (const uint8_t *)"not a png", 9, 9);
        }
        size_t len;
        uint8_t *body = b_body(&b, &len);
        uint8_t *rgba = base_image(), *want = base_image();
        struct regions_error e;
        int rc = apply(body, len, rgba, &e);
        const char *code = bad == 0 ? "region_size_mismatch" : "invalid_image";
        CHECK(rc != 0 && e.status == 422 && !strcmp(e.code, code) && e.index == 1, "bad %d: rc %d %d %s index %d", bad,
              rc, e.status, e.code ? e.code : "(none)", e.index);
        if (bad == 0)
            CHECK(e.expected_w == 8 && e.expected_h == 9 && e.received_w == 8 && e.received_h == 8,
                  "mismatch sizes %ux%u / %ux%u", e.expected_w, e.expected_h, e.received_w, e.received_h);
        CHECK(memcmp(rgba, want, (size_t)LW * LH * 4) == 0, "bad %d: pixels changed after a refused batch", bad);
        free(rgba);
        free(want);
        free(body);
    }
}

static void test_frame_sha(void) {
    /* sha256( lowercase hex of the base frame's sha256 || the request body ). */
    const char *base = "00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF";
    const uint8_t body[] = {'T', 'T', '7', 'R', 1, 0, 0, 0};
    char got[65], want[65];
    CHECK(regions_frame_sha(base, body, sizeof body, got) == 0, "regions_frame_sha failed");
    char buf[64 + sizeof body];
    for (int i = 0; i < 64; i++) buf[i] = (char)(base[i] >= 'A' && base[i] <= 'F' ? base[i] + 32 : base[i]);
    memcpy(buf + 64, body, sizeof body);
    sha256_hex((const uint8_t *)buf, sizeof buf, want);
    CHECK(strcmp(got, want) == 0, "frame sha %s, want %s", got, want);
    /* Known answer from sha256sum over the lowercase hex followed by the 8 body bytes. */
    CHECK(strcmp(got, "017c22fc77a67a73a7f1eecbd07b60c0e76e9cf8d951bd7ef0d92cba90c6a0c5") == 0, "known answer: %s", got);
}

/* ---- the golden vector shared with server/internal/regions (Go) ---------- */

#define GOLDEN_DIR "tt7d/test/fixtures/"

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    *len = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return buf;
}

/* The value of `key=` in the golden's text file. */
static int golden_value(const char *key, char *out, size_t n) {
    FILE *f = fopen(GOLDEN_DIR "regions-v1.txt", "r");
    if (!f) return -1;
    char line[256];
    int found = -1;
    size_t k = strlen(key);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, k) && line[k] == '=') {
            snprintf(out, n, "%s", line + k + 1);
            out[strcspn(out, "\r\n")] = 0;
            found = 0;
        }
    fclose(f);
    return found;
}

static void test_golden(void) {
    size_t len;
    uint8_t *body = read_all(GOLDEN_DIR "regions-v1.bin", &len);
    char base_sha[80], result_sha[80], frame_sha[80], count[16];
    if (!body || golden_value("base_frame_sha256", base_sha, sizeof base_sha) ||
        golden_value("result_rgba_sha256", result_sha, sizeof result_sha) ||
        golden_value("frame_sha256", frame_sha, sizeof frame_sha) || golden_value("regions", count, sizeof count)) {
        CHECK(0, "cannot read the golden vector in %s", GOLDEN_DIR);
        free(body);
        return;
    }
    struct region r[REGIONS_MAX];
    int n;
    struct regions_error e;
    uint8_t *rgba = base_image();
    CHECK(parse(body, len, r, &n, &e) == 0, "golden refused: %s %s", e.code, e.message);
    CHECK(n == atoi(count), "golden has %d regions, the text says %s", n, count);
    CHECK(regions_apply(r, n, rgba, LW, LH, &e) == 0, "golden apply: %s", e.code);
    char got[65];
    sha256_hex(rgba, (size_t)LW * LH * 4, got);
    CHECK(strcmp(got, result_sha) == 0, "golden result pixels hash %s, want %s", got, result_sha);
    CHECK(regions_frame_sha(base_sha, body, len, got) == 0, "regions_frame_sha failed");
    CHECK(strcmp(got, frame_sha) == 0, "golden frame sha %s, want %s", got, frame_sha);
    free(rgba);
    free(body);
}

int main(void) {
    test_parse_valid();
    test_parse_refusals();
    test_bounds_and_counts();
    test_overflowing_lengths();
    test_apply_and_order();
    test_atomic();
    test_frame_sha();
    test_golden();
    return test_finish("test_regions");
}
