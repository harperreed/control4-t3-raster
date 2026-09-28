/* ABOUTME: tt7d's camera worker process: capture (cam/capture.c, or a test-only NV12 file), JPEG encode and
 * ABOUTME: presence scoring (cam/jpeg.c, cam/motion.c), reported to tt7d as camera_proto messages. */
#include "camera_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "camera_proto.h"
#include "capture.h"
#include "jpeg.h"
#include "motion.h"

#define FRAME_BYTES ((size_t)CAMERA_WIDTH * CAMERA_HEIGHT * 3 / 2)

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static int64_t mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- talking to tt7d ---- */

static int write_all(int fd, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_msg(int sock, uint32_t type, const void *payload, size_t len) {
    uint8_t h[CAMERA_MSG_HEADER];
    camera_msg_header(h, type, (uint32_t)len);
    return write_all(sock, h, sizeof h) == 0 && (len == 0 || write_all(sock, payload, len) == 0) ? 0 : -1;
}

static int send_error(int sock, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int send_error(int sock, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return send_msg(sock, CMSG_ERROR, msg, strlen(msg));
}

/* Wait up to timeout_ms (-1: forever) for a command from tt7d. Returns its
 * type, 0 on timeout or a signal, or -1 when tt7d closed the socket (or
 * sent something that is not a command). */
static int wait_command(int sock, int timeout_ms) {
    struct pollfd p = {.fd = sock, .events = POLLIN};
    if (poll(&p, 1, timeout_ms) <= 0) return 0;
    uint8_t h[CAMERA_MSG_HEADER];
    size_t got = 0;
    while (got < sizeof h) {
        ssize_t n = read(sock, h + got, sizeof h - got);
        if (n < 0 && errno == EINTR) {
            if (stop_requested) return -1;
            continue;
        }
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    uint32_t type, plen;
    const uint8_t *payload;
    if (camera_msg_parse(h, sizeof h, &type, &payload, &plen) != CAMERA_MSG_HEADER || type != CMSG_SNAPSHOT) return -1;
    return (int)type;
}

static int send_jpeg(int sock, const uint8_t *nv12) {
    struct jpeg_buf j;
    if (jpeg_encode_nv12(nv12, CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_JPEG_QUALITY, &j) < 0)
        return send_error(sock, "JPEG encode failed (out of memory?)");
    int rc = send_msg(sock, CMSG_JPEG, j.data, j.len);
    jpeg_buf_free(&j);
    return rc;
}

/* ---- frame source: the camera, or (tests only) an NV12 file or FIFO ---- */

struct source {
    const struct camera_worker_config *cfg;
    struct cam cam;
    int open;
    int fd;         /* fake source */
    int regular;    /* fake source is a regular file: loop at its end */
    uint8_t *buf;   /* fake source's frame */
    char *log_text; /* capture.c's step log for this session, kept in memory */
    size_t log_len;
    FILE *log;
};

/* capture.c logs every step. A snapshot every few seconds must not turn into
 * a stream of flash writes to app.log, so the steps go to memory and reach
 * stderr only when something failed. */
static void source_dump_log(struct source *s) {
    if (!s->log) return;
    fflush(s->log);
    if (s->log_len) fprintf(stderr, "tt7d: camera worker: capture log:\n%.*s", (int)s->log_len, s->log_text);
}

static void source_close(struct source *s) {
    if (s->cfg->fake_source) {
        if (s->fd >= 0) close(s->fd);
        s->fd = -1;
        free(s->buf);
        s->buf = NULL;
    } else {
        size_t before = s->log && fflush(s->log) == 0 ? s->log_len : 0;
        cam_close(&s->cam); /* STREAMOFF before the ion buffer goes away (capture.h) */
        /* cam_close logs only "STREAMOFF ok" when all went well; anything
         * else (a failed STREAMOFF, munmap, close or ION_FREE) goes to app.log. */
        if (s->log && fflush(s->log) == 0 && s->log_len > before &&
            strcmp(s->log_text + before, "tt7cam: STREAMOFF ok\n") != 0)
            source_dump_log(s);
        if (s->log) fclose(s->log);
        free(s->log_text);
        s->log = NULL;
        s->log_text = NULL;
        s->log_len = 0;
    }
    s->open = 0;
}

static int source_open(struct source *s, const struct camera_worker_config *cfg, char *err, size_t errlen) {
    memset(s, 0, sizeof *s);
    s->cfg = cfg;
    s->fd = -1;
    cam_init(&s->cam);
    if (cfg->fake_source) {
        s->fd = open(cfg->fake_source, O_RDONLY | O_CLOEXEC); /* a FIFO waits here for its writer */
        struct stat st;
        if (s->fd < 0 || fstat(s->fd, &st) != 0) {
            snprintf(err, errlen, "fake source %s: %s", cfg->fake_source, strerror(errno));
            return -1;
        }
        s->regular = S_ISREG(st.st_mode);
        if (!(s->buf = malloc(FRAME_BYTES))) {
            snprintf(err, errlen, "out of memory");
            return -1;
        }
        s->open = 1;
        return 0;
    }
    s->log = open_memstream(&s->log_text, &s->log_len);
    struct cam_config cc = {.device = cfg->device, .width = CAMERA_WIDTH, .height = CAMERA_HEIGHT,
                            .log = s->log ? s->log : stderr};
    s->open = 1;
    if (cam_open(&s->cam, &cc) < 0) {
        snprintf(err, errlen, "opening %s failed (capture log in app.log)", cfg->device);
        source_dump_log(s);
        return -1;
    }
    if (s->cam.width != CAMERA_WIDTH || s->cam.height != CAMERA_HEIGHT) {
        snprintf(err, errlen, "the driver chose %dx%d, not %dx%d", s->cam.width, s->cam.height, CAMERA_WIDTH,
                 CAMERA_HEIGHT);
        return -1;
    }
    return 0;
}

/* Reads a whole frame. A signal does not end the wait: this models a driver
 * call that does not come back, so only SIGKILL gets the worker out. */
static int fake_read(struct source *s) {
    size_t got = 0;
    int rewound = 0;
    while (got < FRAME_BYTES) {
        ssize_t n = read(s->fd, s->buf + got, FRAME_BYTES - got);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) {
            if (!s->regular || got || rewound || lseek(s->fd, 0, SEEK_SET) != 0) return -1;
            rewound = 1;
            continue;
        }
        got += (size_t)n;
    }
    return 0;
}

/* The next frame into *frame (valid until the next grab). Returns 0, or -1 with err. */
static int source_grab(struct source *s, const uint8_t **frame, char *err, size_t errlen) {
    if (s->cfg->fake_source) {
        if (fake_read(s) != 0) {
            snprintf(err, errlen, "fake source %s: no whole frame (end of the file or FIFO, or a read error)",
                     s->cfg->fake_source);
            return -1;
        }
        *frame = s->buf;
        return 0;
    }
    struct sentinel_report rep;
    int g = cam_grab(&s->cam, CAMERA_GRAB_TIMEOUT_MS, &rep);
    if (g < 0) {
        snprintf(err, errlen, "%s", stop_requested ? "stopped" : "no frame from the camera (capture log in app.log)");
        if (!stop_requested) source_dump_log(s);
        return -1;
    }
    if (g > 0) {
        snprintf(err, errlen, "frame %s: %d/%d luma rows never written", frame_verdict_name(rep.verdict),
                 rep.y_rows_sentinel, CAMERA_HEIGHT);
        return -1;
    }
    *frame = s->cam.va;
    return 0;
}

/* ---- the two modes ---- */

/* On request: open, let auto-exposure settle, keep one frame, close. */
static int snapshot_once(int sock, const struct camera_worker_config *cfg) {
    struct source s;
    char err[200];
    const uint8_t *frame = NULL;
    int rc = source_open(&s, cfg, err, sizeof err);
    for (int i = 0; rc == 0 && i <= cfg->settle_frames && !stop_requested; i++)
        rc = source_grab(&s, &frame, err, sizeof err);
    if (rc == 0 && stop_requested) rc = -1;
    int sent = rc == 0 ? send_jpeg(sock, frame) : stop_requested ? 0 : send_error(sock, "%s", err);
    source_close(&s);
    return sent;
}

static int run_on_request(int sock, const struct camera_worker_config *cfg) {
    while (!stop_requested) {
        int t = wait_command(sock, -1);
        if (t < 0) break;
        if (t == CMSG_SNAPSHOT && snapshot_once(sock, cfg) != 0) return 1;
    }
    return 0;
}

/* One streaming session for as long as presence is on: a frame every
 * interval_ms through the motion detector. The camera keeps streaming
 * between grabs with no buffer queued (capture.c queues one per grab). */
static int run_presence(int sock, const struct camera_worker_config *cfg) {
    struct source s;
    char err[200];
    const uint8_t *frame = NULL;
    if (source_open(&s, cfg, err, sizeof err) != 0) {
        send_error(sock, "%s", err);
        source_close(&s);
        return 1;
    }
    int rc = 0;
    /* The same auto-exposure settling as a snapshot, so the detector's
     * background does not start from a frame that is still brightening. */
    for (int i = 0; i < cfg->settle_frames && !stop_requested && rc == 0; i++)
        rc = source_grab(&s, &frame, err, sizeof err);

    struct motion_params mp = motion_params_default(cfg->threshold);
    struct motion_state ms;
    motion_init(&ms);
    struct camera_tick tick = {0};
    while (rc == 0 && !stop_requested) {
        int64_t next = mono_ms() + cfg->interval_ms;
        if ((rc = source_grab(&s, &frame, err, sizeof err)) != 0) break;
        float grid[MOTION_CELLS];
        motion_grid(frame, CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_WIDTH, grid);
        struct motion_result r = motion_step(&ms, &mp, grid);
        tick.present = r.presence;
        tick.frame++;
        tick.score = (float)r.score;
        if (send_msg(sock, CMSG_TICK, &tick, sizeof tick) != 0) break;
        /* The wait for the next frame is also the wait for commands: a
         * snapshot shares the stream and gets the frame just scored. */
        for (int64_t left; !stop_requested && (left = next - mono_ms()) > 0;) {
            int t = wait_command(sock, (int)left);
            if (t < 0) {
                stop_requested = 1;
                break;
            }
            if (t == CMSG_SNAPSHOT && send_jpeg(sock, frame) != 0) stop_requested = 1;
        }
    }
    if (rc != 0 && !stop_requested) send_error(sock, "%s", err);
    source_close(&s);
    return rc != 0 && !stop_requested;
}

void camera_worker_run(int sock, const struct camera_worker_config *cfg) {
    /* No SA_RESTART: a signal interrupts poll() so the loops see stop_requested. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    prctl(PR_SET_PDEATHSIG, SIGTERM); /* tt7d gone: stop the camera too */
    int rc = cfg->presence ? run_presence(sock, cfg) : run_on_request(sock, cfg);
    /* _exit: the stdio buffers and atexit handlers belong to the tt7d we forked from. */
    _exit(rc);
}
