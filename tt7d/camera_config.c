/* ABOUTME: Parses and validates tt7d's camera settings from camera.conf, the --camera flag and JSON bodies.
 * ABOUTME: Every field is a switch or a bounded integer; flatconf.c does the reading. */
#include "camera_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flatconf.h"

enum kind { SWITCH, INT };

static const struct field {
    const char *conf_key, *json_key;
    enum kind kind;
    size_t offset;
    long min, max;
} fields[CAMF_COUNT] = {
    [CAMF_ENABLED] = {"camera", "enabled", SWITCH, offsetof(struct camera_config, enabled), 0, 0},
    [CAMF_PRESENCE] = {"presence", "presence", SWITCH, offsetof(struct camera_config, presence), 0, 0},
    [CAMF_PRESENCE_WAKE] = {"presence_wake", "presence_wake", SWITCH, offsetof(struct camera_config, presence_wake),
                            0, 0},
    [CAMF_PRESENCE_IDLE_BLANK] = {"presence_idle_blank_s", "presence_idle_blank_s", INT,
                                  offsetof(struct camera_config, presence_idle_blank_s), 0, 86400},
    [CAMF_PRESENCE_INTERVAL_MS] = {"presence_interval_ms", "presence_interval_ms", INT,
                                   offsetof(struct camera_config, presence_interval_ms), 100, 10000},
    [CAMF_PRESENCE_THRESHOLD] = {"presence_threshold", "presence_threshold", INT,
                                 offsetof(struct camera_config, presence_threshold), 1, 255},
    [CAMF_SNAPSHOT_INTERVAL] = {"snapshot_interval", "snapshot_interval", INT,
                                offsetof(struct camera_config, snapshot_interval_s), 0, 86400},
    [CAMF_SNAPSHOT_MAX_AGE] = {"snapshot_max_age_s", "snapshot_max_age_s", INT,
                               offsetof(struct camera_config, snapshot_max_age_s), 0, 3600},
    [CAMF_SETTLE_FRAMES] = {"settle_frames", "settle_frames", INT, offsetof(struct camera_config, settle_frames), 0,
                            300},
    [CAMF_WORKER_TIMEOUT] = {"worker_timeout_s", "worker_timeout_s", INT,
                             offsetof(struct camera_config, worker_timeout_s), 2, 600},
};

void camera_config_defaults(struct camera_config *c) {
    memset(c, 0, sizeof *c);
    c->presence_interval_ms = 500;
    c->presence_threshold = 8; /* tt7cam motion's default */
    c->snapshot_max_age_s = 2;
    c->settle_frames = 29; /* tt7cam snap's 30 frames, the last kept: proven on the panel */
    c->worker_timeout_s = 10;
}

const char *camera_field_conf_key(int f) { return f >= 0 && f < CAMF_COUNT ? fields[f].conf_key : "?"; }
const char *camera_field_json_key(int f) { return f >= 0 && f < CAMF_COUNT ? fields[f].json_key : "?"; }

int camera_field_by_conf_key(const char *key) {
    for (int i = 0; i < CAMF_COUNT; i++)
        if (!strcmp(fields[i].conf_key, key)) return i;
    return -1;
}

int camera_field_by_json_key(const char *key) {
    for (int i = 0; i < CAMF_COUNT; i++)
        if (!strcmp(fields[i].json_key, key)) return i;
    return -1;
}

static int *slot(struct camera_config *c, int f) { return (int *)((char *)c + fields[f].offset); }
static int value_of(const struct camera_config *c, int f) {
    return *(const int *)((const char *)c + fields[f].offset);
}

/* A field from its text form; the name in messages is `name`. */
static int set_field(struct camera_config *c, int f, const char *name, const char *value, char *err, size_t errlen) {
    const struct field *fd = &fields[f];
    if (fd->kind == SWITCH) {
        int on = !strcmp(value, "on") || !strcmp(value, "true");
        if (!on && strcmp(value, "off") && strcmp(value, "false")) {
            snprintf(err, errlen, "%s must be on or off", name);
            return -1;
        }
        *slot(c, f) = on;
        return f;
    }
    char *end;
    errno = 0;
    long v = strtol(value, &end, 10);
    if (!value[0] || *end || errno || v < fd->min || v > fd->max) {
        snprintf(err, errlen, "%s must be a whole number from %ld to %ld", name, fd->min, fd->max);
        return -1;
    }
    *slot(c, f) = (int)v;
    return f;
}

