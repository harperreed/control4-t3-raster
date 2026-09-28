/* ABOUTME: tt7cam: camera tool for the TT7 panel (probe, snap to JPEG, motion/presence, NV12 convert).
 * ABOUTME: Static ARM build runs on the panel; `convert` also runs on the host for pulled raw frames. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "jpeg.h"
#include "kabi.h"
#include "motion.h"
#include "sentinel.h"

static const char USAGE[] =
    "usage:\n"
    "  tt7cam probe   [--device /dev/video0]\n"
    "      Read-only: V4L2 caps/formats, ion/pmem/ipp device nodes, rk29_ipp in /proc/modules,\n"
    "      camera and ion regions in /proc/iomem, and the ioctl ABI this build assumes.\n"
    "  tt7cam snap OUT.jpg [--width 1280] [--height 720] [--quality 80] [--frames 30]\n"
    "                      [--raw OUT.nv12] [--timeout-ms 2000] [--device /dev/video0]\n"
    "      Captures --frames frames and keeps the last (the earlier ones let auto-exposure\n"
    "      settle). Every frame must overwrite the 0xAA sentinel, or it fails.\n"
    "  tt7cam motion  [--interval-ms 500] [--threshold 8] [--count 0] [--width 1280] [--height 720]\n"
    "                 [--timeout-ms 2000] [--device /dev/video0]\n"
    "      Prints {\"presence\":…,\"score\":…,\"ts\":…} on stdout at start and on each change.\n"
    "      --count N stops after N frames (0 = run until SIGINT/SIGTERM).\n"
    "  tt7cam convert IN.nv12 OUT.jpg --width W --height H [--quality 80]\n"
    "      NV12 file (e.g. from snap --raw) to JPEG, BT.601 limited range.\n"
    "exit: 0 ok, 1 capture/IO failure, 2 usage error\n";

static volatile sig_atomic_t stop_requested;
static void on_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

/* No SA_RESTART, so a signal interrupts poll() and nanosleep() and the
 * command returns through its normal cleanup path. */
static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

/* ---- option parsing ---- */

struct opts {
    const char *pos[2];
    int npos;
    const char *device, *raw;
    int width, height, quality, frames, timeout_ms, interval_ms, count;
    double threshold;
};

static int parse_int(const char *name, const char *s, int lo, int hi, int *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) {
        fprintf(stderr, "tt7cam: %s must be an integer in %d..%d, got '%s'\n", name, lo, hi, s);
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int parse_opts(int argc, char **argv, int max_pos, struct opts *o) {
    o->device = "/dev/video0";
    o->width = 1280; /* NT99141 native mode (MMKeypad CAMERA.md) */
    o->height = 720;
    o->quality = 80;
    o->frames = 30; /* MMKeypad: ~30 frames of auto-exposure warmup */
    o->timeout_ms = 2000;
    o->interval_ms = 500;
    o->threshold = 8.0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--", 2) != 0) {
            if (o->npos >= max_pos) {
                fprintf(stderr, "tt7cam: unexpected argument '%s'\n", a);
                return -1;
            }
            o->pos[o->npos++] = a;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "tt7cam: %s needs a value\n", a);
            return -1;
        }
        const char *v = argv[++i];
        int rc = 0;
        if (!strcmp(a, "--device")) o->device = v;
        else if (!strcmp(a, "--raw")) o->raw = v;
        else if (!strcmp(a, "--width")) rc = parse_int(a, v, 2, 4096, &o->width);
        else if (!strcmp(a, "--height")) rc = parse_int(a, v, 2, 4096, &o->height);
        else if (!strcmp(a, "--quality")) rc = parse_int(a, v, 1, 100, &o->quality);
        else if (!strcmp(a, "--frames")) rc = parse_int(a, v, 1, 10000, &o->frames);
        else if (!strcmp(a, "--timeout-ms")) rc = parse_int(a, v, 1, 60000, &o->timeout_ms);
        else if (!strcmp(a, "--interval-ms")) rc = parse_int(a, v, 0, 3600000, &o->interval_ms);
        else if (!strcmp(a, "--count")) rc = parse_int(a, v, 0, 1000000000, &o->count);
        else if (!strcmp(a, "--threshold")) {
            char *end;
            o->threshold = strtod(v, &end);
            if (end == v || *end || !(o->threshold > 0.0 && o->threshold <= 255.0)) {
                fprintf(stderr, "tt7cam: --threshold must be a number in (0, 255], got '%s'\n", v);
                rc = -1;
            }
        } else {
            fprintf(stderr, "tt7cam: unknown option %s\n", a);
            rc = -1;
        }
        if (rc) return -1;
    }
    if ((o->width & 1) || (o->height & 1)) {
        fprintf(stderr, "tt7cam: NV12 needs an even width and height\n");
        return -1;
    }
    return 0;
}

