/* ABOUTME: evdev touch state machine (MT protocol A and B, single touch) with per-pointer move throttling,
 * ABOUTME: following the kernel's Documentation/input/multi-touch-protocol.txt; plus raw -> logical mapping. */
#include "touch.h"

#include <linux/input.h>
#include <string.h>

#include "fbdraw.h"
#include "render.h"

void touch_init(struct touch_state *t, enum touch_protocol proto, int nslots, int first_slot, int throttle_ms) {
    memset(t, 0, sizeof *t);
    t->proto = proto;
    t->throttle_ms = throttle_ms;
    t->nslots = proto == TOUCH_MT_B ? nslots : proto == TOUCH_MT_A ? TOUCH_MAX_POINTERS : 1;
    if (t->nslots > TOUCH_MAX_POINTERS) t->nslots = TOUCH_MAX_POINTERS;
    if (t->nslots < 1) t->nslots = 1;
    t->cur = first_slot >= 0 && first_slot < t->nslots ? first_slot : 0;
}

const char *touch_action_name(enum touch_action a) {
    return a == TOUCH_DOWN ? "down" : a == TOUCH_MOVE ? "move" : "up";
}

static int push(struct touch_out *out, int max, int n, enum touch_action a, int pointer, int32_t x, int32_t y) {
    if (n >= max) return n;
    out[n] = (struct touch_out){.action = a, .pointer = pointer, .x = x, .y = y};
    return n + 1;
}

static void mark_sent(struct touch_slot *s, int64_t now) {
    s->sent_x = s->x;
    s->sent_y = s->y;
    s->sent_ms = now;
    s->pending = 0;
}

/* SYN_REPORT: turn each slot's changes into outputs, in slot order. */
static int report(struct touch_state *t, int64_t now, struct touch_out *out, int max) {
    int n = 0;
    for (int i = 0; i < t->nslots; i++) {
        struct touch_slot *s = &t->s[i];
        if (s->lift) {
            if (s->active && s->reported) {
                if (s->x != s->sent_x || s->y != s->sent_y) n = push(out, max, n, TOUCH_MOVE, i, s->x, s->y);
                n = push(out, max, n, TOUCH_UP, i, s->x, s->y);
            }
            s->active = s->reported = s->pending = s->lift = 0;
        }
        if (s->start) {
            s->active = 1;
            s->reported = s->pending = s->start = 0;
        }
        if (s->active && !s->reported) {
            if (s->have_x && s->have_y) { /* down waits until the contact has a position */
                n = push(out, max, n, TOUCH_DOWN, i, s->x, s->y);
                s->reported = 1;
                mark_sent(s, now);
            }
        } else if (s->active && s->changed) {
            if (s->x == s->sent_x && s->y == s->sent_y) {
                s->pending = 0;
            } else if (now - s->sent_ms >= t->throttle_ms) {
                n = push(out, max, n, TOUCH_MOVE, i, s->x, s->y);
                mark_sent(s, now);
            } else {
                s->pending = 1;
            }
        }
        s->changed = 0;
    }
    return n;
}

static void set_x(struct touch_slot *s, int32_t v) {
    s->changed |= !s->have_x || s->x != v;
    s->x = v;
    s->have_x = 1;
}

static void set_y(struct touch_slot *s, int32_t v) {
    s->changed |= !s->have_y || s->y != v;
    s->y = v;
    s->have_y = 1;
}

/* Protocol A: match the finished report's contacts to the pointers of the
 * last one, by tracking id when the driver sends one, else by position in
 * the report. Unmatched pointers lift; new contacts take the lowest free slot. */
static void match_contacts(struct touch_state *t) {
    int seen[TOUCH_MAX_POINTERS] = {0};
    for (int k = 0; k < t->nframe; k++) {
        const struct touch_contact *c = &t->frame[k];
        int64_t key = c->have_tid ? c->tid : -1 - (int64_t)k;
        int slot = -1;
        for (int i = 0; i < t->nslots && slot < 0; i++)
            if (t->s[i].active && !seen[i] && t->s[i].key == key) slot = i;
        if (slot < 0) {
            for (int i = 0; i < t->nslots && slot < 0; i++)
                if (!t->s[i].active && !t->s[i].start && !seen[i]) slot = i;
            if (slot < 0) continue; /* more contacts than pointers */
            t->s[slot].start = 1;
            t->s[slot].key = key;
            t->s[slot].have_x = t->s[slot].have_y = 0;
        }
        seen[slot] = 1;
        set_x(&t->s[slot], c->x);
        set_y(&t->s[slot], c->y);
    }
    for (int i = 0; i < t->nslots; i++)
        if (t->s[i].active && !seen[i]) t->s[i].lift = 1;
    t->nframe = 0;
}

