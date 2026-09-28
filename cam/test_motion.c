/* ABOUTME: Host test for motion.c: presence from luma frames against a slowly adapting background.
 * ABOUTME: Static scene, step change, lighting drift and jumps, hysteresis, absorbing a lasting change. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "motion.h"
#include "test_common.h"

enum { W = 320, H = 180 };

static uint32_t seed = 1;
static int noise(int amp) { /* uniform in [-amp, amp] */
    seed = seed * 1103515245u + 12345u;
    return (int)((seed >> 16) % (uint32_t)(2 * amp + 1)) - amp;
}

static uint8_t clamp8(double v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); }

/* A textured room: gradient plus a few flat patches. gain scales it (lighting),
 * left_add brightens the left half only, and a "person" block is drawn if asked. */
static void scene(uint8_t *luma, double gain, double left_add, int person) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double v = 40 + x * 0.4 + y * 0.3 + ((x / 40 + y / 30) % 2) * 25;
            v = v * gain + (x < W / 2 ? left_add : 0);
            if (person && x >= 100 && x < 200 && y >= 30 && y < 180) v = 200;
            luma[y * W + x] = clamp8(v + noise(3));
        }
}

static struct motion_result feed(struct motion_state *s, const struct motion_params *p, const uint8_t *luma) {
    float grid[MOTION_CELLS];
    motion_grid(luma, W, H, W, grid);
    return motion_step(s, p, grid);
}

static void test_grid(void) {
    uint8_t *luma = malloc(W * H);
    for (int i = 0; i < W * H; i++) luma[i] = (uint8_t)((i % W) < W / 2 ? 10 : 250);
    float grid[MOTION_CELLS];
    motion_grid(luma, W, H, W, grid);
    CHECK(grid[0] == 10.0f, "left cell %f", grid[0]);
    CHECK(grid[MOTION_GRID_W - 1] == 250.0f, "right cell %f", grid[MOTION_GRID_W - 1]);
    CHECK(grid[MOTION_CELLS - 1] == 250.0f, "last cell %f", grid[MOTION_CELLS - 1]);
    free(luma);
}

static void test_static_scene(void) {
    struct motion_params p = motion_params_default(8.0);
    struct motion_state s;
    motion_init(&s);
    uint8_t *luma = malloc(W * H);
    int presence_seen = 0;
    double worst = 0;
    for (int i = 0; i < 200; i++) {
        scene(luma, 1.0, 0, 0);
        struct motion_result r = feed(&s, &p, luma);
        presence_seen |= r.presence;
        if (r.score > worst) worst = r.score;
    }
    CHECK(!presence_seen, "static scene reported presence (worst score %.2f)", worst);
    free(luma);
}

static void test_step_change(void) {
    struct motion_params p = motion_params_default(8.0);
    struct motion_state s;
    motion_init(&s);
    uint8_t *luma = malloc(W * H);
    struct motion_result r = {0};
    for (int i = 0; i < 20; i++) {
        scene(luma, 1.0, 0, 0);
        r = feed(&s, &p, luma);
    }
    CHECK(!r.presence, "no presence before the step");
    scene(luma, 1.0, 0, 1);
    r = feed(&s, &p, luma);
    CHECK(r.presence && r.changed, "step: presence %d changed %d score %.2f", r.presence, r.changed, r.score);
    r = feed(&s, &p, luma);
    CHECK(r.presence && !r.changed, "second frame of the step reports no new change");

    /* The person leaves: presence clears after the hysteresis hold, once. */
    int changes = 0, frames = 0;
    for (; frames < 20 && r.presence; frames++) {
        scene(luma, 1.0, 0, 0);
        r = feed(&s, &p, luma);
        changes += r.changed;
    }
    CHECK(!r.presence && changes == 1, "leave: presence %d after %d frames, %d changes", r.presence, frames, changes);
    CHECK(frames >= p.off_frames, "cleared after %d frames, hold is %d", frames, p.off_frames);
    free(luma);
}

