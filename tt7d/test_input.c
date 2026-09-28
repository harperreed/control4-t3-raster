/* ABOUTME: Host unit tests for touch.c and input.c: the evdev touch state machine (MT protocol A, B, single touch),
 * ABOUTME: move throttling, native->logical mapping for all 4 rotations (shared with render.c), and device classification. */
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "input.h"
#include "render.h"
#include "test_common.h"
#include "touch.h"

#define THROTTLE 16

/* One evdev record as the tests write them: type, code, value. */
struct ev {
    uint16_t type, code;
    int32_t value;
};

#define SYN {EV_SYN, SYN_REPORT, 0}
#define MT_SYN {EV_SYN, SYN_MT_REPORT, 0}

/* Feed a sequence, all at time `now`, and collect what comes out. */
static int feed(struct touch_state *t, const struct ev *evs, size_t n, int64_t now, struct touch_out *out, int max) {
    int got = 0;
    for (size_t i = 0; i < n; i++) got += touch_feed(t, evs[i].type, evs[i].code, evs[i].value, now, out + got, max - got);
    return got;
}

static int is(const struct touch_out *o, enum touch_action a, int pointer, int32_t x, int32_t y) {
    return o->action == a && o->pointer == pointer && o->x == x && o->y == y;
}

#define FEED(t, now, ...)                                                                \
    do {                                                                                 \
        const struct ev seq_[] = {__VA_ARGS__};                                          \
        n = feed(t, seq_, sizeof seq_ / sizeof seq_[0], now, out, (int)(sizeof out / sizeof out[0])); \
    } while (0)

/* Protocol B, as the TT7's gslX680 advertises it (ABS_MT_SLOT 0..10,
 * TRACKING_ID, POSITION_X/Y): two fingers, one lifts, the other moves on. */
