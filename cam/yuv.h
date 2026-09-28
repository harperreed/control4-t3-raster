/* ABOUTME: NV12 (Y plane + interleaved CbCr) to packed RGB888, BT.601 limited range.
 * ABOUTME: Pure functions with no I/O; the camera's native output is NV12. */
#ifndef CAM_YUV_H
#define CAM_YUV_H

#include <stdint.h>

/* One pixel: Y in 16..235 and Cb/Cr in 16..240 map to full-range RGB. Out-of-range input clamps. */
void yuv601_to_rgb(uint8_t y, uint8_t cb, uint8_t cr, uint8_t rgb[3]);

/* nv12 holds w*h luma bytes, then w*h/2 bytes of Cb,Cr pairs (one pair per
 * 2x2 block). w and h must be even. rgb receives w*h*3 bytes. */
void nv12_to_rgb(const uint8_t *nv12, int w, int h, uint8_t *rgb);

#endif