static void end_contact(struct touch_state *t) {
    if (t->pend.have_x && t->pend.have_y && t->nframe < TOUCH_MAX_POINTERS) t->frame[t->nframe++] = t->pend;
    memset(&t->pend, 0, sizeof t->pend);
}

int touch_feed(struct touch_state *t, uint16_t type, uint16_t code, int32_t value, int64_t now_ms,
               struct touch_out *out, int max) {
    if (type == EV_SYN && code == SYN_REPORT) {
        if (t->proto == TOUCH_MT_A) {
            end_contact(t); /* a lone contact some drivers send without SYN_MT_REPORT */
            match_contacts(t);
        }
        return report(t, now_ms, out, max);
    }
    switch (t->proto) {
    case TOUCH_MT_B: {
        if (type != EV_ABS) break;
        if (code == ABS_MT_SLOT) {
            t->cur = value >= 0 && value < t->nslots ? value : -1;
            break;
        }
        if (t->cur < 0) break;
        struct touch_slot *s = &t->s[t->cur];
        if (code == ABS_MT_TRACKING_ID) {
            if (value < 0) {
                s->lift = s->active;
                s->start = 0;
            } else if (!(s->active && !s->lift && s->key == value)) {
                /* A new contact; a new id on a live slot replaces its contact. The
                 * same id again is not news: some drivers resend it every report. */
                s->lift = s->active;
                s->start = 1;
                s->key = value;
            }
        } else if (code == ABS_MT_POSITION_X) {
            set_x(s, value);
        } else if (code == ABS_MT_POSITION_Y) {
            set_y(s, value);
        }
        break;
    }
    case TOUCH_MT_A:
        if (type == EV_SYN && code == SYN_MT_REPORT) {
            end_contact(t);
        } else if (type == EV_ABS && code == ABS_MT_POSITION_X) {
            t->pend.x = value;
            t->pend.have_x = 1;
        } else if (type == EV_ABS && code == ABS_MT_POSITION_Y) {
            t->pend.y = value;
            t->pend.have_y = 1;
        } else if (type == EV_ABS && code == ABS_MT_TRACKING_ID) {
            t->pend.tid = value;
            t->pend.have_tid = value >= 0;
        }
        break;
    case TOUCH_SINGLE: {
        struct touch_slot *s = &t->s[0];
        if (type == EV_ABS && code == ABS_X) set_x(s, value);
        else if (type == EV_ABS && code == ABS_Y) set_y(s, value);
        else if (type == EV_KEY && code == BTN_TOUCH && value == 1 && !s->active) s->start = 1;
        else if (type == EV_KEY && code == BTN_TOUCH && value == 0) {
            s->lift = s->active;
            s->start = 0;
        }
        break;
    }
    }
    return 0;
}

int touch_flush(struct touch_state *t, int64_t now_ms, struct touch_out *out, int max) {
    int n = 0;
    for (int i = 0; i < t->nslots; i++) {
        struct touch_slot *s = &t->s[i];
        if (!s->pending || now_ms - s->sent_ms < t->throttle_ms) continue;
        n = push(out, max, n, TOUCH_MOVE, i, s->x, s->y);
        mark_sent(s, now_ms);
    }
    return n;
}

int touch_release_all(struct touch_state *t, struct touch_out *out, int max) {
    int n = 0;
    for (int i = 0; i < t->nslots; i++) {
        struct touch_slot *s = &t->s[i];
        if (s->active && s->reported) n = push(out, max, n, TOUCH_UP, i, s->x, s->y);
        s->active = s->reported = s->pending = s->start = s->lift = 0;
    }
    return n;
}

int64_t touch_next_due(const struct touch_state *t, int64_t now_ms) {
    int64_t best = -1;
    for (int i = 0; i < t->nslots; i++) {
        const struct touch_slot *s = &t->s[i];
        if (!s->pending) continue;
        int64_t left = s->sent_ms + t->throttle_ms - now_ms;
        if (left < 0) left = 0;
        if (best < 0 || left < best) best = left;
    }
    return best;
}

int touch_to_logical(struct touch_axis ax, struct touch_axis ay, int rotation, uint32_t native_w, uint32_t native_h,
                     int32_t rx, int32_t ry, uint32_t *lx, uint32_t *ly) {
    int nx = fbd_scale(rx, ax.min, ax.max, native_w);
    int ny = fbd_scale(ry, ay.min, ay.max, native_h);
    if (nx < 0 || ny < 0) return -1;
    render_unmap(rotation, native_w, native_h, (uint32_t)nx, (uint32_t)ny, lx, ly);
    return 0;
}
