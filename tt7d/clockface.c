/* ABOUTME: Builds the fallback clock face: English date and time text in the local TZ, a centred layout,
 * ABOUTME: and the render (flat dark background, large light time, dimmer date, small status line). */
#include "clockface.h"

#include <stdio.h>
#include <string.h>

/* Exact in RGB565 (R and B multiples of 8, G of 4), so the panel shows this
 * blue-grey rather than a truncated, green-tinted one. */
const uint8_t clockface_background[3] = {16, 20, 24};
static const uint8_t time_rgb[3] = {236, 239, 244};
static const uint8_t date_rgb[3] = {148, 158, 173};
static const uint8_t headline_rgb[3] = {198, 206, 217};
static const uint8_t status_rgb[3] = {96, 106, 121};

/* Sizes in pixels per em, for the 1280x800 logical screen. */
#define TIME_PX 300.0f
#define SUFFIX_PX 64.0f
#define DATE_PX 46.0f
#define HEADLINE_PX 84.0f
#define STATUS_PX 24.0f
#define TIME_DATE_GAP 60 /* px between the digits' foot and the date's cap height */
#define SUFFIX_GAP 22    /* px between the time and AM/PM */
#define STATUS_BOTTOM 44 /* status baseline, px above the bottom edge */
#define CENTRE_Y 0.46    /* optical centre: a little above the middle */

static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const months[] = {"January", "February", "March",     "April",   "May",      "June",
                                     "July",    "August",   "September", "October", "November", "December"};

#define MIDDOT "\xc2\xb7"
#define ELLIPSIS "\xe2\x80\xa6"

void clockface_text(time_t now, int synced, int hour12, const char *ip, struct clockface_text *t) {
    memset(t, 0, sizeof *t);
    snprintf(t->status, sizeof t->status, "waiting for server " MIDDOT " %s", ip && *ip ? ip : "no network yet");
    struct tm tm;
    if (!synced || !localtime_r(&now, &tm)) {
        snprintf(t->headline, sizeof t->headline, "Setting clock" ELLIPSIS);
        return;
    }
    t->show_time = 1;
    if (hour12) {
        int h = tm.tm_hour % 12;
        snprintf(t->time, sizeof t->time, "%d:%02d", h ? h : 12, tm.tm_min);
        snprintf(t->suffix, sizeof t->suffix, "%s", tm.tm_hour < 12 ? "AM" : "PM");
    } else {
        snprintf(t->time, sizeof t->time, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
    snprintf(t->date, sizeof t->date, "%s, %d %s %d", days[tm.tm_wday % 7], tm.tm_mday, months[tm.tm_mon % 12],
             tm.tm_year + 1900);
}

/* Place a line with its pen at (pen_x, baseline) and record its screen ink box. */
static void place(struct clockface_line *l, const struct font *f, float px, const char *s, int pen_x, int baseline) {
    struct text_box b;
    font_measure(f, px, s, &b, NULL);
    *l = (struct clockface_line){px, pen_x, baseline, {b.x0 + pen_x, b.y0 + baseline, b.x1 + pen_x, b.y1 + baseline}};
}

/* Place a line horizontally centred on its ink. */
static void place_centred(struct clockface_line *l, const struct font *f, float px, const char *s, int w,
                          int baseline) {
    struct text_box b;
    font_measure(f, px, s, &b, NULL);
    place(l, f, px, s, (w - (b.x1 - b.x0)) / 2 - b.x0, baseline);
}

/* Height above the baseline of a reference glyph, so lines sit the same way
 * whatever their words (no jump for a date with or without descenders). */
static int cap_height(const struct font *f, float px, const char *ref) {
    struct text_box b;
    font_measure(f, px, ref, &b, NULL);
    return -b.y0;
}

void clockface_layout(const struct clockface_fonts *f, const struct clockface_text *t, int w, int h,
                      struct clockface_layout *out) {
    memset(out, 0, sizeof *out);
    place_centred(&out->status, f->text, STATUS_PX, t->status, w, h - STATUS_BOTTOM);
    int centre = (int)(h * CENTRE_Y);
    if (!t->show_time) {
        place_centred(&out->headline, f->time, HEADLINE_PX, t->headline, w,
                      centre + cap_height(f->time, HEADLINE_PX, "S") / 2);
        return;
    }

    /* Vertically: digits, gap, date caps, centred as one block on CENTRE_Y. */
    int digits = cap_height(f->time, TIME_PX, "0"), caps = cap_height(f->text, DATE_PX, "H");
    int top = centre - (digits + TIME_DATE_GAP + caps) / 2;
    int time_base = top + digits, date_base = time_base + TIME_DATE_GAP + caps;

    /* Horizontally: the time and its AM/PM suffix are centred together. */
    struct clockface_line time, suffix = {0};
    place(&time, f->time, TIME_PX, t->time, 0, time_base);
    struct text_box g = time.ink;
    if (t->suffix[0]) {
        place(&suffix, f->text, SUFFIX_PX, t->suffix, 0, time_base);
        int dx = g.x1 + SUFFIX_GAP - suffix.ink.x0;
        place(&suffix, f->text, SUFFIX_PX, t->suffix, dx, time_base);
        g.x1 = suffix.ink.x1;
    }
    int shift = (w - (g.x1 - g.x0)) / 2 - g.x0;
    place(&out->time, f->time, TIME_PX, t->time, time.pen_x + shift, time_base);
    if (t->suffix[0]) place(&out->suffix, f->text, SUFFIX_PX, t->suffix, suffix.pen_x + shift, time_base);
    out->time_group = (struct text_box){g.x0 + shift, g.y0, g.x1 + shift, g.y1};
    if (t->suffix[0] && out->suffix.ink.y0 < out->time_group.y0) out->time_group.y0 = out->suffix.ink.y0;
    place_centred(&out->date, f->text, DATE_PX, t->date, w, date_base);
}

static void draw(const struct clockface_line *l, const struct font *f, const char *s, const uint8_t rgb[3],
                 uint8_t *rgba, int w, int h) {
    if (l->px > 0 && s[0]) font_draw(f, l->px, s, l->pen_x, l->baseline, rgb, rgba, w, h);
}

void clockface_render(const struct clockface_fonts *f, const struct clockface_text *t, uint8_t *rgba, int w, int h) {
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        memcpy(rgba + i * 4, clockface_background, 3);
        rgba[i * 4 + 3] = 255;
    }
    struct clockface_layout l;
    clockface_layout(f, t, w, h, &l);
    draw(&l.time, f->time, t->time, time_rgb, rgba, w, h);
    draw(&l.suffix, f->text, t->suffix, date_rgb, rgba, w, h);
    draw(&l.date, f->text, t->date, date_rgb, rgba, w, h);
    draw(&l.headline, f->time, t->headline, headline_rgb, rgba, w, h);
    draw(&l.status, f->text, t->status, status_rgb, rgba, w, h);
}
