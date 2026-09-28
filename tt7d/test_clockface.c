/* ABOUTME: Unit and render tests for the fallback clock face: local-time text under the default POSIX TZ
 * ABOUTME: (12/24 h, both DST changes), centred layout, and a real render with the embedded fonts, saved as PNGs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "clockface.h"
#include "font.h"
#include "lodepng.h"
#include "test_common.h"
#include "timesync.h"

#define W 1280
#define H 800
#define OUT_DIR "build/host" /* tests run from the repo root, like FIXTURE */

static void text_is(time_t t, int hour12, const char *time, const char *suffix, const char *date) {
    struct clockface_text c;
    clockface_text(t, 1, hour12, "10.0.0.2", &c);
    CHECK(c.show_time && !strcmp(c.time, time) && !strcmp(c.suffix, suffix) && !strcmp(c.date, date),
          "%lld (%s): got '%s' '%s' '%s', want '%s' '%s' '%s'", (long long)t, hour12 ? "12h" : "24h", c.time, c.suffix,
          c.date, time, suffix, date);
    CHECK(!strcmp(c.status, "waiting for server \xc2\xb7 10.0.0.2") && c.headline[0] == 0, "status '%s'", c.status);
}

static void local_time_text(void) {
    timesync_tz_apply(TIMESYNC_DEFAULT_TZ);
    text_is(1790622300, 0, "14:05", "", "Monday, 28 September 2026"); /* 19:05Z, CDT = UTC-5 */
    text_is(1790622300, 1, "2:05", "PM", "Monday, 28 September 2026");
    text_is(1790571600, 0, "00:00", "", "Monday, 28 September 2026");
    text_is(1790571600, 1, "12:00", "AM", "Monday, 28 September 2026");
    text_is(1790614800, 1, "12:00", "PM", "Monday, 28 September 2026");
    text_is(1767247140, 0, "23:59", "", "Wednesday, 31 December 2025"); /* CST = UTC-6 */
    /* DST ends 2026-11-01 02:00 CDT: 1:30 happens twice, then 2:30 CST. */
    text_is(1793514600, 0, "01:30", "", "Sunday, 1 November 2026");
    text_is(1793518200, 0, "01:30", "", "Sunday, 1 November 2026");
    text_is(1793521800, 1, "2:30", "AM", "Sunday, 1 November 2026");
    /* DST starts 2026-03-08 02:00 CST: 1:59 is followed by 3:00. */
    text_is(1772956740, 0, "01:59", "", "Sunday, 8 March 2026");
    text_is(1772956800, 1, "3:00", "AM", "Sunday, 8 March 2026");
    timesync_tz_apply("UTC0");
    text_is(1790622300, 0, "19:05", "", "Monday, 28 September 2026");
    timesync_tz_apply(TIMESYNC_DEFAULT_TZ);
}

static void unsynced_text(void) {
    struct clockface_text c;
    clockface_text(1790622300, 0, 0, "192.168.23.197", &c);
    CHECK(!c.show_time && !c.time[0] && !c.suffix[0] && !c.date[0], "no time or date before sync: '%s' '%s'", c.time,
          c.date);
    CHECK(!strcmp(c.headline, "Setting clock\xe2\x80\xa6"), "headline '%s'", c.headline);
    CHECK(!strcmp(c.status, "waiting for server \xc2\xb7 192.168.23.197"), "status '%s'", c.status);
    clockface_text(1790622300, 0, 0, NULL, &c);
    CHECK(!strcmp(c.status, "waiting for server \xc2\xb7 no network yet"), "no ip: '%s'", c.status);
}

static int inside(struct text_box b) { return b.x0 >= 0 && b.y0 >= 0 && b.x1 <= W && b.y1 <= H && b.x1 > b.x0; }

static void layout_checks(const struct clockface_fonts *f) {
    struct clockface_text c;
    struct clockface_layout l;
    for (int hour12 = 0; hour12 <= 1; hour12++) {
        clockface_text(1790622300, 1, hour12, "192.168.23.197", &c);
        clockface_layout(f, &c, W, H, &l);
        int left = l.time_group.x0, right = W - l.time_group.x1;
        CHECK(abs(left - right) <= 2, "%s time group not centred: %d px left, %d px right", hour12 ? "12h" : "24h",
              left, right);
        CHECK(l.time.ink.y1 - l.time.ink.y0 > 180, "the time is large: %d px of ink", l.time.ink.y1 - l.time.ink.y0);
        CHECK(inside(l.time_group) && inside(l.date.ink) && inside(l.status.ink), "everything on screen");
        CHECK(l.time.ink.y1 < l.date.ink.y0 && l.date.ink.y1 < l.status.ink.y0, "time, then date, then status");
        CHECK(abs((l.date.ink.x0 + l.date.ink.x1) / 2 - W / 2) <= 2, "date centred");
        CHECK(abs((l.status.ink.x0 + l.status.ink.x1) / 2 - W / 2) <= 2, "status centred");
        CHECK(hour12 ? l.suffix.px > 0 : l.suffix.px == 0, "suffix only in 12-hour mode");
    }
    clockface_text(1790622300, 0, 0, "192.168.23.197", &c);
    clockface_layout(f, &c, W, H, &l);
    CHECK(l.time.px == 0 && l.date.px == 0 && l.headline.px > 0, "unsynced: only the headline and status");
    CHECK(abs((l.headline.ink.x0 + l.headline.ink.x1) / 2 - W / 2) <= 2 && inside(l.headline.ink), "headline centred");
}

