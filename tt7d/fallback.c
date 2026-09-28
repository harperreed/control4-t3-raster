/* ABOUTME: The fallback state machine: tracks the newest frame or heartbeat and decides when tt7d
 * ABOUTME: shows its own clock screen instead of the server's frame (rules in fallback.h). */
#include "fallback.h"

#include <stddef.h>

static void enter(struct fallback *f, enum fallback_reason reason, int64_t now_ms) {
    f->active = 1;
    f->reason = reason;
    f->since_ms = now_ms;
}

void fallback_init(struct fallback *f, unsigned timeout_s, int64_t now_ms) {
    *f = (struct fallback){.timeout_s = timeout_s, .last_activity_ms = now_ms};
    if (timeout_s) enter(f, FALLBACK_NO_FRAME, now_ms);
}

void fallback_frame(struct fallback *f, int64_t now_ms) {
    f->last_activity_ms = now_ms;
    f->active = 0;
    f->reason = FALLBACK_NONE;
}

void fallback_heartbeat(struct fallback *f, int64_t now_ms) { f->last_activity_ms = now_ms; }

int64_t fallback_ms_until_timeout(const struct fallback *f, int64_t now_ms) {
    if (!f->timeout_s || f->active) return -1;
    int64_t left = f->last_activity_ms + (int64_t)f->timeout_s * 1000 - now_ms;
    return left > 0 ? left : 0;
}

int fallback_update(struct fallback *f, int64_t now_ms) {
    if (fallback_ms_until_timeout(f, now_ms) != 0) return 0;
    enter(f, FALLBACK_TIMEOUT, now_ms);
    return 1;
}

void fallback_json(const struct fallback *f, int64_t now_ms, const struct timespec *now_wall, int with_since,
                   struct sbuf *sb) {
    sb_printf(sb, "\"fallback\":{\"active\":%s,\"reason\":", f->active ? "true" : "false");
    sb_json_str(sb, fallback_reason_name(f->reason));
    sb_printf(sb, ",\"timeout_s\":%u", f->timeout_s);
    if (with_since) {
        sb_puts(sb, ",\"since\":");
        if (f->active) {
            int64_t ago_ns = (now_ms - f->since_ms) * 1000000;
            int64_t ns = (int64_t)now_wall->tv_sec * 1000000000 + now_wall->tv_nsec - ago_ns;
            struct timespec t = {.tv_sec = (time_t)(ns / 1000000000), .tv_nsec = (long)(ns % 1000000000)};
            sb_json_time(sb, &t);
        } else {
            sb_puts(sb, "null");
        }
    }
    sb_puts(sb, "}");
}

const char *fallback_reason_name(enum fallback_reason r) {
    switch (r) {
    case FALLBACK_NO_FRAME: return "no_frame_since_boot";
    case FALLBACK_TIMEOUT: return "server_timeout";
    default: return NULL;
    }
}
