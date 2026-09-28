/* ABOUTME: BT.601 limited-range YCbCr -> RGB in 16.16 fixed point, and the NV12 frame walk.
 * ABOUTME: Coefficients are round(c * 65536) of the exact BT.601 values; test_yuv.c checks all inputs. */
#include "yuv.h"

/* Kr = 0.299, Kb = 0.114, Y scale 255/219, chroma scale 255/224:
 *   Y  1.164384 -> 76309
 *   Cr->R 1.596027 -> 104597    Cb->G 0.391762 -> 25675
 *   Cr->G 0.812968 -> 53279     Cb->B 2.017232 -> 132201 */
#define FIX_Y 76309
#define FIX_RV 104597
#define FIX_GU 25675
#define FIX_GV 53279
#define FIX_BU 132201
#define FIX_HALF (1 << 15)

static uint8_t clamp_fix(int32_t v) {
    v = (v + FIX_HALF) >> 16;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void yuv601_to_rgb(uint8_t y, uint8_t cb, uint8_t cr, uint8_t rgb[3]) {
    int32_t yy = FIX_Y * ((int32_t)y - 16);
    int32_t u = (int32_t)cb - 128, v = (int32_t)cr - 128;
    rgb[0] = clamp_fix(yy + FIX_RV * v);
    rgb[1] = clamp_fix(yy - FIX_GU * u - FIX_GV * v);
    rgb[2] = clamp_fix(yy + FIX_BU * u);
}

void nv12_to_rgb(const uint8_t *nv12, int w, int h, uint8_t *rgb) {
    const uint8_t *uv_plane = nv12 + (long)w * h;
    for (int row = 0; row < h; row++) {
        const uint8_t *y = nv12 + (long)row * w;
        const uint8_t *uv = uv_plane + (long)(row / 2) * w;
        uint8_t *out = rgb + (long)row * w * 3;
        for (int col = 0; col < w; col++)
            yuv601_to_rgb(y[col], uv[col & ~1], uv[col | 1], out + col * 3);
    }
}