/* ---- file output ---- */

/* Writes to PATH.tmp, then renames, so a failure never leaves a truncated file at PATH. */
static int write_file(const char *path, const void *data, size_t len) {
    char tmp[4096];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) {
        fprintf(stderr, "tt7cam: path too long: %s\n", path);
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "tt7cam: open %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    const uint8_t *p = data;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            fprintf(stderr, "tt7cam: write %s: %s\n", tmp, n < 0 ? strerror(errno) : "short write");
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    if (close(fd) < 0 || rename(tmp, path) < 0) {
        fprintf(stderr, "tt7cam: finishing %s: %s\n", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int write_jpeg(const char *path, const uint8_t *nv12, int w, int h, int quality) {
    struct jpeg_buf j;
    if (jpeg_encode_nv12(nv12, w, h, quality, &j) < 0) {
        fprintf(stderr, "tt7cam: JPEG encode of %dx%d failed\n", w, h);
        return -1;
    }
    int rc = write_file(path, j.data, j.len);
    if (rc == 0) printf("wrote %s (%zu bytes, JPEG %dx%d q%d)\n", path, j.len, w, h, quality);
    jpeg_buf_free(&j);
    return rc;
}

/* ---- probe ---- */

static void probe_abi(void) {
    printf("abi: v4l2_buffer=%zu bytes (32-bit timeval), v4l2_format=%zu, ion_allocation_data=%zu\n",
           sizeof(struct k_v4l2_buffer), sizeof(struct k_v4l2_format), sizeof(struct k_ion_allocation_data));
    printf("abi: QUERYCAP=0x%08x ENUM_FMT=0x%08x G_FMT=0x%08x S_FMT=0x%08x REQBUFS=0x%08x\n", K_VIDIOC_QUERYCAP,
           K_VIDIOC_ENUM_FMT, K_VIDIOC_G_FMT, K_VIDIOC_S_FMT, K_VIDIOC_REQBUFS);
    printf("abi: QBUF=0x%08x DQBUF=0x%08x STREAMON=0x%08x STREAMOFF=0x%08x\n", K_VIDIOC_QBUF, K_VIDIOC_DQBUF,
           K_VIDIOC_STREAMON, K_VIDIOC_STREAMOFF);
    printf("abi: ION_ALLOC=0x%08x ION_FREE=0x%08x ION_MAP=0x%08x GET_PHYS=0x%08x CACHE_OP=0x%08x\n",
           K_ION_IOC_ALLOC, K_ION_IOC_FREE, K_ION_IOC_MAP, K_ION_CUSTOM_GET_PHYS, K_ION_CUSTOM_CACHE_OP);
}

static void fourcc_str(uint32_t f, char out[5]) {
    for (int i = 0; i < 4; i++) {
        char c = (char)((f >> (8 * i)) & 0xff);
        out[i] = (c >= 32 && c < 127) ? c : '?';
    }
    out[4] = 0;
}

/* Returns 0 if the device opened and answered QUERYCAP. */
static int probe_video(const char *device) {
    int fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        printf("video: open %s: %s\n", device, strerror(errno));
        return -1;
    }
    struct k_v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    int rc = 0;
    if (ioctl(fd, K_VIDIOC_QUERYCAP, &cap) < 0) {
        printf("video: QUERYCAP: %s\n", strerror(errno));
        rc = -1;
    } else {
        printf("video: QUERYCAP driver=%.16s card=%.32s bus=%.32s version=0x%08x caps=0x%08x (capture=%s "
               "streaming=%s)\n",
               (const char *)cap.driver, (const char *)cap.card, (const char *)cap.bus_info, cap.version,
               cap.capabilities, cap.capabilities & K_V4L2_CAP_VIDEO_CAPTURE ? "yes" : "no",
               cap.capabilities & K_V4L2_CAP_STREAMING ? "yes" : "no");
    }
    for (uint32_t i = 0; i < 64; i++) {
        struct k_v4l2_fmtdesc d;
        memset(&d, 0, sizeof d);
        d.index = i;
        d.type = K_V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, K_VIDIOC_ENUM_FMT, &d) < 0) {
            if (i == 0) printf("video: ENUM_FMT[0]: %s\n", strerror(errno));
            break;
        }
        char fc[5];
        fourcc_str(d.pixelformat, fc);
        printf("video: ENUM_FMT[%u] %s (0x%08x) flags=0x%x \"%.32s\"\n", i, fc, d.pixelformat, d.flags,
               (const char *)d.description);
    }
    struct k_v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = K_V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, K_VIDIOC_G_FMT, &f) < 0) {
        printf("video: G_FMT: %s\n", strerror(errno));
    } else {
        char fc[5];
        fourcc_str(f.fmt.pix.pixelformat, fc);
        printf("video: G_FMT %ux%u %s bytesperline=%u sizeimage=%u field=%u colorspace=%u\n", f.fmt.pix.width,
               f.fmt.pix.height, fc, f.fmt.pix.bytesperline, f.fmt.pix.sizeimage, f.fmt.pix.field,
               f.fmt.pix.colorspace);
    }
    close(fd);
    return rc;
}

