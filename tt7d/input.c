/* ABOUTME: evdev discovery by capability (EVIOCGBIT/EVIOCGABS, or sysfs modalias for non-evdev test nodes),
 * ABOUTME: non-blocking reads of struct input_event records, and device loss handling with periodic rescans. */
#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* The stock 3.0.36 kernel hands out 16-byte events on 32-bit ARM, and musl's
 * __USE_TIME_BITS64 layout agrees (the same check as tt7probe.c). */
#if defined(__arm__)
_Static_assert(sizeof(struct input_event) == 16, "input_event must match the 32-bit ARM kernel ABI");
#endif

static int bit(const unsigned long *bits, unsigned n) { return (bits[n / INPUT_LONG_BITS] >> (n % INPUT_LONG_BITS)) & 1; }
static void set_bit(unsigned long *bits, unsigned n) { bits[n / INPUT_LONG_BITS] |= 1UL << (n % INPUT_LONG_BITS); }

void input_caps_from_modalias(const char *modalias, struct input_caps *caps) {
    memset(caps, 0, sizeof *caps);
    for (unsigned c = 0; c < EV_CNT; c++)
        if (modalias_has(modalias, 'e', c)) set_bit(caps->ev, c);
    for (unsigned c = 0; c < KEY_CNT; c++)
        if (modalias_has(modalias, 'k', c)) set_bit(caps->key, c);
    for (unsigned c = 0; c < ABS_CNT; c++)
        if (modalias_has(modalias, 'a', c)) set_bit(caps->abs, c);
}

int input_caps_from_fd(int fd, struct input_caps *caps) {
    memset(caps, 0, sizeof *caps);
    if (ioctl(fd, EVIOCGBIT(0, sizeof caps->ev), caps->ev) < 0) return -1;
    if (bit(caps->ev, EV_KEY)) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof caps->key), caps->key);
    if (bit(caps->ev, EV_ABS)) ioctl(fd, EVIOCGBIT(EV_ABS, sizeof caps->abs), caps->abs);
    return 0;
}

/* BTN_MISC..BTN_GEAR_UP and the BTN_TRIGGER_HAPPY range are pointer and
 * joystick buttons, not panel keys. */
static int is_panel_key(unsigned code) {
    return code < BTN_MISC || (code >= KEY_OK && code < BTN_TRIGGER_HAPPY);
}

int input_caps_keys(const struct input_caps *caps, int *codes, int max) {
    int n = 0;
    if (!bit(caps->ev, EV_KEY)) return 0;
    for (unsigned c = 1; c < KEY_CNT && n < max; c++)
        if (bit(caps->key, c) && is_panel_key(c)) codes[n++] = (int)c;
    return n;
}

enum input_role input_classify(const struct input_caps *caps, enum touch_protocol *proto) {
    if (bit(caps->ev, EV_ABS)) {
        if (bit(caps->abs, ABS_MT_POSITION_X) && bit(caps->abs, ABS_MT_POSITION_Y)) {
            *proto = bit(caps->abs, ABS_MT_SLOT) ? TOUCH_MT_B : TOUCH_MT_A;
            return INPUT_ROLE_TOUCH;
        }
        if (bit(caps->abs, ABS_X) && bit(caps->abs, ABS_Y) && bit(caps->ev, EV_KEY) && bit(caps->key, BTN_TOUCH)) {
            *proto = TOUCH_SINGLE;
            return INPUT_ROLE_TOUCH;
        }
    }
    int one;
    if (input_caps_keys(caps, &one, 1) > 0) return INPUT_ROLE_BUTTONS;
    return INPUT_ROLE_NONE;
}

const char *input_button_name(unsigned code, char *buf, size_t n) {
    switch (code) {
    case KEY_POWER: return "power";
    case KEY_VOLUMEUP: return "volume_up";
    case KEY_VOLUMEDOWN: return "volume_down";
    default: snprintf(buf, n, "key_%u", code); return buf;
    }
}

int input_absinfo_file(const char *path, int code, int32_t *min, int32_t *max) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[128];
    int found = -1;
    while (found != 0 && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *p = line, *end;
        long c = strtol(p, &end, 0);
        if (end == p) continue;
        long lo = strtol(p = end, &end, 0);
        if (end == p) continue;
        long hi = strtol(p = end, &end, 0);
        if (end == p || c != code) continue;
        *min = (int32_t)lo;
        *max = (int32_t)hi;
        found = 0;
    }
    fclose(f);
    return found;
}

