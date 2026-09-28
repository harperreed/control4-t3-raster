/* ABOUTME: Turns raw evdev touch events into down/move/up per pointer: multitouch protocol A and B, and single touch.
 * ABOUTME: Pure state machine plus the raw -> logical coordinate mapping; no I/O, so the unit tests replay sequences. */
#ifndef TT7D_TOUCH_H
#define TT7D_TOUCH_H

#include <stdint.h>

#define TOUCH_MAX_POINTERS 16

enum touch_protocol {
    TOUCH_MT_B,   /* ABS_MT_SLOT + ABS_MT_TRACKING_ID (the TT7's gslX680 advertises these) */
    TOUCH_MT_A,   /* anonymous contacts separated by SYN_MT_REPORT */
    TOUCH_SINGLE, /* ABS_X/ABS_Y + BTN_TOUCH */
};

enum touch_action { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };

/* One output event, in raw device units. */
struct touch_out {
    enum touch_action action;
    int pointer; /* the slot (protocol B), or a small id kept for the contact's lifetime */
    int32_t x, y;
};

struct touch_slot {
    int active, reported;   /* a contact is present / its down went out */
    int32_t x, y;           /* latest raw position */
    int have_x, have_y;
    int32_t sent_x, sent_y; /* the last position that went out */
    int64_t sent_ms;
    int pending;            /* a move is held back by the throttle */
    int changed, start, lift;
    int64_t key;            /* the tracking id (B); A: the tracking id, or -1 - its index in the report */
};

struct touch_contact { /* protocol A: one contact of the report being built */
    int32_t x, y, tid;
    int have_x, have_y, have_tid;
};

struct touch_state {
    enum touch_protocol proto;
    int throttle_ms;
    int nslots;
    int cur; /* protocol B: the slot ABS_MT_* values go to; -1 = out of range, ignored */
    struct touch_slot s[TOUCH_MAX_POINTERS];
    struct touch_contact pend, frame[TOUCH_MAX_POINTERS];
    int nframe;
};

/* nslots and first_slot matter for protocol B only (ABS_MT_SLOT's max + 1
 * and its current value); nslots is capped at TOUCH_MAX_POINTERS. */
void touch_init(struct touch_state *t, enum touch_protocol proto, int nslots, int first_slot, int throttle_ms);

/* Feed one evdev event read at now_ms. Appends what it completes (on
 * SYN_REPORT) to out and returns how many, at most max. Moves closer than
 * throttle_ms to the pointer's previous output are held back; the latest
 * position goes out via touch_flush() or right before the pointer's up. */
int touch_feed(struct touch_state *t, uint16_t type, uint16_t code, int32_t value, int64_t now_ms,
               struct touch_out *out, int max);

/* Emit the held-back moves whose interval has passed. */
int touch_flush(struct touch_state *t, int64_t now_ms, struct touch_out *out, int max);

/* Milliseconds until the next held-back move is due (0 = now), or -1 if none. */
int64_t touch_next_due(const struct touch_state *t, int64_t now_ms);

/* An up for every pointer that is down (the device went away). */
int touch_release_all(struct touch_state *t, struct touch_out *out, int max);

const char *touch_action_name(enum touch_action a);

/* A raw axis range from EVIOCGABS. */
struct touch_axis {
    int32_t min, max;
};

/* Raw device coordinates -> the logical image's pixel. The raw ranges are
 * scaled onto the native framebuffer as tt7probe draws its dots (x across
 * native_w, y down native_h), then turned back by the display's rotation
 * with render_unmap(), the inverse of the mapping frames are drawn with.
 * Returns 0, or -1 for an empty axis range. */
int touch_to_logical(struct touch_axis ax, struct touch_axis ay, int rotation, uint32_t native_w, uint32_t native_h,
                     int32_t rx, int32_t ry, uint32_t *lx, uint32_t *ly);

#endif
