/* ABOUTME: tt7d's camera settings: defaults (camera off), camera.conf (KEY=VALUE), the --camera flag and
 * ABOUTME: JSON bodies for PUT /api/v1/config/camera. One field table and validator for every source. */
#ifndef TT7D_CAMERA_CONFIG_H
#define TT7D_CAMERA_CONFIG_H

#include <stddef.h>

#include "json.h"

enum camera_field {
    CAMF_ENABLED,              /* camera.conf "camera", JSON "enabled" */
    CAMF_PRESENCE,             /* presence detection streams frames through the motion detector */
    CAMF_PRESENCE_WAKE,        /* presence wakes a blanked display */
    CAMF_PRESENCE_IDLE_BLANK,  /* seconds without presence before a display presence woke blanks again; 0 = never */
    CAMF_PRESENCE_INTERVAL_MS, /* time between presence frames */
    CAMF_PRESENCE_THRESHOLD,   /* motion score (luma levels) that turns presence on; off at half */
    CAMF_SNAPSHOT_INTERVAL,    /* seconds between snapshots published to MQTT; 0 = only on request */
    CAMF_SNAPSHOT_MAX_AGE,     /* seconds a snapshot is served from memory before a new capture */
    CAMF_SETTLE_FRAMES,        /* frames thrown away (auto-exposure) before a snapshot's frame */
    CAMF_WORKER_TIMEOUT,       /* seconds of silence before the camera worker is killed */
    CAMF_COUNT
};

struct camera_config {
    int enabled;
    int presence;
    int presence_wake;
    int presence_idle_blank_s;
    int presence_interval_ms;
    int presence_threshold;
    int snapshot_interval_s;
    int snapshot_max_age_s;
    int settle_frames;
    int worker_timeout_s;
};

/* Camera off, presence off, wake off, idle blank 0 (never), 500 ms between
 * presence frames, threshold 8, no periodic snapshots, 2 s snapshot cache,
 * 29 settle frames (tt7cam snap's 30 frames, the last kept), 10 s worker
 * timeout. */
void camera_config_defaults(struct camera_config *c);

/* The camera.conf key and the JSON member of a field, and the field of a
 * camera.conf key or JSON member (-1 if unknown). They differ only for the
 * on/off switch: "camera" in camera.conf and on the command line, "enabled"
 * in JSON. */
const char *camera_field_conf_key(int field);
const char *camera_field_json_key(int field);
int camera_field_by_conf_key(const char *key);
int camera_field_by_json_key(const char *key);

/* Set a field from text: switches take on/off (or true/false), numbers
 * decimal integers within the field's range. Returns the field, or -1 with
 * a message in err (c unchanged). */
int camera_config_set(struct camera_config *c, const char *conf_key, const char *value, char *err, size_t errlen);

/* Apply camera.conf text. Returns 0, or -1 with "line N: ..." in err. */
int camera_config_parse(struct camera_config *c, const char *text, char *err, size_t errlen);

/* camera.conf text for c: every key, switches as on/off, with a header comment. */
void camera_config_format(const struct camera_config *c, struct sbuf *out);

/* Apply a JSON object of fields ({"enabled": true, "presence_interval_ms": 500}).
 * Switches must be JSON booleans and numbers whole JSON numbers. Fields in
 * `locked` (a bitmask of enum camera_field, set by a command-line flag) are
 * refused with -2. Returns 0, or -1/-2 with the member in bad_field[32] ("" for
 * a syntax error) and a message in err; c is then unchanged. */
int camera_config_apply_json(struct camera_config *c, const char *body, size_t len, unsigned locked, char *bad_field,
                             char *err, size_t errlen);

/* The settings as JSON members (no braces), JSON names, switches as booleans. */
void camera_config_json_members(const struct camera_config *c, struct sbuf *sb);

#endif