/* ---- devices ---------------------------------------------------------------- */

void input_init(struct input *in, const char *dir, const char *sysfs_root, const struct input_sink *sink, FILE *log) {
    memset(in, 0, sizeof *in);
    snprintf(in->dir, sizeof in->dir, "%s", dir);
    in->sysfs_root = sysfs_root;
    in->sink = *sink;
    in->log = log;
    for (int i = 0; i < INPUT_MAX_DEVICES; i++) in->dev[i].fd = -1;
}

static void dev_close(struct input_device *d) {
    if (d->fd >= 0) close(d->fd);
    memset(d, 0, sizeof *d);
    d->fd = -1;
}

void input_close_all(struct input *in) {
    for (int i = 0; i < INPUT_MAX_DEVICES; i++) dev_close(&in->dev[i]);
}

const struct input_device *input_find(const struct input *in, enum input_role role, int nth) {
    for (int i = 0; i < INPUT_MAX_DEVICES; i++)
        if (in->dev[i].fd >= 0 && in->dev[i].role == role && nth-- == 0) return &in->dev[i];
    return NULL;
}

static int is_open_or_ignored(const struct input *in, const char *node) {
    for (int i = 0; i < INPUT_MAX_DEVICES; i++)
        if (in->dev[i].fd >= 0 && !strcmp(in->dev[i].node, node)) return 1;
    for (int i = 0; i < in->nignored; i++)
        if (!strcmp(in->ignored[i], node)) return 1;
    return 0;
}

static void ignore(struct input *in, const char *node) {
    if (in->nignored < MAX_NAMES) snprintf(in->ignored[in->nignored++], NAME_LEN, "%s", node);
}

