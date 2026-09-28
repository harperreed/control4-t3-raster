/* ABOUTME: The fallback state machine (SPEC 41.1): show the server's frame, or tt7d's own clock screen.
 * ABOUTME: Pure: every call takes the monotonic time in ms, so tests drive it without sleeping. */
#ifndef TT7D_FALLBACK_H
#define TT7D_FALLBACK_H

#include <stdint.h>
#include <time.h>

#include "json.h"

enum fallback_reason {
    FALLBACK_NONE,     /* not in fallback: the server's frame shows */
    FALLBACK_NO_FRAME, /* no frame since boot (a restored last-frame.png counts as one) */
    FALLBACK_TIMEOUT,  /* the newest frame or heartbeat is timeout_s old */
};

/* Rules:
 * - Boot: in fallback until the first accepted frame, unless timeout_s is 0.
 * - An accepted frame (new or a duplicate of the current one) leaves the
 *   fallback at once and restarts the timer.
 * - A heartbeat restarts the timer but never leaves the fallback: there is
 *   no fresh server frame to show, and the old one may be stale. So a
 *   heartbeat keeps a server frame up, and changes nothing while the clock
 *   shows except when the next timeout could trip.
 * - When the newest frame or heartbeat is timeout_s old, fall back.
 * - timeout_s 0 disables the fallback entirely, boot included.
 * Whether the clock is synchronized only changes what the fallback screen
 * shows (clockface.h), never whether it shows. */
struct fallback {
    unsigned timeout_s;
    int64_t last_activity_ms; /* the newest frame or heartbeat */
    int active;
    enum fallback_reason reason;
    int64_t since_ms; /* when the fallback became active */
};

void fallback_init(struct fallback *f, unsigned timeout_s, int64_t now_ms);
void fallback_frame(struct fallback *f, int64_t now_ms);
void fallback_heartbeat(struct fallback *f, int64_t now_ms);

/* Apply the timeout. Returns 1 if this call entered the fallback, else 0. */
int fallback_update(struct fallback *f, int64_t now_ms);

/* Milliseconds until fallback_update() could enter the fallback (0 if
 * overdue), or -1 when it cannot (disabled, or already active). */
int64_t fallback_ms_until_timeout(const struct fallback *f, int64_t now_ms);

/* "no_frame_since_boot", "server_timeout", or NULL for FALLBACK_NONE. */
const char *fallback_reason_name(enum fallback_reason r);

/* The "fallback" member for /api/v1/state and the MQTT state (key included,
 * no comma): {"active", "reason", "timeout_s", "since"}. `since` is when the
 * fallback began: wall time now_wall minus the monotonic time since then, so
 * a clock set after it began still gives the right instant; null when not
 * active. with_since 0 leaves it out, for the MQTT change signature, where a
 * `since` that moves by a millisecond must not count as a change. */
void fallback_json(const struct fallback *f, int64_t now_ms, const struct timespec *now_wall, int with_since,
                   struct sbuf *sb);

#endif