static void test_protocol_b(void) {
    struct touch_state t;
    struct touch_out out[64];
    int n;
    touch_init(&t, TOUCH_MT_B, 11, 0, THROTTLE);

    FEED(&t, 1000, {EV_ABS, ABS_MT_SLOT, 0}, {EV_ABS, ABS_MT_TRACKING_ID, 45}, {EV_ABS, ABS_MT_POSITION_X, 100},
         {EV_ABS, ABS_MT_POSITION_Y, 200}, {EV_ABS, ABS_MT_TOUCH_MAJOR, 10}, SYN);
    CHECK(n == 1 && is(&out[0], TOUCH_DOWN, 0, 100, 200), "first finger down (n=%d)", n);

    /* Second finger in slot 1; the first moves in the same report. */
    FEED(&t, 1020, {EV_ABS, ABS_MT_POSITION_X, 110}, {EV_ABS, ABS_MT_SLOT, 1}, {EV_ABS, ABS_MT_TRACKING_ID, 46},
         {EV_ABS, ABS_MT_POSITION_X, 500}, {EV_ABS, ABS_MT_POSITION_Y, 600}, SYN);
    CHECK(n == 2, "two outputs, got %d", n);
    CHECK(n == 2 && is(&out[0], TOUCH_MOVE, 0, 110, 200), "slot 0 moved (x kept y)");
    CHECK(n == 2 && is(&out[1], TOUCH_DOWN, 1, 500, 600), "slot 1 down");

    /* The current slot stays 1 across reports: a bare Y update moves slot 1. */
    FEED(&t, 1040, {EV_ABS, ABS_MT_POSITION_Y, 610}, SYN);
    CHECK(n == 1 && is(&out[0], TOUCH_MOVE, 1, 500, 610), "slot 1 moved");

    /* Slot 0 lifts. */
    FEED(&t, 1060, {EV_ABS, ABS_MT_SLOT, 0}, {EV_ABS, ABS_MT_TRACKING_ID, -1}, SYN);
    CHECK(n == 1 && is(&out[0], TOUCH_UP, 0, 110, 200), "slot 0 up at its last position");

    /* A new contact in slot 0 keeps the slot's last position until it sends a new one:
     * the kernel drops unchanged values, so an identical X never arrives. */
    FEED(&t, 1080, {EV_ABS, ABS_MT_TRACKING_ID, 47}, {EV_ABS, ABS_MT_POSITION_Y, 300}, SYN);
    CHECK(n == 1 && is(&out[0], TOUCH_DOWN, 0, 110, 300), "new contact in slot 0");

    FEED(&t, 1100, {EV_ABS, ABS_MT_TRACKING_ID, -1}, {EV_ABS, ABS_MT_SLOT, 1}, {EV_ABS, ABS_MT_TRACKING_ID, -1}, SYN);
    CHECK(n == 2 && is(&out[0], TOUCH_UP, 0, 110, 300) && is(&out[1], TOUCH_UP, 1, 500, 610), "both up");

    /* A driver that resends the same tracking id with every report: still one contact. */
    FEED(&t, 1150, {EV_ABS, ABS_MT_TRACKING_ID, 48}, {EV_ABS, ABS_MT_POSITION_X, 20}, SYN);
    CHECK(n == 1 && is(&out[0], TOUCH_DOWN, 1, 20, 610), "slot 1 down again (n=%d)", n);
    FEED(&t, 1170, {EV_ABS, ABS_MT_TRACKING_ID, 48}, {EV_ABS, ABS_MT_POSITION_X, 30}, SYN);
    CHECK(n == 1 && is(&out[0], TOUCH_MOVE, 1, 30, 610), "same id again is a move, not up + down (n=%d)", n);
    /* A different id on the live slot: the old contact ends, a new one starts. */
    FEED(&t, 1190, {EV_ABS, ABS_MT_TRACKING_ID, 49}, SYN);
    CHECK(n == 2 && is(&out[0], TOUCH_UP, 1, 30, 610) && is(&out[1], TOUCH_DOWN, 1, 30, 610), "id change (n=%d)", n);
    FEED(&t, 1195, {EV_ABS, ABS_MT_TRACKING_ID, -1}, SYN);
    CHECK(n == 1 && out[0].action == TOUCH_UP, "up");

    /* An out-of-range slot is ignored rather than written past the array. */
    FEED(&t, 1200, {EV_ABS, ABS_MT_SLOT, 99}, {EV_ABS, ABS_MT_TRACKING_ID, 5}, {EV_ABS, ABS_MT_POSITION_X, 1}, SYN);
    CHECK(n == 0, "slot 99 ignored (n=%d)", n);

    /* A contact that lifts before it ever had a position never went down: no up either. */
    touch_init(&t, TOUCH_MT_B, 11, 0, THROTTLE);
    FEED(&t, 2000, {EV_ABS, ABS_MT_TRACKING_ID, 1}, SYN);
    CHECK(n == 0, "no position yet: no down");
    FEED(&t, 2010, {EV_ABS, ABS_MT_TRACKING_ID, -1}, SYN);
    CHECK(n == 0, "no up for a contact that never went down");

    /* BTN_TOUCH and ABS_X/Y (the kernel's pointer emulation) are ignored under protocol B. */
    FEED(&t, 2020, {EV_ABS, ABS_MT_TRACKING_ID, 2}, {EV_ABS, ABS_MT_POSITION_X, 5}, {EV_ABS, ABS_MT_POSITION_Y, 6},
         {EV_KEY, BTN_TOUCH, 1}, {EV_ABS, ABS_X, 999}, {EV_ABS, ABS_Y, 999}, SYN);
    CHECK(n == 1 && is(&out[0], TOUCH_DOWN, 0, 5, 6), "emulated pointer ignored");
}

/* Protocol A: anonymous contacts, each closed by SYN_MT_REPORT, the whole
 * set re-sent on every SYN_REPORT; an empty report means every finger is up. */
