/* ABOUTME: The fallback clock face (SPEC 41.1): the words to show for a moment in time, their layout
 * ABOUTME: on the 1280x800 logical screen, and rendering them into an RGBA buffer with font.c. */
#ifndef TT7D_CLOCKFACE_H
#define TT7D_CLOCKFACE_H

#include <stdint.h>
#include <time.h>

#include "font.h"

/* What the face says. With the clock unsynchronized there is no time and
 * no date at all, only the "Setting clock…" headline (never a wrong time). */
struct clockface_text {
    int show_time;
    char time[8];      /* "14:05" (24 h) or "2:05" (12 h); "" without a time */
    char suffix[4];    /* "AM" or "PM" in 12-hour mode, else "" */
    char date[48];     /* "Sunday, 28 September 2026"; "" without a time */
    char headline[32]; /* "Setting clock…" without a time, else "" */
    char status[96];   /* "waiting for server · 192.168.1.20" */
};

/* Fill t for wall time `now` in the current TZ (timesync_tz_apply).
 * hour12 picks 12-hour time. ip may be NULL or "" when there is none. */
void clockface_text(time_t now, int synced, int hour12, const char *ip, struct clockface_text *t);

struct clockface_fonts {
    const struct font *time; /* the large time (Inter Display Light) */
    const struct font *text; /* date, suffix, headline, status (Inter Regular) */
};

/* One line of text placed on the screen: size, pen start, baseline, and its
 * ink box in screen pixels. Unused lines have px 0. */
struct clockface_line {
    float px;
    int pen_x, baseline;
    struct text_box ink;
};

struct clockface_layout {
    struct clockface_line time, suffix, date, headline, status;
    struct text_box time_group; /* the time with its AM/PM suffix: centred as one */
};

void clockface_layout(const struct clockface_fonts *f, const struct clockface_text *t, int w, int h,
                      struct clockface_layout *out);

/* Paint the whole w x h RGBA buffer: background, then the text. */
void clockface_render(const struct clockface_fonts *f, const struct clockface_text *t, uint8_t *rgba, int w, int h);

/* The background colour, for tests and anyone matching the face. */
extern const uint8_t clockface_background[3];

#endif
