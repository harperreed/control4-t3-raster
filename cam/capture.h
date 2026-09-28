/* ABOUTME: One-buffer V4L2 capture from the RK3188 CIF camera through an ION-backed OVERLAY buffer.
 * ABOUTME: Every grab refills the 0xAA sentinel, so each frame is checked for a real write. */
#ifndef CAM_CAPTURE_H
#define CAM_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "sentinel.h"

struct cam_config {
    const char *device; /* /dev/video0 */
    int width, height;  /* requested; the driver may adjust */
    FILE *log;          /* step-by-step progress and errors */
};

struct cam {
    int vfd, ionfd, buf_fd; /* -1 when not open */
    int have_handle;
    uint32_t handle; /* ion handle (opaque kernel pointer) */
    uint8_t *va;     /* our mmap of the ion buffer, or NULL */
    size_t len;      /* ion allocation and mapping size */
    uint32_t phys;   /* physical address handed to QBUF as m.offset */
    int streaming;
    int width, height;  /* as the driver accepted them */
    size_t frame_len;   /* width * height * 3 / 2 (NV12) */
    const char *heap;   /* name of the ion heap that took the allocation */
    uint32_t sequence;  /* from the last DQBUF */
    FILE *log;
};

/* Sets every fd to -1 so cam_close is safe on a half-opened cam. */
void cam_init(struct cam *c);

/* S_FMT NV12, REQBUFS(OVERLAY, 1), ion alloc + map + GET_PHYS. Leaves the
 * camera stopped; the first cam_grab starts it. Returns 0, or -1 after
 * logging the failing step (call cam_close either way). */
int cam_open(struct cam *c, const struct cam_config *cfg);

/* Fills the sentinel, QBUF, STREAMON on first use, waits up to timeout_ms
 * for the frame, DQBUF, invalidates the CPU cache, checks the sentinel.
 * Returns 0 if the frame was fully written, 1 if the sentinel shows it was
 * not (see *rep), -1 on an ioctl error or timeout (errno EINTR if a signal
 * interrupted the wait). The frame is at c->va, c->frame_len bytes. */
int cam_grab(struct cam *c, int timeout_ms, struct sentinel_report *rep);

/* STREAMOFF, close the video fd, unmap, close the ion buffer fd, free the
 * ion handle, close /dev/ion: in that order, so the camera has stopped
 * before its target memory goes away. Idempotent. */
void cam_close(struct cam *c);

#endif