static void test_protocol_a(void) {
    struct touch_state t;
    struct touch_out out[64];
    int n;
    touch_init(&t, TOUCH_MT_A, 0, 0, THROTTLE);

    FEED(&t, 1000, {EV_ABS, ABS_MT_POSITION_X, 10}, {EV_ABS, ABS_MT_POSITION_Y, 20}, MT_SYN, SYN);
    CHECK(n == 1 && is(&out[0], TOUCH_DOWN, 0, 10, 20), "one contact down");

    FEED(&t, 1020, {EV_ABS, ABS_MT_POSITION_X, 11}, {EV_ABS, ABS_MT_POSITION_Y, 20}, MT_SYN,
         {EV_ABS, ABS_MT_POSITION_X, 300}, {EV_ABS, ABS_MT_POSITION_Y, 400}, MT_SYN, SYN);
    CHECK(n == 2 && is(&out[0], TOUCH_MOVE, 0, 11, 20) && is(&out[1], TOUCH_DOWN, 1, 300, 400), "move + second down");

    /* Same positions again: nothing to report. */
    FEED(&t, 1040, {EV_ABS, ABS_MT_POSITION_X, 11}, {EV_ABS, ABS_MT_POSITION_Y, 20}, MT_SYN,
         {EV_ABS, ABS_MT_POSITION_X, 300}, {EV_ABS, ABS_MT_POSITION_Y, 400}, MT_SYN, SYN);
    CHECK(n == 0, "unchanged contacts are quiet (n=%d)", n);

    /* Everyone lifts: a lone SYN_MT_REPORT (or none) and SYN_REPORT. */
    FEED(&t, 1060, MT_SYN, SYN);
    CHECK(n == 2 && is(&out[0], TOUCH_UP, 0, 11, 20) && is(&out[1], TOUCH_UP, 1, 300, 400), "all up (n=%d)", n);

    /* With tracking ids, a contact keeps its pointer when an earlier one lifts. */
    FEED(&t, 2000, {EV_ABS, ABS_MT_TRACKING_ID, 7}, {EV_ABS, ABS_MT_POSITION_X, 1}, {EV_ABS, ABS_MT_POSITION_Y, 1},
         MT_SYN, {EV_ABS, ABS_MT_TRACKING_ID, 8}, {EV_ABS, ABS_MT_POSITION_X, 2}, {EV_ABS, ABS_MT_POSITION_Y, 2},
         MT_SYN, SYN);
    CHECK(n == 2 && is(&out[0], TOUCH_DOWN, 0, 1, 1) && is(&out[1], TOUCH_DOWN, 1, 2, 2), "ids 7, 8 down");
    FEED(&t, 2020, {EV_ABS, ABS_MT_TRACKING_ID, 8}, {EV_ABS, ABS_MT_POSITION_X, 3}, {EV_ABS, ABS_MT_POSITION_Y, 2},
         MT_SYN, SYN);
    CHECK(n == 2 && is(&out[0], TOUCH_UP, 0, 1, 1) && is(&out[1], TOUCH_MOVE, 1, 3, 2), "id 8 stays pointer 1 (n=%d)",
          n);

    /* A single contact with no SYN_MT_REPORT before SYN_REPORT still counts. */
    touch_init(&t, TOUCH_MT_A, 0, 0, THROTTLE);
    FEED(&t, 3000, {EV_ABS, ABS_MT_POSITION_X, 50}, {EV_ABS, ABS_MT_POSITION_Y, 60}, SYN);
    CHECK(n == 1 && is(&out[0], TOUCH_DOWN, 0, 50, 60), "contact without SYN_MT_REPORT");
}

static void test_single_touch(void) {
    struct touch_state t;
    struct touch_out out[64];
    int n;
    touch_init(&t, TOUCH_SINGLE, 0, 0, THROTTLE);
    FEED(&t, 1000, {EV_ABS, ABS_X, 40}, {EV_ABS, ABS_Y, 50}, SYN);
    CHECK(n == 0, "position without BTN_TOUCH is hover, not a touch");
    FEED(&t, 1010, {EV_KEY, BTN_TOUCH, 1}, SYN);
    CHECK(n == 1 && is(&out[0], TOUCH_DOWN, 0, 40, 50), "down at the known position");
    FEED(&t, 1030, {EV_ABS, ABS_X, 45}, SYN);
    CHECK(n == 1 && is(&out[0], TOUCH_MOVE, 0, 45, 50), "move");
    FEED(&t, 1050, {EV_ABS, ABS_Y, 55}, {EV_KEY, BTN_TOUCH, 0}, SYN);
    CHECK(n == 2 && is(&out[0], TOUCH_MOVE, 0, 45, 55) && is(&out[1], TOUCH_UP, 0, 45, 55), "final move, then up");
}

/* At most one move per THROTTLE ms per pointer; the held-back position goes
 * out when the interval passes (touch_flush) or right before the up. */
