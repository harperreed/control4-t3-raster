/* ABOUTME: Presence -> display decisions: wake on arrival if the backlight is dark, and blank again after
 * ABOUTME: presence_idle_blank_s without presence, but only a display that presence itself woke. */
#include "presence.h"

#include <string.h>

void presence_ctl_init(struct presence_ctl *s) { memset(s, 0, sizeof *s); }

enum presence_action presence_update(struct presence_ctl *s, const struct presence_policy *p, int present,
                                     int display_blank, int64_t now_ms) {
    if (!p->wake || (s->woke && display_blank && !s->present)) s->woke = 0; /* not ours to blank any more */
    if (present && !s->present) {
        s->present = 1;
        if (p->wake && display_blank) {
            s->woke = 1;
            return PRESENCE_WAKE;
        }
        return PRESENCE_NOTHING;
    }
    if (!present && s->present) {
        s->present = 0;
        s->absent_since_ms = now_ms;
        return PRESENCE_NOTHING;
    }
    if (!present && presence_next_ms(s, p, now_ms) == 0) {
        s->woke = 0;
        return PRESENCE_BLANK;
    }
    return PRESENCE_NOTHING;
}

int64_t presence_next_ms(const struct presence_ctl *s, const struct presence_policy *p, int64_t now_ms) {
    if (!s->woke || s->present || !p->wake || p->idle_blank_s <= 0) return -1;
    int64_t left = s->absent_since_ms + (int64_t)p->idle_blank_s * 1000 - now_ms;
    return left > 0 ? left : 0;
}
