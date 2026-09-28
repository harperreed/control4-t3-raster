/* ABOUTME: Grid-mean presence scoring with shift-compensated difference, hysteresis and background adaptation.
 * ABOUTME: Uniform lighting shifts cancel out; slow non-uniform drift is absorbed by the background. */
#include "motion.h"

#include <math.h>
#include <string.h>

struct motion_params motion_params_default(double threshold) {
    struct motion_params p = {
        .on_threshold = threshold,
        .off_threshold = threshold / 2.0,
        .off_frames = 3,
        .adapt = 0.05,
        .adapt_present = 0.005,
    };
    return p;
}

void motion_init(struct motion_state *s) { memset(s, 0, sizeof *s); }

void motion_grid(const uint8_t *luma, int w, int h, int stride, float grid[MOTION_CELLS]) {
    for (int gy = 0; gy < MOTION_GRID_H; gy++) {
        int y0 = gy * h / MOTION_GRID_H, y1 = (gy + 1) * h / MOTION_GRID_H;
        for (int gx = 0; gx < MOTION_GRID_W; gx++) {
            int x0 = gx * w / MOTION_GRID_W, x1 = (gx + 1) * w / MOTION_GRID_W;
            uint32_t sum = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) sum += luma[(long)y * stride + x];
            uint32_t n = (uint32_t)((y1 - y0) * (x1 - x0));
            grid[gy * MOTION_GRID_W + gx] = n ? (float)sum / (float)n : 0.0f;
        }
    }
}

int motion_hysteresis(struct motion_state *s, const struct motion_params *p, double score) {
    if (!s->presence) {
        if (score >= p->on_threshold) {
            s->presence = 1;
            s->quiet_frames = 0;
        }
    } else if (score < p->off_threshold) {
        if (++s->quiet_frames >= p->off_frames) {
            s->presence = 0;
            s->quiet_frames = 0;
        }
    } else {
        s->quiet_frames = 0;
    }
    return s->presence;
}

struct motion_result motion_step(struct motion_state *s, const struct motion_params *p,
                                 const float grid[MOTION_CELLS]) {
    struct motion_result r = {0.0, s->presence, 0};
    if (!s->initialized) {
        memcpy(s->bg, grid, sizeof s->bg);
        s->initialized = 1;
        return r;
    }

    /* Subtract the mean difference first, so a uniform brightness change
     * (auto-exposure step, lights dimming evenly) does not score. */
    double diff[MOTION_CELLS], mean = 0.0;
    for (int i = 0; i < MOTION_CELLS; i++) {
        diff[i] = (double)grid[i] - (double)s->bg[i];
        mean += diff[i];
    }
    mean /= MOTION_CELLS;
    double score = 0.0;
    for (int i = 0; i < MOTION_CELLS; i++) score += fabs(diff[i] - mean);
    r.score = score / MOTION_CELLS;

    int before = s->presence;
    r.presence = motion_hysteresis(s, p, r.score);
    r.changed = r.presence != before;

    double rate = r.presence ? p->adapt_present : p->adapt;
    for (int i = 0; i < MOTION_CELLS; i++) s->bg[i] += (float)(rate * diff[i]);
    return r;
}