static void test_throttle(void) {
    struct touch_state t;
    struct touch_out out[64];
    int n;
    touch_init(&t, TOUCH_MT_B, 2, 0, THROTTLE);
    FEED(&t, 1000, {EV_ABS, ABS_MT_TRACKING_ID, 1}, {EV_ABS, ABS_MT_POSITION_X, 0}, {EV_ABS, ABS_MT_POSITION_Y, 0},
         SYN);
    CHECK(n == 1 && out[0].action == TOUCH_DOWN, "down");
    CHECK(touch_next_due(&t, 1000) < 0, "nothing held back");

    FEED(&t, 1005, {EV_ABS, ABS_MT_POSITION_X, 1}, SYN);
    CHECK(n == 0, "move 5 ms after the down is held back");
    CHECK(touch_next_due(&t, 1005) == 11, "due in 11 ms, got %lld", (long long)touch_next_due(&t, 1005));
    FEED(&t, 1010, {EV_ABS, ABS_MT_POSITION_X, 2}, SYN);
    CHECK(n == 0, "still held back");
    n = touch_flush(&t, 1015, out, 64);
    CHECK(n == 0, "not due at 15 ms");
    n = touch_flush(&t, 1016, out, 64);
    CHECK(n == 1 && is(&out[0], TOUCH_MOVE, 0, 2, 0), "latest position flushed at 16 ms");
    CHECK(touch_next_due(&t, 1016) < 0, "nothing held back after the flush");

    FEED(&t, 1020, {EV_ABS, ABS_MT_POSITION_X, 3}, SYN);
    CHECK(n == 0, "held back again");
    FEED(&t, 1021, {EV_ABS, ABS_MT_TRACKING_ID, -1}, SYN);
    CHECK(n == 2 && is(&out[0], TOUCH_MOVE, 0, 3, 0) && is(&out[1], TOUCH_UP, 0, 3, 0), "final move before up");

    /* A steady stream every 4 ms for 100 ms: about 100/16 moves, never two within 16 ms. */
    touch_init(&t, TOUCH_MT_B, 2, 0, THROTTLE);
    FEED(&t, 0, {EV_ABS, ABS_MT_TRACKING_ID, 1}, {EV_ABS, ABS_MT_POSITION_X, 0}, {EV_ABS, ABS_MT_POSITION_Y, 0}, SYN);
    int moves = 0;
    int64_t last = 0;
    for (int64_t now = 4; now <= 100; now += 4) {
        struct ev seq[] = {{EV_ABS, ABS_MT_POSITION_X, (int32_t)now}, SYN};
        n = feed(&t, seq, 2, now, out, 64);
        n += touch_flush(&t, now, out + n, 64 - n);
        for (int i = 0; i < n; i++) {
            CHECK(out[i].action == TOUCH_MOVE && now - last >= THROTTLE, "move at %lld after %lld", (long long)now,
                  (long long)last);
            last = now;
            moves++;
        }
    }
    CHECK(moves == 6, "6 moves in 100 ms, got %d", moves);
}

/* Raw -> native (scaled over the absinfo range) -> logical (render_unmap),
 * checked against the display's own render_map for every rotation. */
static void test_mapping(void) {
    const uint32_t W = 800, H = 1280; /* the TT7's native fb */
    struct touch_axis ax = {0, 800}, ay = {0, 1280};
    int rots[] = {0, 90, 180, 270};
    for (int r = 0; r < 4; r++) {
        int rot = rots[r];
        uint32_t lw, lh;
        render_logical_size(W, H, rot, &lw, &lh);
        for (int32_t rx = 0; rx <= 800; rx += 100)
            for (int32_t ry = 0; ry <= 1280; ry += 160) {
                uint32_t lx, ly, nx, ny;
                CHECK(touch_to_logical(ax, ay, rot, W, H, rx, ry, &lx, &ly) == 0, "maps");
                CHECK(lx < lw && ly < lh, "rot %d: (%u,%u) inside %ux%u", rot, lx, ly, lw, lh);
                render_map(rot, lw, lh, lx, ly, &nx, &ny);
                uint32_t want_x = (uint32_t)((int64_t)rx * (W - 1) / 800), want_y = (uint32_t)((int64_t)ry * (H - 1) / 1280);
                CHECK(nx == want_x && ny == want_y, "rot %d raw (%d,%d): logical (%u,%u) draws at native (%u,%u), want (%u,%u)",
                      rot, rx, ry, lx, ly, nx, ny, want_x, want_y);
            }
    }
    /* Rotation 90 in words: the fb's top-right corner is the logical top-left. */
    uint32_t lx, ly;
    touch_to_logical(ax, ay, 90, W, H, 800, 0, &lx, &ly);
    CHECK(lx == 0 && ly == 0, "rot 90: native top-right -> logical (0,0), got (%u,%u)", lx, ly);
    touch_to_logical(ax, ay, 270, W, H, 0, 1280, &lx, &ly);
    CHECK(lx == 0 && ly == 0, "rot 270: native bottom-left -> logical (0,0), got (%u,%u)", lx, ly);
    /* Out-of-range raw values clamp to the edge; an empty range is refused. */
    touch_to_logical(ax, ay, 0, W, H, -50, 5000, &lx, &ly);
    CHECK(lx == 0 && ly == H - 1, "clamped (%u,%u)", lx, ly);
    struct touch_axis bad = {10, 10};
    CHECK(touch_to_logical(bad, ay, 0, W, H, 10, 10, &lx, &ly) != 0, "empty range refused");

    /* render_unmap undoes render_map at every rotation. */
    for (int r = 0; r < 4; r++) {
        uint32_t lw, lh, nx, ny, bx, by;
        render_logical_size(W, H, rots[r], &lw, &lh);
        for (uint32_t y = 0; y < lh; y += 97)
            for (uint32_t x = 0; x < lw; x += 89) {
                render_map(rots[r], lw, lh, x, y, &nx, &ny);
                render_unmap(rots[r], W, H, nx, ny, &bx, &by);
                CHECK(bx == x && by == y, "rot %d: (%u,%u) -> (%u,%u) -> (%u,%u)", rots[r], x, y, nx, ny, bx, by);
            }
    }
}

