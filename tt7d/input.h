/* ABOUTME: Finds the touchscreen and button devices by capability, reads their evdev records in the poll loop,
 * ABOUTME: and hands touches (via touch.c) and key presses to a sink. Lost devices are closed and looked for again. */
#ifndef TT7D_INPUT_H
#define TT7D_INPUT_H

#include <linux/input.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "sysinfo.h"
#include "touch.h"

#define INPUT_MAX_DEVICES 8
#define INPUT_MAX_KEYS 32
#define INPUT_RESCAN_MS 5000  /* while a touchscreen or button device is missing */
#define INPUT_THROTTLE_MS 16  /* at most one move per pointer per 16 ms (about 60 per second) */

#define INPUT_LONG_BITS (8 * sizeof(unsigned long))
#define INPUT_NLONGS(n) (((n) + INPUT_LONG_BITS - 1) / INPUT_LONG_BITS)

/* What a device can send, as EVIOCGBIT reports it (or its sysfs modalias). */
struct input_caps {
    unsigned long ev[INPUT_NLONGS(EV_CNT)];
    unsigned long key[INPUT_NLONGS(KEY_CNT)];
    unsigned long abs[INPUT_NLONGS(ABS_CNT)];
};

enum input_role { INPUT_ROLE_NONE, INPUT_ROLE_TOUCH, INPUT_ROLE_BUTTONS };

/* Fill caps from a modalias ("input:b...-e0,1,3,k...,a2F,35,..."). */
void input_caps_from_modalias(const char *modalias, struct input_caps *caps);

/* Fill caps with EVIOCGBIT. Returns -1 if fd is not an evdev device. */
int input_caps_from_fd(int fd, struct input_caps *caps);

/* The key codes a button device has, in order: everything except the BTN_*
 * ranges (mouse, joystick and touch buttons). Returns how many (at most max). */
int input_caps_keys(const struct input_caps *caps, int *codes, int max);

/* Touch = ABS_MT_POSITION_X/Y (protocol B if it also has ABS_MT_SLOT, else A),
 * or ABS_X/Y plus BTN_TOUCH (single touch). Buttons = EV_KEY with at least one
 * code from input_caps_keys(). Touch wins if both. */
enum input_role input_classify(const struct input_caps *caps, enum touch_protocol *proto);

/* "power", "volume_up", "volume_down", else "key_<code>" (written into buf). */
const char *input_button_name(unsigned code, char *buf, size_t n);

/* Look up an axis range in an absinfo file: lines "<code> <min> <max>" (code
 * in C syntax, e.g. 0x35; '#' starts a comment). Used for input nodes that
 * are not evdev devices (the FIFOs of the host tests), where EVIOCGABS fails.
 * Returns 0, or -1 if the file or the code is missing. */
int input_absinfo_file(const char *path, int code, int32_t *min, int32_t *max);

struct input_device {
    int fd; /* -1: a free slot */
    char node[NAME_LEN]; /* "event1" */
    char name[NAME_LEN];
    enum input_role role;
    int is_file; /* a regular file (replay): its end is the end, not a lost device */
    struct touch_state touch;
    enum touch_protocol proto;
    struct touch_axis ax, ay;
    int keys[INPUT_MAX_KEYS];
    int nkeys;
    int dropping; /* after SYN_DROPPED: skip to the next SYN_REPORT */
    uint8_t carry[sizeof(struct input_event)];
    size_t carry_len;
};

struct input_sink {
    void *ctx;
    void (*touch)(void *ctx, const struct input_device *d, const struct touch_out *o);
    void (*button)(void *ctx, const struct input_device *d, int code, int pressed);
};

struct input {
    char dir[256];          /* where the eventN nodes are (--input-dir) */
    const char *sysfs_root; /* class/input/inputN/eventM names the nodes */
    struct input_device dev[INPUT_MAX_DEVICES];
    int pidx[INPUT_MAX_DEVICES];
    char ignored[MAX_NAMES][NAME_LEN]; /* nodes already looked at and not used */
    int nignored;
    int scans;
    int64_t next_scan_ms;
    struct input_sink sink;
    FILE *log;
};

void input_init(struct input *in, const char *dir, const char *sysfs_root, const struct input_sink *sink, FILE *log);
void input_close_all(struct input *in);

/* Open every touchscreen and button device not open yet. */
void input_scan(struct input *in, int64_t now_ms);

/* The first open device with this role, or NULL. */
const struct input_device *input_find(const struct input *in, enum input_role role, int nth);

/* The poll loop hooks, as in ws.h. */
int input_prepare(struct input *in, struct pollfd *pfd, int max, int64_t *wait_ms, int64_t now_ms);
void input_service(struct input *in, const struct pollfd *pfd, int n, int64_t now_ms);

#endif