static int name_matches(const char *name) {
    static const char *prefixes[] = {"ion", "pmem", "vipmem", "video", "rk29-ipp", "ipp", "camera"};
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
        if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return 1;
    return 0;
}

static int probe_dev_nodes(int *have_ion) {
    DIR *d = opendir("/dev");
    if (!d) {
        printf("dev: opendir /dev: %s\n", strerror(errno));
        return -1;
    }
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!name_matches(e->d_name)) continue;
        char path[300];
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        struct stat st;
        if (stat(path, &st) < 0) continue;
        if (S_ISCHR(st.st_mode))
            printf("dev: %s char %u,%u mode %03o\n", path, major(st.st_rdev), minor(st.st_rdev), st.st_mode & 0777);
        else
            printf("dev: %s (not a char device)\n", path);
        if (!strcmp(e->d_name, "ion")) *have_ion = 1;
        found++;
    }
    closedir(d);
    if (!found) printf("dev: no ion/pmem/vipmem/video/ipp nodes under /dev\n");
    return 0;
}

/* Prints matching lines of a /proc file; returns how many matched. */
static int grep_file(const char *path, const char *label, const char *const *needles, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("%s: open %s: %s\n", label, path, strerror(errno));
        return 0;
    }
    char line[512];
    int hits = 0;
    while (fgets(line, sizeof line, f)) {
        for (size_t i = 0; i < n; i++)
            if (strstr(line, needles[i])) {
                printf("%s: %s", label, line);
                if (!strchr(line, '\n')) putchar('\n');
                hits++;
                break;
            }
    }
    fclose(f);
    return hits;
}