static void test_buttons(void) {
    char buf[16];
    CHECK(!strcmp(input_button_name(KEY_POWER, buf, sizeof buf), "power"), "power");
    CHECK(!strcmp(input_button_name(KEY_VOLUMEUP, buf, sizeof buf), "volume_up"), "volume_up");
    CHECK(!strcmp(input_button_name(KEY_VOLUMEDOWN, buf, sizeof buf), "volume_down"), "volume_down");
    CHECK(!strcmp(input_button_name(143, buf, sizeof buf), "key_143"), "KEY_WAKEUP -> key_143: %s", buf);
}

/* The TT7's two devices, from their real modaliases (hardware/discovery input-sysfs.txt). */
static void test_classify(void) {
    struct input_caps caps;
    enum touch_protocol proto;
    input_caps_from_modalias("input:b0019v0001p0001e0100-e0,1,k72,73,74,8F,ramlsfw", &caps);
    CHECK(input_classify(&caps, &proto) == INPUT_ROLE_BUTTONS, "rk29-keypad is buttons");
    int codes[8], n = input_caps_keys(&caps, codes, 8);
    CHECK(n == 4 && codes[0] == 114 && codes[1] == 115 && codes[2] == 116 && codes[3] == 143, "keys 114,115,116,143 (%d)",
          n);

    input_caps_from_modalias("input:b0018v0000p0000e0000-e0,1,3,14,kra2F,30,32,35,36,39,mlsfw", &caps);
    CHECK(input_classify(&caps, &proto) == INPUT_ROLE_TOUCH && proto == TOUCH_MT_B, "gslX680 is MT protocol B");
    CHECK(input_caps_keys(&caps, codes, 8) == 0, "gslX680 has no keys");

    /* Protocol A: MT positions but no ABS_MT_SLOT. */
    input_caps_from_modalias("input:b0018v0000p0000e0000-e0,1,3,k14A,ra35,36,mlsfw", &caps);
    CHECK(input_classify(&caps, &proto) == INPUT_ROLE_TOUCH && proto == TOUCH_MT_A, "protocol A");
    /* Single touch: ABS_X/Y plus BTN_TOUCH (0x14a). */
    input_caps_from_modalias("input:b0018v0000p0000e0000-e0,1,3,k14A,ra0,1,mlsfw", &caps);
    CHECK(input_classify(&caps, &proto) == INPUT_ROLE_TOUCH && proto == TOUCH_SINGLE, "single touch");
    /* ABS_X/Y without BTN_TOUCH (a joystick, an accelerometer) is not a touchscreen. */
    input_caps_from_modalias("input:b0018v0000p0000e0000-e0,3,ra0,1,2,mlsfw", &caps);
    CHECK(input_classify(&caps, &proto) == INPUT_ROLE_NONE, "accelerometer ignored");
    /* Only BTN_* codes (a mouse's buttons) do not make a button device. */
    input_caps_from_modalias("input:b0003v0000p0000e0000-e0,1,2,k110,111,112,r0,1,mlsfw", &caps);
    CHECK(input_classify(&caps, &proto) == INPUT_ROLE_NONE, "mouse ignored");
}

static void test_absinfo_file(void) {
    char path[] = "/tmp/tt7d-absinfo-XXXXXX";
    int fd = mkstemp(path);
    const char text[] = "# code min max\n0x35 0 1280\n0x36 0 800\n0x2f 0 10\n";
    CHECK(write(fd, text, sizeof text - 1) == (ssize_t)(sizeof text - 1), "write");
    close(fd);
    int32_t min, max;
    CHECK(input_absinfo_file(path, ABS_MT_POSITION_X, &min, &max) == 0 && min == 0 && max == 1280, "x");
    CHECK(input_absinfo_file(path, ABS_MT_POSITION_Y, &min, &max) == 0 && max == 800, "y");
    CHECK(input_absinfo_file(path, ABS_X, &min, &max) != 0, "absent code");
    unlink(path);
    CHECK(input_absinfo_file(path, ABS_MT_POSITION_X, &min, &max) != 0, "absent file");
}

int main(void) {
    test_protocol_b();
    test_protocol_a();
    test_single_touch();
    test_throttle();
    test_mapping();
    test_buttons();
    test_classify();
    test_absinfo_file();
    return test_finish("test_input");
}
