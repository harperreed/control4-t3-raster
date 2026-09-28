/* ABOUTME: Host test for yuv.c: BT.601 limited-range YCbCr -> RGB values and the NV12 plane layout.
 * ABOUTME: Expected values come from a float reference built from Kr/Kb, not from the fixed-point code. */
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"
#include "yuv.h"

/* BT.601: Kr = 0.299, Kb = 0.114. Limited range: Y in 16..235 (219 steps),
 * Cb/Cr in 16..240 around 128 (224 steps). */
static void reference(int y, int u, int v, int out[3]) {
    const double kr = 0.299, kb = 0.114, kg = 1.0 - kr - kb;
    double Y = (y - 16) * 255.0 / 219.0, U = (u - 128) * 255.0 / 224.0, V = (v - 128) * 255.0 / 224.0;
    double rgb[3] = {
        Y + 2.0 * (1.0 - kr) * V,
        Y - 2.0 * kb * (1.0 - kb) / kg * U - 2.0 * kr * (1.0 - kr) / kg * V,
        Y + 2.0 * (1.0 - kb) * U,
    };
    for (int i = 0; i < 3; i++) {
        long r = lround(rgb[i]);
        out[i] = r < 0 ? 0 : r > 255 ? 255 : (int)r;
    }
}

static void check_triple(const char *name, uint8_t y, uint8_t u, uint8_t v, int r, int g, int b) {
    uint8_t got[3];
    yuv601_to_rgb(y, u, v, got);
    CHECK(got[0] == r && got[1] == g && got[2] == b, "%s (%u,%u,%u) -> (%u,%u,%u), want (%d,%d,%d)", name, y, u,
          v, got[0], got[1], got[2], r, g, b);
}

int main(void) {
    /* Worked by hand from the formula in reference() and rounded:
     * grey: (128-16)*255/219 = 130.41 -> 130.
     * red (81,90,240): R = 65*1.16438 + 112*1.59603 = 254.44 -> 254; G = -0.48 -> 0; B = -0.97 -> 0.
     * green (145,54,34): R = 0.18 -> 0; G = 255.61 -> 255 (clamped); B = 0.93 -> 1.
     * blue (41,240,110): R = 0.38 -> 0; G = -0.13 -> 0; B = 255.04 -> 255. */
    check_triple("black", 16, 128, 128, 0, 0, 0);
    check_triple("white", 235, 128, 128, 255, 255, 255);
    check_triple("mid-grey", 128, 128, 128, 130, 130, 130);
    check_triple("red", 81, 90, 240, 254, 0, 0);
    check_triple("green", 145, 54, 34, 0, 255, 1);
    check_triple("blue", 41, 240, 110, 0, 0, 255);
    check_triple("below black clamps", 0, 128, 128, 0, 0, 0);
    check_triple("above white clamps", 255, 128, 128, 255, 255, 255);

    /* Every one of the 2^24 inputs lands within 1 of the float reference. */
    int worst = 0, wy = 0, wu = 0, wv = 0;
    for (int y = 0; y < 256; y++)
        for (int u = 0; u < 256; u++)
            for (int v = 0; v < 256; v++) {
                uint8_t got[3];
                int want[3];
                yuv601_to_rgb((uint8_t)y, (uint8_t)u, (uint8_t)v, got);
                reference(y, u, v, want);
                for (int i = 0; i < 3; i++) {
                    int d = abs(got[i] - want[i]);
                    if (d > worst) worst = d, wy = y, wu = u, wv = v;
                }
            }
    CHECK(worst <= 1, "max error %d at (%d,%d,%d)", worst, wy, wu, wv);

    /* NV12 layout: Y plane w*h, then one interleaved Cb,Cr pair per 2x2 block.
     * 4x2 image: left block red chroma, right block blue chroma. */
    uint8_t nv12[4 * 2 * 3 / 2] = {
        81, 81, 41, 41, /* Y row 0 */
        81, 81, 41, 41, /* Y row 1 */
        90, 240,        /* Cb,Cr of the left 2x2 block */
        240, 110,       /* Cb,Cr of the right 2x2 block */
    };
    uint8_t rgb[4 * 2 * 3];
    memset(rgb, 0x55, sizeof rgb);
    nv12_to_rgb(nv12, 4, 2, rgb);
    for (int row = 0; row < 2; row++)
        for (int col = 0; col < 4; col++) {
            const uint8_t *p = rgb + (row * 4 + col) * 3;
            int want_r = col < 2 ? 254 : 0, want_b = col < 2 ? 0 : 255;
            CHECK(p[0] == want_r && p[1] == 0 && p[2] == want_b, "pixel (%d,%d) = (%u,%u,%u)", col, row, p[0], p[1],
                  p[2]);
        }

    return test_finish("cam yuv: BT.601 limited-range NV12 -> RGB");
}