/* Axis range: EVIOCGABS, or the <node>.absinfo file beside a non-evdev node. */
static int axis(const struct input *in, int fd, const char *node, int code, struct touch_axis *a, const char **src) {
    struct input_absinfo ai;
    if (ioctl(fd, EVIOCGABS(code), &ai) == 0) {
        a->min = ai.minimum;
        a->max = ai.maximum;
        *src = "EVIOCGABS";
        return 0;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.absinfo", in->dir, node);
    *src = "absinfo file";
    return input_absinfo_file(path, code, &a->min, &a->max);
}

/* Look at one eventN node: open it, and keep it if it is touch or buttons. */
static void consider(struct input *in, const char *sysdev, const char *node) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", in->dir, node);
    struct input_device *d = NULL;
    for (int i = 0; i < INPUT_MAX_DEVICES && !d; i++)
        if (in->dev[i].fd < 0) d = &in->dev[i];
    if (!d) return;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (in->scans == 0 && in->log) fprintf(in->log, "tt7d: input: cannot open %s: %s\n", path, strerror(errno));
        return;
    }
    struct input_caps caps;
    const char *caps_src = "EVIOCGBIT";
    char name[NAME_LEN] = "";
    if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0 || !name[0])
        read_attr(in->sysfs_root, "class/input", sysdev, "name", name, sizeof name);
    if (input_caps_from_fd(fd, &caps) != 0) {
        char mod[512];
        if (read_attr(in->sysfs_root, "class/input", sysdev, "modalias", mod, sizeof mod) != 0) mod[0] = 0;
        input_caps_from_modalias(mod, &caps);
        caps_src = "sysfs modalias";
    }
    enum touch_protocol proto = TOUCH_SINGLE;
    enum input_role role = input_classify(&caps, &proto);
    struct stat st;
    int is_file = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);

    memset(d, 0, sizeof *d);
    d->fd = fd;
    d->role = role;
    d->is_file = is_file;
    snprintf(d->node, sizeof d->node, "%s", node);
    snprintf(d->name, sizeof d->name, "%s", name);

    if (role == INPUT_ROLE_TOUCH) {
        int xc = proto == TOUCH_SINGLE ? ABS_X : ABS_MT_POSITION_X, yc = proto == TOUCH_SINGLE ? ABS_Y : ABS_MT_POSITION_Y;
        const char *src = "";
        if (axis(in, fd, node, xc, &d->ax, &src) != 0 || axis(in, fd, node, yc, &d->ay, &src) != 0 ||
            d->ax.max <= d->ax.min || d->ay.max <= d->ay.min) {
            if (in->log) fprintf(in->log, "tt7d: input: %s \"%s\": touch without usable axis ranges, ignored\n", node, name);
            dev_close(d);
            ignore(in, node);
            return;
        }
        struct touch_axis slots = {0, 0};
        int nslots = 1, first = 0;
        if (proto == TOUCH_MT_B) {
            struct input_absinfo ai;
            const char *ssrc;
            if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &ai) == 0) {
                slots.max = ai.maximum;
                first = ai.value;
            } else if (axis(in, fd, node, ABS_MT_SLOT, &slots, &ssrc) != 0) {
                slots.max = TOUCH_MAX_POINTERS - 1;
            }
            nslots = slots.max + 1;
        }
        d->proto = proto;
        touch_init(&d->touch, proto, nslots, first, INPUT_THROTTLE_MS);
        if (in->log)
            fprintf(in->log,
                    "tt7d: input: %s \"%s\": touch, %s, %d pointer(s), raw x %d..%d y %d..%d (%s; capabilities from %s)\n",
                    node, name, proto == TOUCH_MT_B ? "multitouch protocol B" : proto == TOUCH_MT_A ? "multitouch protocol A"
                                                                                         : "single touch",
                    d->touch.nslots, d->ax.min, d->ax.max, d->ay.min, d->ay.max, src, caps_src);
    } else if (role == INPUT_ROLE_BUTTONS) {
        d->nkeys = input_caps_keys(&caps, d->keys, INPUT_MAX_KEYS);
        if (in->log) {
            fprintf(in->log, "tt7d: input: %s \"%s\": buttons (capabilities from %s):", node, name, caps_src);
            for (int k = 0; k < d->nkeys; k++) {
                char buf[16];
                fprintf(in->log, " %d=%s", d->keys[k], input_button_name((unsigned)d->keys[k], buf, sizeof buf));
            }
            fprintf(in->log, "\n");
        }
    } else {
        if (in->log) fprintf(in->log, "tt7d: input: %s \"%s\": neither touch nor buttons, ignored\n", node, name);
        dev_close(d);
        ignore(in, node);
    }
}

void input_scan(struct input *in, int64_t now_ms) {
    struct names devs;
    list_dir(in->sysfs_root, "class/input", &devs);
    for (int i = 0; i < devs.n; i++) {
        if (strncmp(devs.v[i], "input", 5) != 0) continue;
        char rel[128];
        struct names children;
        snprintf(rel, sizeof rel, "class/input/%s", devs.v[i]);
        list_dir(in->sysfs_root, rel, &children);
        for (int k = 0; k < children.n; k++)
            if (!strncmp(children.v[k], "event", 5) && !is_open_or_ignored(in, children.v[k]))
                consider(in, devs.v[i], children.v[k]);
    }
    if (in->scans == 0 && in->log) {
        if (!input_find(in, INPUT_ROLE_TOUCH, 0))
            fprintf(in->log, "tt7d: input: no touchscreen found; looking again every %d s\n", INPUT_RESCAN_MS / 1000);
        if (!input_find(in, INPUT_ROLE_BUTTONS, 0))
            fprintf(in->log, "tt7d: input: no button device found; looking again every %d s\n", INPUT_RESCAN_MS / 1000);
    }
    in->scans++;
    in->next_scan_ms = now_ms + INPUT_RESCAN_MS;
}

static int want_scan(const struct input *in) {
    return !input_find(in, INPUT_ROLE_TOUCH, 0) || !input_find(in, INPUT_ROLE_BUTTONS, 0);
}

