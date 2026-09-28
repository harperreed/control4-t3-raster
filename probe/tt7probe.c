/* ABOUTME: TT7 hardware probe: draws a test pattern on /dev/fb0 and logs raw input events.
 * ABOUTME: `fbinfo` prints the fb ioctls; `run <dir>` draws, then logs input forever; `log <dir>` only logs. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "fbdraw.h"

/* The stock 3.0.36 kernel hands out 16-byte events on 32-bit ARM. musl
 * defines __USE_TIME_BITS64, which makes linux/input.h use two longs instead
 * of a 64-bit struct timeval, so the sizes agree. Fail the build if not. */
#if defined(__arm__)
_Static_assert(sizeof(struct input_event) == 16, "input_event must match the 32-bit ARM kernel ABI");
#endif

#define FB_DEV "/dev/fb0"
#define FB_SYS "/sys/class/graphics/fb0/dev"
#define INPUT_SYS "/sys/class/input"
#define MAX_DEVS 16
#define LOG_CAP (4u << 20) /* input-events.log stops growing at 4 MiB per boot */

/* ---- device nodes ---------------------------------------------------------
 * This kernel has no working devtmpfs (see init.c), so /dev is a tmpfs that
 * init fills by hand. Make the nodes we need from their sysfs "maj:min". */
static int ensure_node(const char *devpath, const char *syspath) {
    if (access(devpath, F_OK) == 0) return 0;
    FILE *f = fopen(syspath, "r");
    if (!f) return -1;
    unsigned maj, min;
    int n = fscanf(f, "%u:%u", &maj, &min);
    fclose(f);
    if (n != 2) return -1;
    return mknod(devpath, S_IFCHR | 0600, makedev(maj, min));
}

/* ---- framebuffer ---------------------------------------------------------- */
struct fb {
    int fd;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    uint8_t *map;
    struct fbd_surface surf;
    int ready; /* surf is drawable */
};

static void print_fbinfo(FILE *out, const struct fb_var_screeninfo *v, const struct fb_fix_screeninfo *f) {
    fprintf(out, "FBIOGET_FSCREENINFO\n");
    fprintf(out, "  id           %.16s\n", f->id);
    fprintf(out, "  smem_start   0x%lx\n", (unsigned long)f->smem_start);
    fprintf(out, "  smem_len     %u\n", f->smem_len);
    fprintf(out, "  type         %u\n", f->type);
    fprintf(out, "  visual       %u\n", f->visual);
    fprintf(out, "  xpanstep     %u  ypanstep %u  ywrapstep %u\n", f->xpanstep, f->ypanstep, f->ywrapstep);
    fprintf(out, "  line_length  %u\n", f->line_length);
    fprintf(out, "  accel        %u\n", f->accel);
    fprintf(out, "FBIOGET_VSCREENINFO\n");
    fprintf(out, "  xres         %u  yres %u\n", v->xres, v->yres);
    fprintf(out, "  virtual      %u x %u\n", v->xres_virtual, v->yres_virtual);
    fprintf(out, "  offset       %u, %u\n", v->xoffset, v->yoffset);
    fprintf(out, "  bits/pixel   %u  grayscale %u  nonstd %u\n", v->bits_per_pixel, v->grayscale, v->nonstd);
    fprintf(out, "  red          offset %u length %u msb_right %u\n", v->red.offset, v->red.length, v->red.msb_right);
    fprintf(out, "  green        offset %u length %u msb_right %u\n", v->green.offset, v->green.length, v->green.msb_right);
    fprintf(out, "  blue         offset %u length %u msb_right %u\n", v->blue.offset, v->blue.length, v->blue.msb_right);
    fprintf(out, "  transp       offset %u length %u msb_right %u\n", v->transp.offset, v->transp.length, v->transp.msb_right);
    fprintf(out, "  activate     %u\n", v->activate);
    fprintf(out, "  size_mm      %u x %u\n", v->width, v->height);
    fprintf(out, "  pixclock_ps  %u\n", v->pixclock);
    fprintf(out, "  margins      left %u right %u upper %u lower %u\n", v->left_margin, v->right_margin,
            v->upper_margin, v->lower_margin);
    fprintf(out, "  sync_len     h %u v %u  sync 0x%x  vmode 0x%x  rotate %u\n", v->hsync_len, v->vsync_len,
            v->sync, v->vmode, v->rotate);
}

