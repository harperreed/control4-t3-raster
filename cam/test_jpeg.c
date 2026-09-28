/* ABOUTME: Host test for the NV12 -> JPEG pipeline: synthetic frame in, well-formed baseline JPEG out.
 * ABOUTME: Checks SOI/EOI markers and the SOF0 header's size and component count; test_convert.py decodes it. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg.h"
#include "test_common.h"

/* Finds the first SOF0 (FF C0) segment by walking marker segments from SOI.
 * SOF0 payload: length(2) precision(1) height(2) width(2) components(1). */
static int parse_sof0(const uint8_t *d, size_t n, int *w, int *h, int *comps) {
    size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) return -1;
        uint8_t marker = d[i + 1];
        size_t seglen = (size_t)d[i + 2] << 8 | d[i + 3];
        if (marker == 0xC0) {
            if (i + 10 > n) return -1;
            *h = d[i + 5] << 8 | d[i + 6];
            *w = d[i + 7] << 8 | d[i + 8];
            *comps = d[i + 9];
            return 0;
        }
        if (marker == 0xDA) return -1; /* start of scan before any SOF0 */
        i += 2 + seglen;
    }
    return -1;
}

/* 4 vertical bands: red, green, blue, mid-grey (BT.601 limited-range YCbCr). */
static uint8_t *synthetic_nv12(int w, int h) {
    static const uint8_t bands[4][3] = {{81, 90, 240}, {145, 54, 34}, {41, 240, 110}, {128, 128, 128}};
    uint8_t *f = malloc((size_t)w * h * 3 / 2);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) f[y * w + x] = bands[x * 4 / w][0];
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w; x += 2) {
            const uint8_t *b = bands[x * 4 / w];
            f[w * h + y * w + x] = b[1];
            f[w * h + y * w + x + 1] = b[2];
        }
    return f;
}

int main(void) {
    const int w = 128, h = 64;
    uint8_t *nv12 = synthetic_nv12(w, h);
    struct jpeg_buf j;
    CHECK(jpeg_encode_nv12(nv12, w, h, 80, &j) == 0, "encode failed");
    CHECK(j.len > 4, "jpeg is %zu bytes", j.len);
    if (j.len > 4) {
        CHECK(j.data[0] == 0xFF && j.data[1] == 0xD8, "SOI %02x %02x", j.data[0], j.data[1]);
        CHECK(j.data[j.len - 2] == 0xFF && j.data[j.len - 1] == 0xD9, "EOI %02x %02x", j.data[j.len - 2],
              j.data[j.len - 1]);
        int jw = 0, jh = 0, comps = 0;
        CHECK(parse_sof0(j.data, j.len, &jw, &jh, &comps) == 0, "no SOF0 segment");
        CHECK(jw == w && jh == h && comps == 3, "SOF0 says %dx%d, %d components", jw, jh, comps);
    }
    jpeg_buf_free(&j);

    /* 1280x720 is the sensor's native mode. */
    uint8_t *big = synthetic_nv12(1280, 720);
    CHECK(jpeg_encode_nv12(big, 1280, 720, 80, &j) == 0, "1280x720 encode failed");
    int jw = 0, jh = 0, comps = 0;
    CHECK(parse_sof0(j.data, j.len, &jw, &jh, &comps) == 0 && jw == 1280 && jh == 720, "SOF0 %dx%d", jw, jh);
    jpeg_buf_free(&j);

    CHECK(jpeg_encode_nv12(nv12, 127, 64, 80, &j) == -1, "odd width must be refused");
    CHECK(jpeg_encode_nv12(nv12, w, h, 0, &j) == -1, "quality 0 must be refused");
    CHECK(jpeg_encode_nv12(nv12, w, h, 101, &j) == -1, "quality 101 must be refused");

    free(nv12);
    free(big);
    return test_finish("cam jpeg: NV12 -> baseline JPEG markers and SOF0 header");
}