int input_prepare(struct input *in, struct pollfd *pfd, int max, int64_t *wait_ms, int64_t now_ms) {
    int n = 0;
    for (int i = 0; i < INPUT_MAX_DEVICES && n < max; i++) {
        struct input_device *d = &in->dev[i];
        if (d->fd < 0) continue;
        pfd[n] = (struct pollfd){.fd = d->fd, .events = POLLIN};
        in->pidx[n++] = i;
        int64_t due = d->role == INPUT_ROLE_TOUCH ? touch_next_due(&d->touch, now_ms) : -1;
        if (due >= 0 && (*wait_ms < 0 || due < *wait_ms)) *wait_ms = due;
    }
    if (want_scan(in)) {
        int64_t left = in->next_scan_ms > now_ms ? in->next_scan_ms - now_ms : 0;
        if (*wait_ms < 0 || left < *wait_ms) *wait_ms = left;
    }
    return n;
}

static void emit_touches(struct input *in, const struct input_device *d, const struct touch_out *out, int n) {
    for (int i = 0; i < n; i++)
        if (in->sink.touch) in->sink.touch(in->sink.ctx, d, &out[i]);
}

static void handle_event(struct input *in, struct input_device *d, const struct input_event *ev, int64_t now) {
    if (ev->type == EV_SYN && ev->code == SYN_DROPPED) { /* the kernel's buffer overflowed */
        d->dropping = 1;
        return;
    }
    if (d->dropping) {
        if (ev->type == EV_SYN && ev->code == SYN_REPORT) d->dropping = 0;
        return;
    }
    if (d->role == INPUT_ROLE_TOUCH) {
        struct touch_out out[2 * TOUCH_MAX_POINTERS];
        int n = touch_feed(&d->touch, ev->type, ev->code, ev->value, now, out, 2 * TOUCH_MAX_POINTERS);
        emit_touches(in, d, out, n);
    } else if (ev->type == EV_KEY && (ev->value == 0 || ev->value == 1)) { /* 2 = autorepeat */
        if (in->sink.button) in->sink.button(in->sink.ctx, d, ev->code, ev->value);
    }
}

/* Read what the device has. Returns -1 when it is gone (or finished). */
static int dev_read(struct input *in, struct input_device *d, int64_t now, const char **why) {
    for (int round = 0; round < 8; round++) {
        uint8_t buf[sizeof d->carry + 64 * sizeof(struct input_event)];
        memcpy(buf, d->carry, d->carry_len);
        ssize_t got = read(d->fd, buf + d->carry_len, sizeof buf - d->carry_len);
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            *why = got == 0 ? "end of file" : strerror(errno);
            return -1;
        }
        size_t have = d->carry_len + (size_t)got, off = 0;
        for (; have - off >= sizeof(struct input_event); off += sizeof(struct input_event)) {
            struct input_event ev;
            memcpy(&ev, buf + off, sizeof ev);
            handle_event(in, d, &ev, now);
            if (d->fd < 0) return 0;
        }
        d->carry_len = have - off;
        memcpy(d->carry, buf + off, d->carry_len);
    }
    return 0;
}

void input_service(struct input *in, const struct pollfd *pfd, int n, int64_t now_ms) {
    for (int k = 0; k < n; k++) {
        struct input_device *d = &in->dev[in->pidx[k]];
        if (d->fd != pfd[k].fd || d->fd < 0 || !pfd[k].revents) continue;
        const char *why = "";
        if (dev_read(in, d, now_ms, &why) != 0) {
            if (in->log) fprintf(in->log, "tt7d: input: %s \"%s\" closed (%s)\n", d->node, d->name, why);
            if (d->role == INPUT_ROLE_TOUCH) { /* fingers on a lost device will never send their up */
                struct touch_out out[TOUCH_MAX_POINTERS];
                emit_touches(in, d, out, touch_release_all(&d->touch, out, TOUCH_MAX_POINTERS));
            }
            if (d->is_file) ignore(in, d->node); /* a replayed file is done, not lost */
            dev_close(d);
            in->next_scan_ms = now_ms + INPUT_RESCAN_MS;
        }
    }
    for (int i = 0; i < INPUT_MAX_DEVICES; i++) {
        struct input_device *d = &in->dev[i];
        if (d->fd < 0 || d->role != INPUT_ROLE_TOUCH) continue;
        struct touch_out out[TOUCH_MAX_POINTERS];
        emit_touches(in, d, out, touch_flush(&d->touch, now_ms, out, TOUCH_MAX_POINTERS));
    }
    if (want_scan(in) && now_ms >= in->next_scan_ms) input_scan(in, now_ms);
}
