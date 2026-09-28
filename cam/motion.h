/* ABOUTME: Presence detector over luma frames: a coarse grid compared against a slowly adapting background.
 * ABOUTME: Pure functions over arrays, so the host tests drive it with synthetic scenes. */
#ifndef CAM_MOTION_H
#define CAM_MOTION_H

#include <stdint.h>

#define MOTION_GRID_W 32
#define MOTION_GRID_H 18
#define MOTION_CELLS (MOTION_GRID_W * MOTION_GRID_H)

struct motion_params {
    double on_threshold;  /* score at or above this turns presence on */
    double off_threshold; /* score below this counts as a quiet frame */
    int off_frames;       /* consecutive quiet frames before presence turns off */
    double adapt;         /* background update rate per frame while nobody is there */
    double adapt_present; /* slower rate while presence is on, so a person standing still stays seen */
};

struct motion_state {
    float bg[MOTION_CELLS];
    int initialized;
    int presence;
    int quiet_frames;
};

struct motion_result {
    double score; /* mean absolute deviation from background, in luma levels, after removing the mean shift */
    int presence;
    int changed; /* presence differs from the previous frame */
};

/* on = threshold, off = threshold / 2, 3 quiet frames, adapt 0.05, adapt_present 0.005. */
struct motion_params motion_params_default(double threshold);

void motion_init(struct motion_state *s);

/* Mean luma of each grid cell. stride is the byte distance between rows. */
void motion_grid(const uint8_t *luma, int w, int h, int stride, float grid[MOTION_CELLS]);

/* The first call only seeds the background. Later calls score the grid,
 * run the hysteresis, then move the background toward the grid. */
struct motion_result motion_step(struct motion_state *s, const struct motion_params *p,
                                 const float grid[MOTION_CELLS]);

/* The on/off decision alone; returns the new presence. */
int motion_hysteresis(struct motion_state *s, const struct motion_params *p, double score);

#endif
