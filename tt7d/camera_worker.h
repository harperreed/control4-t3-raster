/* ABOUTME: The camera worker: a child process of tt7d that alone opens the camera, captures NV12 frames,
 * ABOUTME: encodes JPEGs and scores presence, and talks to tt7d over a socketpair (camera_proto.h). */
#ifndef TT7D_CAMERA_WORKER_H
#define TT7D_CAMERA_WORKER_H

#define CAMERA_WIDTH 1280 /* the NT99141's native mode; the RK CIF driver has BUG() paths for other sizes */
#define CAMERA_HEIGHT 720
#define CAMERA_JPEG_QUALITY 80
#define CAMERA_GRAB_TIMEOUT_MS 2000 /* per frame, as tt7cam */

struct camera_worker_config {
    const char *device; /* /dev/video0 */
    /* TEST ONLY: read 1280x720 NV12 frames from this file or FIFO instead of
     * the camera (tt7d --camera-fake-source). NULL on the panel. A read that
     * blocks stays blocked, even through SIGTERM, the way a stuck driver
     * call would, so the tests exercise tt7d's SIGKILL path. */
    const char *fake_source;
    int presence;     /* stream frames through the motion detector, else capture only on request */
    int interval_ms;  /* between presence frames */
    int threshold;    /* motion_params_default(threshold) */
    int settle_frames; /* frames dropped before an on-request snapshot's frame */
};

/* The worker's main: runs in the forked child with sock as its end of the
 * socketpair, and never returns. On-request mode opens the camera for each
 * CMSG_SNAPSHOT and closes it after. Presence mode opens it once, keeps one
 * streaming session, scores a frame every interval_ms (a CMSG_TICK each),
 * and answers CMSG_SNAPSHOT with the latest frame. SIGTERM, or tt7d closing
 * the socket, ends it through cam_close (STREAMOFF, then the ion buffer is
 * freed). */
void camera_worker_run(int sock, const struct camera_worker_config *cfg) __attribute__((noreturn));

#endif
