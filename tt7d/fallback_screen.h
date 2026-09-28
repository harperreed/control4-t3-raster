/* ABOUTME: Puts the fallback clock on the display: runs the fallback state machine from the poll loop,
 * ABOUTME: redraws the clock face when its words change, and answers heartbeat, /frame and /state for it. */
#ifndef TT7D_FALLBACK_SCREEN_H
#define TT7D_FALLBACK_SCREEN_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "clockface.h"
#include "display.h"
#include "fallback.h"
#include "frame.h"
#include "http.h"
#include "json.h"
#include "server.h"

struct fallback_screen {
    struct fallback state;
    struct display *disp;
    struct frame_store *frames;
    struct clockface_fonts fonts;
    int hour12;
    const char *marker; /* the NTP sync marker (timesync.h) */
    char tz[64];

    int drawn;                  /* the clock face is what the screen shows now */
    struct clockface_text shown; /* its words */
    char id[48];                /* "fallback-clock-<unix minute>" of that drawing */
    struct timespec drawn_wall, drawn_mono;
    unsigned long redraws;
    int64_t next_check_ms; /* when to look at the clock, sync marker and IP again */
    int synced_logged;

    uint8_t *png; /* the drawing as PNG for GET /frame/image, encoded on demand */
    size_t png_len;
    char png_sha[65];
    unsigned long png_redraw; /* which drawing png holds (0: none) */
};

struct fallback_screen_config {
    unsigned timeout_s; /* 0 disables the fallback */
    int hour12;
    const char *marker;
    const char *tz; /* a valid POSIX TZ string (timesync_tz_choose) */
};

/* Load the embedded fonts and start the state machine. `restored`: a
 * persisted frame is already on screen, which counts as a frame at boot.
 * Returns 0, or -1 with err. */
int fallback_screen_init(struct fallback_screen *s, const struct fallback_screen_config *c, struct display *d,
                         struct frame_store *frames, int restored, char *err, size_t errlen);

/* A frame PUT answered 200 (new or duplicate): it is on screen now. */
void fallback_screen_frame_accepted(struct fallback_screen *s);

/* Poll loop hooks: lower *wait_ms to the next timeout or redraw check, and
 * after every wakeup apply the timeout and redraw if the words changed. */
void fallback_screen_prepare(struct fallback_screen *s, int64_t *wait_ms);
void fallback_screen_service(struct fallback_screen *s);

/* POST /api/v1/heartbeat (token required). check_head returns
 * FALLBACK_NOT_MINE for any other path, 0 to go on to handle(), or -1 with
 * resp filled. */
#define FALLBACK_NOT_MINE 1
int fallback_screen_check_head(const char *token, const struct http_request *req, struct response *resp);
void fallback_screen_handle(struct fallback_screen *s, struct response *resp);

/* GET /api/v1/frame and /api/v1/frame/image while the clock shows. */
void fallback_screen_frame_json(struct fallback_screen *s, struct sbuf *sb);
void fallback_screen_image(struct fallback_screen *s, struct response *resp);

/* The "fallback" and "clock" members for /api/v1/state (keys included, no
 * surrounding commas). */
void fallback_screen_state_members(struct fallback_screen *s, struct sbuf *sb);

/* 1 while the clock face is what the screen shows: drawn, and no frame
 * (the panel's test pattern, say) drawn over it since. */
static inline int fallback_screen_showing(const struct fallback_screen *s) {
    return s && s->drawn && !s->frames->on_screen;
}

/* What the screen shows: the clock face's id while it is drawn, else the
 * frame store's frame (NULL id when unknown), and seconds since it was drawn
 * or received (-1 for nothing). Inline, so mqtt.c can use it without
 * linking the display code. */
static inline void fallback_screen_current(const struct fallback_screen *s, const struct frame_store *fs,
                                           const char **id, double *age_s) {
    struct timespec now, since;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (fallback_screen_showing(s)) {
        *id = s->id;
        since = s->drawn_mono;
    } else if (fs->have) {
        *id = fs->id[0] ? fs->id : NULL;
        since = fs->received_mono;
    } else {
        *id = NULL;
        *age_s = -1;
        return;
    }
    *age_s = (double)(now.tv_sec - since.tv_sec) + (double)(now.tv_nsec - since.tv_nsec) / 1e9;
}

#endif