static int luma(const uint8_t *p) { return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000; }

static int in_box(int x, int y, struct text_box b) { return x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1; }

static void save_png(const uint8_t *rgba, const char *name) {
    char path[128];
    snprintf(path, sizeof path, "%s/%s", OUT_DIR, name);
    unsigned char *png = NULL;
    size_t n = 0;
    unsigned err = lodepng_encode32(&png, &n, rgba, W, H);
    CHECK(err == 0, "encoding %s: %s", path, lodepng_error_text(err));
    FILE *f = err ? NULL : fopen(path, "wb");
    CHECK(err || (f && fwrite(png, 1, n, f) == n), "writing %s", path);
    if (f && fclose(f) == 0) printf("  png  %s\n", path);
    free(png);
}

/* Render into a buffer with guard bytes on both sides, then check it. */
static void render_checks(const struct clockface_fonts *f, time_t t, int synced, int hour12, const char *png) {
    const size_t guard = 4096, len = (size_t)W * H * 4;
    uint8_t *mem = malloc(len + 2 * guard), *rgba = mem + guard;
    memset(mem, 0xA5, len + 2 * guard);
    struct clockface_text c;
    struct clockface_layout l;
    clockface_text(t, synced, hour12, "192.168.23.197", &c);
    clockface_layout(f, &c, W, H, &l);
    clockface_render(f, &c, rgba, W, H);

    int guards_ok = 1;
    for (size_t i = 0; i < guard; i++) guards_ok &= mem[i] == 0xA5 && rgba[len + i] == 0xA5;
    CHECK(guards_ok, "%s: wrote outside the buffer", png);

    const struct clockface_line *lines[] = {&l.time, &l.suffix, &l.date, &l.headline, &l.status};
    int bg = luma(clockface_background), max_text = 0, stray = 0, opaque = 1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t *p = rgba + ((size_t)y * W + x) * 4;
            opaque &= p[3] == 255;
            int text = 0;
            for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
                text |= lines[i]->px > 0 && in_box(x, y, lines[i]->ink);
            if (text) {
                if (luma(p) > max_text) max_text = luma(p);
            } else if (memcmp(p, clockface_background, 3) != 0) {
                stray++;
            }
        }
    CHECK(opaque, "%s: every pixel opaque", png);
    CHECK(stray == 0, "%s: %d pixels outside the text boxes differ from the background", png, stray);
    CHECK(max_text > bg + 150, "%s: text luma %d vs background %d: not bright enough", png, max_text, bg);
    CHECK(bg < 40, "%s: the background is dark (luma %d)", png, bg);
    save_png(rgba, png);
    free(mem);
}

int main(void) {
    local_time_text();
    unsynced_text();

    const struct asset *ta = asset_find(ASSET_FONT_TIME), *xa = asset_find(ASSET_FONT_TEXT);
    CHECK(ta && xa, "both fonts are embedded");
    if (!ta || !xa) return test_finish("test_clockface");
    struct font *tf = font_load(ta->data, ta->len), *xf = font_load(xa->data, xa->len);
    CHECK(tf && xf, "both fonts load");
    CHECK(font_load((const unsigned char *)"not a font at all", 17) == NULL, "junk is refused");
    if (!tf || !xf) return test_finish("test_clockface");
    struct clockface_fonts f = {tf, xf};

    struct text_box b;
    int adv;
    font_measure(xf, 40, "", &b, &adv);
    CHECK(b.x0 == 0 && b.x1 == 0 && adv == 0, "empty string");
    font_measure(xf, 40, "\xff\xfe", &b, &adv);
    CHECK(adv > 0, "malformed UTF-8 still advances (missing-glyph box)");

    const uint8_t *bg = clockface_background;
    CHECK(bg[0] % 8 == 0 && bg[1] % 4 == 0 && bg[2] % 8 == 0, "the background survives RGB565 unchanged");
    layout_checks(&f);
    timesync_tz_apply(TIMESYNC_DEFAULT_TZ);
    render_checks(&f, 1790622300, 1, 0, "clockface-24h.png");
    render_checks(&f, 1790622300, 1, 1, "clockface-12h.png");
    render_checks(&f, 1790622300, 0, 0, "clockface-unsynced.png");
    font_free(tf);
    font_free(xf);
    return test_finish("test_clockface");
}
