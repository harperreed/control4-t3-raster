/* ABOUTME: What presence does to the display: wake a blanked display when someone arrives, and optionally
 * ABOUTME: blank it again after a quiet spell. Pure decisions over (presence, display state, time). */
#ifndef TT7D_PRESENCE_H
#define TT7D_PRESENCE_H

#include <stdint.h>

struct presence_policy {
    int wake;         /* presence_wake: wake a blanked display on arrival */
    int idle_blank_s; /* blank a display that presence woke after this long without presence; 0 = never */
};

struct presence_ctl {
    int present;
    int woke; /* presence woke the display and nothing has blanked it since */
    int64_t absent_since_ms;
};

enum presence_action { PRESENCE_NOTHING, PRESENCE_WAKE, PRESENCE_BLANK };

void presence_ctl_init(struct presence_ctl *s);

/* Call on every presence report and whenever presence_next_ms says. present
 * is the detector's current presence; display_blank is 1 if the backlight is
 * dark now. Only a display that presence itself woke is ever blanked again,
 * and only while presence_wake stays on: a display someone woke by hand is
 * left alone. */
enum presence_action presence_update(struct presence_ctl *s, const struct presence_policy *p, int present,
                                     int display_blank, int64_t now_ms);

/* Milliseconds until the idle blank falls due (0 if due now), or -1 if none is pending. */
int64_t presence_next_ms(const struct presence_ctl *s, const struct presence_policy *p, int64_t now_ms);

#endif