static int fb_open(struct fb *fb) {
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;
    if (ensure_node(FB_DEV, FB_SYS) != 0 && access(FB_DEV, F_OK) != 0) {
        fprintf(stderr, "tt7probe: no %s and no usable %s\n", FB_DEV, FB_SYS);
        return -1;
    }
    fb->fd = open(FB_DEV, O_RDWR);
    if (fb->fd < 0) {
        fprintf(stderr, "tt7probe: open %s: %s\n", FB_DEV, strerror(errno));
        return -1;
    }
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) != 0 || ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) != 0) {
        fprintf(stderr, "tt7probe: fb ioctls: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Map the fb and point the surface at the window the display shows now. */
static int fb_map(struct fb *fb) {
    const struct fb_var_screeninfo *v = &fb->var;
    size_t len = fb->fix.smem_len;
    fb->map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->map == MAP_FAILED) {
        fb->map = NULL;
        fprintf(stderr, "tt7probe: mmap %zu bytes: %s\n", len, strerror(errno));
        return -1;
    }
    size_t off = (size_t)v->yoffset * fb->fix.line_length + (size_t)v->xoffset * (v->bits_per_pixel / 8);
    size_t need = off + (size_t)v->yres * fb->fix.line_length;
    if (need > len) {
        fprintf(stderr, "tt7probe: offset %u,%u window overruns smem_len %zu; drawing at 0,0\n", v->xoffset,
                v->yoffset, len);
        off = 0;
        if ((size_t)v->yres * fb->fix.line_length > len) return -1;
    }
    struct fbd_chan r = {v->red.offset, v->red.length}, g = {v->green.offset, v->green.length};
    struct fbd_chan b = {v->blue.offset, v->blue.length}, a = {v->transp.offset, v->transp.length};
    if (fbd_init(&fb->surf, fb->map + off, v->xres, v->yres, fb->fix.line_length, v->bits_per_pixel, r, g, b, a) != 0) {
        fprintf(stderr, "tt7probe: cannot draw %u bpp\n", v->bits_per_pixel);
        return -1;
    }
    fb->ready = 1;
    return 0;
}

static void fb_draw_pattern(struct fb *fb) {
    const struct fb_var_screeninfo *v = &fb->var;
    const struct fbd_surface *s = &fb->surf;
    char l0[64], l1[64], l2[64], l3[64];
    snprintf(l0, sizeof l0, "fb0 %ux%u %ubpp", v->xres, v->yres, v->bits_per_pixel);
    snprintf(l1, sizeof l1, "R%u/%u G%u/%u B%u/%u A%u/%u", s->red.offset, s->red.length, s->green.offset,
             s->green.length, s->blue.offset, s->blue.length, s->transp.offset, s->transp.length);
    snprintf(l2, sizeof l2, "stride %u virt %ux%u", fb->fix.line_length, v->xres_virtual, v->yres_virtual);
    snprintf(l3, sizeof l3, "offset %u,%u id %.16s", v->xoffset, v->yoffset, fb->fix.id);
    const char *info[] = {l0, l1, l2, l3, NULL};
    fbd_test_pattern(s, info);
    /* Some drivers only latch new contents on a pan; re-pan to where we are. */
    if (ioctl(fb->fd, FBIOPAN_DISPLAY, &fb->var) != 0)
        fprintf(stderr, "tt7probe: FBIOPAN_DISPLAY (harmless if unsupported): %s\n", strerror(errno));
}

/* ---- input ----------------------------------------------------------------- */
enum { AX_X, AX_Y, AX_MT_X, AX_MT_Y, AX_N };
static const int axis_code[AX_N] = {ABS_X, ABS_Y, ABS_MT_POSITION_X, ABS_MT_POSITION_Y};

struct dev {
    int fd;
    int num; /* N in eventN */
    char name[128];
    int has[AX_N];
    struct input_absinfo abs[AX_N];
    int32_t val[AX_N];
    int seen[AX_N];
    int dirty; /* a position changed since the last dot */
};

#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define NLONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
static int test_bit(const unsigned long *bits, int n) { return (bits[n / BITS_PER_LONG] >> (n % BITS_PER_LONG)) & 1; }

static const char *ev_type_name(unsigned t) {
    switch (t) {
    case EV_SYN: return "SYN";
    case EV_KEY: return "KEY";
    case EV_REL: return "REL";
    case EV_ABS: return "ABS";
    case EV_MSC: return "MSC";
    case EV_SW: return "SW";
    case EV_LED: return "LED";
    case EV_REP: return "REP";
    default: return "?";
    }
}

/* Describe one device (identity, event types, keys, absinfo) to `out`. */
static void describe_dev(FILE *out, struct dev *d) {
    char phys[128] = "", uniq[128] = "";
    struct input_id id = {0};
    ioctl(d->fd, EVIOCGPHYS(sizeof phys - 1), phys);
    ioctl(d->fd, EVIOCGUNIQ(sizeof uniq - 1), uniq);
    ioctl(d->fd, EVIOCGID, &id);
    fprintf(out, "/dev/input/event%d\n  name    \"%s\"\n  phys    \"%s\"\n  uniq    \"%s\"\n", d->num, d->name, phys, uniq);
    fprintf(out, "  id      bus 0x%04x vendor 0x%04x product 0x%04x version 0x%04x\n", id.bustype, id.vendor,
            id.product, id.version);

    unsigned long evbits[NLONGS(EV_CNT)] = {0};
    ioctl(d->fd, EVIOCGBIT(0, sizeof evbits), evbits);
    fprintf(out, "  types  ");
    for (int t = 0; t < EV_CNT; t++)
        if (test_bit(evbits, t)) fprintf(out, " %s(%d)", ev_type_name((unsigned)t), t);
    fprintf(out, "\n");

    if (test_bit(evbits, EV_KEY)) {
        unsigned long keybits[NLONGS(KEY_CNT)] = {0};
        ioctl(d->fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
        fprintf(out, "  keys   ");
        for (int k = 0; k < KEY_CNT; k++)
            if (test_bit(keybits, k)) fprintf(out, " %d(0x%x)", k, k);
        fprintf(out, "\n");
    }
    if (test_bit(evbits, EV_ABS)) {
        unsigned long absbits[NLONGS(ABS_CNT)] = {0};
        ioctl(d->fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
        for (int c = 0; c < ABS_CNT; c++) {
            if (!test_bit(absbits, c)) continue;
            struct input_absinfo ai;
            if (ioctl(d->fd, EVIOCGABS(c), &ai) != 0) continue;
            fprintf(out, "  abs 0x%02x  value %d min %d max %d fuzz %d flat %d resolution %d\n", c, ai.value,
                    ai.minimum, ai.maximum, ai.fuzz, ai.flat, ai.resolution);
            for (int a = 0; a < AX_N; a++)
                if (axis_code[a] == c) {
                    d->has[a] = 1;
                    d->abs[a] = ai;
                }
        }
    }
}

static int is_event_node(const char *name, int *num) {
    char tail;
    return sscanf(name, "event%d%c", num, &tail) == 1;
}

/* Open every eventN listed in sysfs, making its /dev node if needed. */
static int open_inputs(struct dev *devs, FILE *desc) {
    mkdir("/dev/input", 0755);
    DIR *dir = opendir(INPUT_SYS);
    if (!dir) {
        fprintf(desc, "no %s: %s\n", INPUT_SYS, strerror(errno));
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && n < MAX_DEVS) {
        int num;
        if (!is_event_node(e->d_name, &num)) continue;
        char devpath[64], syspath[128];
        snprintf(devpath, sizeof devpath, "/dev/input/event%d", num);
        snprintf(syspath, sizeof syspath, INPUT_SYS "/event%d/dev", num);
        ensure_node(devpath, syspath);
        int fd = open(devpath, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            fprintf(desc, "%s: open failed: %s\n", devpath, strerror(errno));
            continue;
        }
        struct dev *d = &devs[n++];
        memset(d, 0, sizeof *d);
        d->fd = fd;
        d->num = num;
        if (ioctl(fd, EVIOCGNAME(sizeof d->name - 1), d->name) < 0) snprintf(d->name, sizeof d->name, "?");
        describe_dev(desc, d);
    }
    closedir(dir);
    return n;
}

/* Remember the latest raw position; ABS_MT_* wins over ABS_X/Y when both come. */
static void track_position(struct dev *d, const struct input_event *ev) {
    for (int a = 0; a < AX_N; a++)
        if (ev->type == EV_ABS && ev->code == axis_code[a]) {
            d->val[a] = ev->value;
            d->seen[a] = 1;
            d->dirty = 1;
        }
}

/* On SYN_REPORT / SYN_MT_REPORT, draw a dot at the raw position, scaled from
 * the device's own absinfo range straight onto fb x/y with no rotation. */
static void maybe_draw_dot(struct dev *d, const struct input_event *ev, struct fb *fb) {
    if (ev->type != EV_SYN || (ev->code != SYN_REPORT && ev->code != SYN_MT_REPORT) || !d->dirty) return;
    d->dirty = 0;
    int ax = AX_MT_X, ay = AX_MT_Y;
    if (!(d->seen[ax] && d->seen[ay] && d->has[ax] && d->has[ay])) {
        ax = AX_X;
        ay = AX_Y;
    }
    if (!(d->seen[ax] && d->seen[ay] && d->has[ax] && d->has[ay]) || !fb->ready) return;
    int x = fbd_scale(d->val[ax], d->abs[ax].minimum, d->abs[ax].maximum, fb->surf.width);
    int y = fbd_scale(d->val[ay], d->abs[ay].minimum, d->abs[ay].maximum, fb->surf.height);
    if (x >= 0 && y >= 0) fbd_dot(&fb->surf, x, y, fbd_pack(&fb->surf, 255, 0, 255));
}

/* ---- logging --------------------------------------------------------------- */
struct evlog {
    int fd;
    off_t size;
    int capped;
    struct timespec last_sync;
    int unsynced;
};

static void evlog_write(struct evlog *lg, const char *fmt, ...) {
    if (lg->fd < 0 || lg->capped) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = sizeof line - 1;
    if (lg->size + n > (off_t)LOG_CAP) {
        static const char msg[] = "# log cap reached (4 MiB); later events are not logged, dots still drawn\n";
        if (write(lg->fd, msg, sizeof msg - 1) > 0) lg->size += (off_t)sizeof msg - 1;
        lg->capped = 1;
        fdatasync(lg->fd);
        return;
    }
    if (write(lg->fd, line, (size_t)n) == n) {
        lg->size += n;
        lg->unsynced = 1;
    }
}

/* Push buffered lines to flash at most once a second, so a power pull after
 * touching the panel still leaves the events on /data. */
static void evlog_maybe_sync(struct evlog *lg) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (lg->unsynced && now.tv_sec != lg->last_sync.tv_sec) {
        fdatasync(lg->fd);
        lg->unsynced = 0;
        lg->last_sync = now;
    }
}

static FILE *open_out(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) fprintf(stderr, "tt7probe: %s: %s\n", path, strerror(errno));
    return f;
}

/* ---- commands -------------------------------------------------------------- */
static int cmd_fbinfo(void) {
    struct fb fb;
    if (fb_open(&fb) != 0) return 1;
    print_fbinfo(stdout, &fb.var, &fb.fix);
    return 0;
}

/* draw = 0 (`log`): leave the framebuffer to tt7d and only log input. */
static _Noreturn void cmd_run(const char *outdir, int draw) {
    struct fb fb;
    memset(&fb, 0, sizeof fb); /* fb.ready stays 0 when not drawing: no touch dots */
    fb.fd = -1;
    FILE *f = draw ? open_out(outdir, "fb-ioctl.txt") : NULL;
    if (draw && fb_open(&fb) == 0) {
        if (f) print_fbinfo(f, &fb.var, &fb.fix);
        if (ioctl(fb.fd, FBIOBLANK, FB_BLANK_UNBLANK) != 0)
            fprintf(stderr, "tt7probe: FBIOBLANK unblank: %s\n", strerror(errno));
        if (fb_map(&fb) == 0) {
            fb_draw_pattern(&fb);
            printf("tt7probe: test pattern drawn on %ux%u %ubpp\n", fb.var.xres, fb.var.yres, fb.var.bits_per_pixel);
        }
    } else if (f) {
        fprintf(f, "fb0 unavailable (see tt7probe.log)\n");
    }
    if (f) fclose(f);

    static struct dev devs[MAX_DEVS];
    FILE *desc = open_out(outdir, "input-devices.txt");
    int ndev = open_inputs(devs, desc ? desc : stderr);
    if (desc) fclose(desc);
    printf("tt7probe: logging %d input device(s)\n", ndev);
    fflush(stdout);

    char path[512];
    snprintf(path, sizeof path, "%s/input-events.log", outdir);
    struct evlog lg = {.fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644)};
    struct stat st;
    if (lg.fd >= 0 && fstat(lg.fd, &st) == 0) lg.size = st.st_size; /* respawns append to one capped file */
    if (lg.fd < 0) fprintf(stderr, "tt7probe: %s: %s\n", path, strerror(errno));
    evlog_write(&lg, "# tt7probe start: %d device(s); fields: sec.usec eventN \"name\" type code value\n", ndev);

    struct pollfd pfd[MAX_DEVS];
    for (;;) {
        int live = 0;
        for (int i = 0; i < ndev; i++) {
            pfd[i].fd = devs[i].fd; /* -1 once a device is gone: poll skips it */
            pfd[i].events = POLLIN;
            if (devs[i].fd >= 0) live++;
        }
        if (live == 0) { /* nothing to read: keep the pattern up, stay alive */
            pause();
            continue;
        }
        if (poll(pfd, (nfds_t)ndev, 1000) < 0 && errno != EINTR) {
            fprintf(stderr, "tt7probe: poll: %s\n", strerror(errno));
            sleep(1);
        }
        for (int i = 0; i < ndev; i++) {
            if (devs[i].fd < 0 || !(pfd[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            struct input_event evs[64];
            ssize_t r = read(devs[i].fd, evs, sizeof evs);
            if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (r <= 0) {
                evlog_write(&lg, "# event%d \"%s\" gone: %s\n", devs[i].num, devs[i].name, r < 0 ? strerror(errno) : "EOF");
                close(devs[i].fd);
                devs[i].fd = -1;
                continue;
            }
            for (size_t k = 0; k < (size_t)r / sizeof evs[0]; k++) {
                const struct input_event *ev = &evs[k];
                evlog_write(&lg, "%lu.%06lu event%d \"%s\" %s(%u) 0x%03x %d\n", (unsigned long)ev->input_event_sec,
                            (unsigned long)ev->input_event_usec, devs[i].num, devs[i].name,
                            ev_type_name(ev->type), ev->type, ev->code, ev->value);
                track_position(&devs[i], ev);
                maybe_draw_dot(&devs[i], ev, &fb);
            }
        }
        evlog_maybe_sync(&lg);
    }
}

static void usage(FILE *out) {
    fprintf(out,
            "usage: tt7probe fbinfo        print FBIOGET_FSCREENINFO/VSCREENINFO (an fbset stand-in)\n"
            "       tt7probe run <outdir>  draw the test pattern on /dev/fb0, write fb-ioctl.txt and\n"
            "                              input-devices.txt to <outdir>, then append every input event\n"
            "                              to <outdir>/input-events.log (capped at 4 MiB) and draw a dot\n"
            "                              at each raw touch position. Never returns.\n"
            "       tt7probe log <outdir>  like run, but never opens the framebuffer: only\n"
            "                              input-devices.txt and input-events.log (runs beside tt7d).\n");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "fbinfo") == 0) return cmd_fbinfo();
    if (argc == 3 && strcmp(argv[1], "run") == 0) cmd_run(argv[2], 1);
    if (argc == 3 && strcmp(argv[1], "log") == 0) cmd_run(argv[2], 0);
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(stdout);
        return 0;
    }
    usage(stderr);
    return 2;
}