static int cmd_probe(struct opts *o) {
    probe_abi();
    int video_ok = probe_video(o->device) == 0;
    int have_ion = 0;
    probe_dev_nodes(&have_ion);
    static const char *const ipp[] = {"rk29_ipp"};
    int ipp_loaded = grep_file("/proc/modules", "modules", ipp, 1) > 0;
    if (!ipp_loaded) printf("modules: rk29_ipp NOT loaded: the CIF->buffer copy would be a no-op stub\n");
    /* Busy regions: rk29_vipmem is the driver's capture area; rk_camera_vb is
     * our buffer while the driver maps it; ion is the carveout. */
    static const char *const regions[] = {"vipmem", "camera", "cam", "ion", "ipp"};
    grep_file("/proc/iomem", "iomem", regions, sizeof regions / sizeof regions[0]);
    printf("probe summary: video=%s ion=%s rk29_ipp=%s\n", video_ok ? "ok" : "FAIL", have_ion ? "present" : "MISSING",
           ipp_loaded ? "loaded" : "MISSING");
    return video_ok ? 0 : 1;
}

/* ---- snap ---- */

static int report_bad_frame(int frame, const struct sentinel_report *rep, int h) {
    if (rep->verdict == FRAME_UNTOUCHED)
        fprintf(stderr,
                "tt7cam: FAIL frame %d: sentinel 0xAA survived in every row. DQBUF returned but nothing wrote "
                "the buffer: ipp_blit_sync is the no-op stub (is rk29_ipp loaded?), the IPP wrote elsewhere, or the "
                "ion cache invalidate failed silently (dmesg: \"has not been maped\").\n",
                frame);
    else
        fprintf(stderr,
                "tt7cam: FAIL frame %d: partial capture, %d/%d luma rows and %d/%d chroma rows still 0xAA.\n",
                frame, rep->y_rows_sentinel, h, rep->uv_rows_sentinel, h / 2);
    return 1;
}

static int cmd_snap(struct opts *o) {
    const char *out = o->pos[0];
    struct cam c;
    cam_init(&c);
    struct cam_config cfg = {.device = o->device, .width = o->width, .height = o->height, .log = stderr};
    int rc = 1;
    if (cam_open(&c, &cfg) < 0) goto done;

    struct luma_stats st = {0};
    uint32_t prev_hash = 0;
    int distinct = 0;
    for (int i = 1; i <= o->frames; i++) {
        struct sentinel_report rep;
        int g = cam_grab(&c, o->timeout_ms, &rep);
        if (g < 0) {
            fprintf(stderr, "tt7cam: FAIL frame %d: %s\n", i, stop_requested ? "interrupted" : "no frame");
            goto done;
        }
        if (g > 0) {
            report_bad_frame(i, &rep, c.height);
            goto done;
        }
        luma_stats(c.va, (size_t)c.width * c.height, &st);
        if (i > 1 && st.hash != prev_hash) distinct++;
        prev_hash = st.hash;
        if (i == 1 || i == o->frames)
            fprintf(stderr, "tt7cam: frame %d/%d ok: seq=%u luma min=%u max=%u mean=%.1f\n", i, o->frames,
                    c.sequence, st.min, st.max, st.mean);
    }
    if (o->frames > 1)
        fprintf(stderr, "tt7cam: %d of %d consecutive frame pairs differ%s\n", distinct, o->frames - 1,
                distinct ? "" : " (WARN: identical frames: stale copy, not live video?)");

    if (o->raw) {
        if (write_file(o->raw, c.va, c.frame_len) < 0) goto done;
        printf("wrote %s (%zu bytes, NV12 %dx%d)\n", o->raw, c.frame_len, c.width, c.height);
    }
    if (luma_is_flat(&st)) {
        fprintf(stderr,
                "tt7cam: FAIL: frame written but flat (luma %u..%u): sensor asleep, IPP copying an idle "
                "buffer, or lens covered. %s\n",
                st.min, st.max, o->raw ? "Raw frame kept for inspection." : "Use --raw to keep it.");
        goto done;
    }
    if (write_jpeg(out, c.va, c.width, c.height, o->quality) < 0) goto done;
    printf("snap: ok\n");
    rc = 0;
done:
    cam_close(&c);
    return rc;
}