static void test_slow_drift(void) {
    struct motion_params p = motion_params_default(8.0);
    struct motion_state s;
    motion_init(&s);
    uint8_t *luma = malloc(W * H);
    int presence_seen = 0;
    double worst = 0;
    /* Over 400 frames (200 s at 500 ms): overall gain 1.0 -> 1.4, and the left
     * half gains another 40 levels (a lamp on one side warming up). */
    for (int i = 0; i < 400; i++) {
        scene(luma, 1.0 + 0.4 * i / 400.0, 40.0 * i / 400.0, 0);
        struct motion_result r = feed(&s, &p, luma);
        presence_seen |= r.presence;
        if (r.score > worst) worst = r.score;
    }
    CHECK(!presence_seen, "slow drift reported presence (worst score %.2f)", worst);
    free(luma);
}

static void test_uniform_brightness_step(void) {
    struct motion_params p = motion_params_default(8.0);
    struct motion_state s;
    motion_init(&s);
    uint8_t *luma = malloc(W * H);
    for (int i = 0; i < 10; i++) {
        scene(luma, 1.0, 0, 0);
        feed(&s, &p, luma);
    }
    /* Auto-exposure jumps: every pixel 20 levels brighter at once. */
    int presence_seen = 0;
    double worst = 0;
    for (int i = 0; i < 20; i++) {
        scene(luma, 1.0, 0, 0);
        for (int k = 0; k < W * H; k++) luma[k] = (uint8_t)(luma[k] + 20 > 255 ? 255 : luma[k] + 20);
        struct motion_result r = feed(&s, &p, luma);
        presence_seen |= r.presence;
        if (r.score > worst) worst = r.score;
    }
    CHECK(!presence_seen, "uniform brightness step reported presence (worst score %.2f)", worst);
    free(luma);
}

static void test_lasting_change_is_absorbed(void) {
    struct motion_params p = motion_params_default(8.0);
    struct motion_state s;
    motion_init(&s);
    uint8_t *luma = malloc(W * H);
    for (int i = 0; i < 10; i++) {
        scene(luma, 1.0, 0, 0);
        feed(&s, &p, luma);
    }
    /* Something new stays in view (a moved chair): presence at first, then the
     * background learns it and presence clears. */
    struct motion_result r = {0};
    int first = -1, cleared = -1;
    for (int i = 0; i < 2000 && cleared < 0; i++) {
        scene(luma, 1.0, 0, 1);
        r = feed(&s, &p, luma);
        if (r.presence && first < 0) first = i;
        if (!r.presence && first >= 0) cleared = i;
    }
    CHECK(first == 0, "presence first at frame %d", first);
    /* adapt_present keeps a still person for well over 100 frames (50 s at 500 ms). */
    CHECK(cleared > 100, "lasting change cleared at frame %d (should hold a while)", cleared);
    free(luma);
}

static void test_hysteresis(void) {
    struct motion_params p = motion_params_default(10.0);
    CHECK(p.on_threshold == 10.0 && p.off_threshold == 5.0 && p.off_frames == 3, "defaults %.1f %.1f %d",
          p.on_threshold, p.off_threshold, p.off_frames);
    struct motion_state s;
    motion_init(&s);
    CHECK(!motion_hysteresis(&s, &p, 9.9), "below on stays false");
    CHECK(motion_hysteresis(&s, &p, 10.0), "at on turns true");
    CHECK(motion_hysteresis(&s, &p, 6.0), "between off and on stays true");
    CHECK(motion_hysteresis(&s, &p, 4.0), "quiet 1 stays true");
    CHECK(motion_hysteresis(&s, &p, 4.0), "quiet 2 stays true");
    CHECK(motion_hysteresis(&s, &p, 7.0), "a busy frame resets the quiet count");
    CHECK(motion_hysteresis(&s, &p, 4.0), "quiet 1 again");
    CHECK(motion_hysteresis(&s, &p, 4.0), "quiet 2 again");
    CHECK(!motion_hysteresis(&s, &p, 4.0), "quiet 3 turns false");
    CHECK(!motion_hysteresis(&s, &p, 9.0), "back below on stays false");
}

int main(void) {
    test_grid();
    test_static_scene();
    test_step_change();
    test_slow_drift();
    test_uniform_brightness_step();
    test_lasting_change_is_absorbed();
    test_hysteresis();
    return test_finish("cam motion: static / step / drift / brightness step / hysteresis / absorb");
}