int camera_config_set(struct camera_config *c, const char *conf_key, const char *value, char *err, size_t errlen) {
    int f = camera_field_by_conf_key(conf_key);
    if (f < 0) {
        snprintf(err, errlen, "unknown key '%s'", conf_key);
        return -1;
    }
    return set_field(c, f, conf_key, value, err, errlen);
}

static int set_line(void *ctx, const char *key, const char *value, char *err, size_t errlen) {
    return camera_config_set(ctx, key, value, err, errlen);
}

int camera_config_parse(struct camera_config *c, const char *text, char *err, size_t errlen) {
    return flatconf_parse_lines(text, set_line, c, err, errlen);
}

void camera_config_format(const struct camera_config *c, struct sbuf *out) {
    sb_puts(out, "# tt7d camera settings (KEY=VALUE). Written by PUT /api/v1/config/camera; see tt7d/README.md.\n"
                 "# The camera is off unless camera=on. Snapshots are never written to disk.\n");
    for (int f = 0; f < CAMF_COUNT; f++) {
        if (fields[f].kind == SWITCH) sb_printf(out, "%s=%s\n", fields[f].conf_key, value_of(c, f) ? "on" : "off");
        else sb_printf(out, "%s=%d\n", fields[f].conf_key, value_of(c, f));
    }
}

struct apply_ctx {
    struct camera_config next;
    unsigned locked;
    char *last_key; /* [32] */
    char *err;
    size_t errlen;
};

#define APPLY_INVALID 1
#define APPLY_LOCKED 2

static int apply_member(void *vctx, const char *key, enum jtype type, const char *value, int nul) {
    struct apply_ctx *a = vctx;
    snprintf(a->last_key, 32, "%s", key);
    int f = camera_field_by_json_key(key);
    if (f < 0) {
        snprintf(a->err, a->errlen, "unknown setting '%s'", key);
        return APPLY_INVALID;
    }
    if (a->locked & (1u << f)) {
        snprintf(a->err, a->errlen, "%s is set by --%s on the tt7d command line", key, fields[f].conf_key);
        return APPLY_LOCKED;
    }
    enum jtype want = fields[f].kind == SWITCH ? J_BOOL : J_INT;
    if (type != want || nul) {
        snprintf(a->err, a->errlen, "%s must be %s", key,
                 fields[f].kind == SWITCH ? "a JSON boolean" : "a whole JSON number");
        return APPLY_INVALID;
    }
    return set_field(&a->next, f, key, value, a->err, a->errlen) < 0 ? APPLY_INVALID : 0;
}

int camera_config_apply_json(struct camera_config *c, const char *body, size_t len, unsigned locked, char *bad_field,
                             char *err, size_t errlen) {
    char last_key[32] = "";
    struct apply_ctx a = {*c, locked, last_key, err, errlen};
    int rc = flatjson_each(body, len, apply_member, &a, bad_field, 32);
    if (rc == 0) {
        *c = a.next;
        return 0;
    }
    if (rc == FLATJSON_SYNTAX) {
        if (bad_field[0]) snprintf(err, errlen, "%s: malformed or too long value", bad_field);
        else snprintf(err, errlen, "the body must be one flat JSON object of settings");
        return -1;
    }
    snprintf(bad_field, 32, "%s", last_key);
    return rc == APPLY_LOCKED ? -2 : -1;
}

void camera_config_json_members(const struct camera_config *c, struct sbuf *sb) {
    for (int f = 0; f < CAMF_COUNT; f++) {
        if (fields[f].kind == SWITCH)
            sb_printf(sb, "%s\"%s\":%s", f ? "," : "", fields[f].json_key, value_of(c, f) ? "true" : "false");
        else sb_printf(sb, "%s\"%s\":%d", f ? "," : "", fields[f].json_key, value_of(c, f));
    }
}