/* ---- motion ---- */

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL); /* a signal ends it early; the loop then sees stop_requested */
}

static int cmd_motion(struct opts *o) {
    struct cam c;
    cam_init(&c);
    struct cam_config cfg = {.device = o->device, .width = o->width, .height = o->height, .log = stderr};
    struct motion_params p = motion_params_default(o->threshold);
    struct motion_state s;
    motion_init(&s);
    int rc = 1;
    if (cam_open(&c, &cfg) < 0) goto done;

    for (int n = 0; !stop_requested && (o->count == 0 || n < o->count); n++) {
        struct sentinel_report rep;
        int g = cam_grab(&c, o->timeout_ms, &rep);
        if (g < 0) {
            if (stop_requested) break;
            fprintf(stderr, "tt7cam: FAIL: no frame\n");
            goto done;
        }
        if (g > 0) {
            report_bad_frame(n + 1, &rep, c.height);
            goto done;
        }
        float grid[MOTION_CELLS];
        motion_grid(c.va, c.width, c.height, c.width, grid);
        struct motion_result r = motion_step(&s, &p, grid);
        if (n == 0 || r.changed) {
            printf("{\"presence\":%s,\"score\":%.2f,\"ts\":%.3f}\n", r.presence ? "true" : "false", r.score,
                   now_seconds());
            fflush(stdout);
        }
        if (o->interval_ms) sleep_ms(o->interval_ms);
    }
    rc = 0;
done:
    cam_close(&c);
    return rc;
}

/* ---- convert ---- */

static int cmd_convert(struct opts *o) {
    const char *in = o->pos[0], *out = o->pos[1];
    size_t want = (size_t)o->width * o->height * 3 / 2;
    FILE *f = fopen(in, "rb");
    if (!f) {
        fprintf(stderr, "tt7cam: open %s: %s\n", in, strerror(errno));
        return 1;
    }
    uint8_t *buf = malloc(want + 1);
    size_t got = buf ? fread(buf, 1, want + 1, f) : 0;
    fclose(f);
    int rc = 1;
    if (!buf) {
        fprintf(stderr, "tt7cam: out of memory\n");
    } else if (got != want) {
        fprintf(stderr, "tt7cam: %s is %s%zu bytes; NV12 %dx%d is %zu\n", in, got > want ? "over " : "", got,
                o->width, o->height, want);
    } else {
        rc = write_jpeg(out, buf, o->width, o->height, o->quality) < 0;
    }
    free(buf);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        fputs(USAGE, argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    const char *cmd = argv[1];
    struct opts o;
    memset(&o, 0, sizeof o);
    int npos = !strcmp(cmd, "snap") ? 1 : !strcmp(cmd, "convert") ? 2 : 0;
    if (parse_opts(argc - 2, argv + 2, npos, &o) < 0) return 2;
    if (o.npos != npos) {
        fprintf(stderr, "tt7cam: %s takes %d file argument(s)\n%s", cmd, npos, USAGE);
        return 2;
    }
    install_signals();
    if (!strcmp(cmd, "probe")) return cmd_probe(&o);
    if (!strcmp(cmd, "snap")) return cmd_snap(&o);
    if (!strcmp(cmd, "motion")) return cmd_motion(&o);
    if (!strcmp(cmd, "convert")) {
        int has_size = 0;
        for (int i = 2; i < argc; i++) has_size += !strcmp(argv[i], "--width") || !strcmp(argv[i], "--height");
        if (has_size != 2) {
            fprintf(stderr, "tt7cam: convert needs --width and --height\n");
            return 2;
        }
        return cmd_convert(&o);
    }
    fprintf(stderr, "tt7cam: unknown command '%s'\n%s", cmd, USAGE);
    return 2;
}
