/* ABOUTME: Parses and validates tt7d's MQTT settings from mqtt.conf, --mqtt-* flags and JSON PUT bodies.
 * ABOUTME: Includes a small parser for flat JSON objects (strings, integers, booleans, null). */
#include "mqtt_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flatconf.h"

enum kind { BOOL, INT, STR };

static const struct field {
    const char *name;
    enum kind kind;
    size_t offset, size; /* STR: the char array; INT/BOOL: an int */
    long min, max;       /* INT range */
} fields[MQF_COUNT] = {
    [MQF_ENABLED] = {"enabled", BOOL, offsetof(struct mqtt_config, enabled), 0, 0, 0},
    [MQF_HOST] = {"host", STR, offsetof(struct mqtt_config, host), sizeof ((struct mqtt_config *)0)->host, 0, 0},
    [MQF_PORT] = {"port", INT, offsetof(struct mqtt_config, port), 0, 1, 65535},
    [MQF_USERNAME] = {"username", STR, offsetof(struct mqtt_config, username),
                      sizeof ((struct mqtt_config *)0)->username, 0, 0},
    [MQF_PASSWORD_FILE] = {"password_file", STR, offsetof(struct mqtt_config, password_file),
                           sizeof ((struct mqtt_config *)0)->password_file, 0, 0},
    [MQF_PREFIX] = {"prefix", STR, offsetof(struct mqtt_config, prefix), sizeof ((struct mqtt_config *)0)->prefix, 0,
                    0},
    [MQF_CLIENT_ID] = {"client_id", STR, offsetof(struct mqtt_config, client_id),
                       sizeof ((struct mqtt_config *)0)->client_id, 0, 0},
    [MQF_KEEPALIVE] = {"keepalive", INT, offsetof(struct mqtt_config, keepalive), 0, 5, 3600},
    [MQF_TELEMETRY_INTERVAL] = {"telemetry_interval", INT, offsetof(struct mqtt_config, telemetry_interval), 0, 1,
                                86400},
    [MQF_HA_DISCOVERY] = {"ha_discovery", BOOL, offsetof(struct mqtt_config, ha_discovery), 0, 0, 0},
    [MQF_ALLOW_REBOOT_CMD] = {"allow_reboot_cmd", BOOL, offsetof(struct mqtt_config, allow_reboot_cmd), 0, 0, 0},
};

void mqtt_config_defaults(struct mqtt_config *c, const char *data_dir) {
    memset(c, 0, sizeof *c);
    c->enabled = 1;
    c->port = 1883;
    snprintf(c->password_file, sizeof c->password_file, "%s/mqtt-password", data_dir);
    snprintf(c->prefix, sizeof c->prefix, "tt7");
    c->keepalive = 60;
    c->telemetry_interval = 10;
    c->ha_discovery = 1;
}

const char *mqtt_field_name(int field) { return field >= 0 && field < MQF_COUNT ? fields[field].name : "?"; }

int mqtt_field_by_name(const char *key) {
    for (int i = 0; i < MQF_COUNT; i++)
        if (!strcmp(fields[i].name, key)) return i;
    return -1;
}

static int has_control(const char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f) return 1;
    return 0;
}

/* A topic prefix: no wildcards, no empty levels, no leading or trailing '/'. */
static int prefix_valid(const char *p) {
    size_t n = strlen(p);
    return n > 0 && !has_control(p) && !strpbrk(p, "+# ") && p[0] != '/' && p[n - 1] != '/' && !strstr(p, "//");
}

/* Client ids: letters, digits and - _ . : only, so every broker accepts them. */
static int client_id_valid(const char *s) {
    return strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:") == strlen(s);
}

static int host_valid(const char *s) {
    struct in_addr a;
    return !s[0] || inet_pton(AF_INET, s, &a) == 1;
}

int mqtt_password_valid(const char *p) { return strlen(p) <= 255 && !strpbrk(p, "\r\n"); }

static int set_field(struct mqtt_config *c, int f, const char *value, char *err, size_t errlen) {
    const struct field *fd = &fields[f];
    char *base = (char *)c + fd->offset;
    if (fd->kind == BOOL) {
        if (strcmp(value, "true") && strcmp(value, "false")) {
            snprintf(err, errlen, "%s must be true or false", fd->name);
            return -1;
        }
        *(int *)base = !strcmp(value, "true");
        return f;
    }
    if (fd->kind == INT) {
        char *end;
        errno = 0;
        long v = strtol(value, &end, 10);
        if (!value[0] || *end || errno || v < fd->min || v > fd->max) {
            snprintf(err, errlen, "%s must be a whole number from %ld to %ld", fd->name, fd->min, fd->max);
            return -1;
        }
        *(int *)base = (int)v;
        return f;
    }
    const char *why = NULL;
    if (strlen(value) >= fd->size) why = "is too long";
    else if (has_control(value)) why = "has control characters";
    else if (f == MQF_HOST && !host_valid(value))
        why = "must be an IPv4 address such as 192.168.1.10 (host names are not resolved)";
    else if (f == MQF_PREFIX && !prefix_valid(value))
        why = "must be non-empty, without + # spaces, empty levels, or a leading or trailing /";
    else if (f == MQF_CLIENT_ID && !client_id_valid(value)) why = "may only use letters, digits and - _ . :";
    else if (f == MQF_PASSWORD_FILE && value[0] != '/') why = "must be an absolute path";
    if (why) {
        snprintf(err, errlen, "%s %s", fd->name, why);
        return -1;
    }
    snprintf(base, fd->size, "%s", value);
    return f;
}

int mqtt_config_set(struct mqtt_config *c, const char *key, const char *value, char *err, size_t errlen) {
    int f = mqtt_field_by_name(key);
    if (f < 0) {
        snprintf(err, errlen, "unknown key '%s'", key);
        return -1;
    }
    return set_field(c, f, value, err, errlen);
}

static int set_line(void *ctx, const char *key, const char *value, char *err, size_t errlen) {
    return mqtt_config_set(ctx, key, value, err, errlen);
}

int mqtt_config_parse(struct mqtt_config *c, const char *text, char *err, size_t errlen) {
    return flatconf_parse_lines(text, set_line, c, err, errlen);
}

void mqtt_config_format(const struct mqtt_config *c, struct sbuf *out) {
    sb_puts(out, "# tt7d MQTT settings (KEY=VALUE). Written by PUT /api/v1/config/mqtt; see tt7d/README.md.\n"
                 "# The broker password is not here: it lives in the file named by password_file.\n");
    for (int f = 0; f < MQF_COUNT; f++) {
        const char *base = (const char *)c + fields[f].offset;
        if (fields[f].kind == STR) sb_printf(out, "%s=%s\n", fields[f].name, base);
        else if (fields[f].kind == INT) sb_printf(out, "%s=%d\n", fields[f].name, *(const int *)base);
        else sb_printf(out, "%s=%s\n", fields[f].name, *(const int *)base ? "true" : "false");
    }
}

static const char *kind_word(enum kind k) { return k == BOOL ? "a JSON boolean" : k == INT ? "a whole JSON number" : "a JSON string"; }

struct apply_ctx {
    struct mqtt_config next;
    unsigned locked;
    char *password;
    size_t password_size;
    int *password_change;
    char *err;
    size_t errlen;
    char *last_key; /* [32]: the member being applied */
};

#define APPLY_INVALID 1
#define APPLY_LOCKED 2

static int apply_member(void *vctx, const char *key, enum jtype type, const char *value, int nul) {
    struct apply_ctx *a = vctx;
    snprintf(a->last_key, 32, "%s", key);
    if (!strcmp(key, "password")) {
        if (type == J_NULL) value = "";
        else if (type != J_STR || nul || !mqtt_password_valid(value)) {
            snprintf(a->err, a->errlen, "password must be a string of at most 255 bytes without NUL, CR or LF, or null");
            return APPLY_INVALID;
        }
        snprintf(a->password, a->password_size, "%s", value);
        *a->password_change = 1;
        return 0;
    }
    int f = mqtt_field_by_name(key);
    if (f < 0) {
        snprintf(a->err, a->errlen, "unknown setting '%s'", key);
        return APPLY_INVALID;
    }
    if (f == MQF_PASSWORD_FILE) {
        snprintf(a->err, a->errlen, "password_file can only be set in mqtt.conf or with --mqtt-password-file");
        return APPLY_INVALID;
    }
    if (a->locked & (1u << f)) {
        snprintf(a->err, a->errlen, "%s is set by a --mqtt-%s flag on the tt7d command line", key, key);
        return APPLY_LOCKED;
    }
    if (f == MQF_CLIENT_ID && type == J_NULL) { /* null = back to the device id */
        type = J_STR;
        value = "";
    }
    enum jtype want = fields[f].kind == BOOL ? J_BOOL : fields[f].kind == INT ? J_INT : J_STR;
    if (type != want || nul) {
        snprintf(a->err, a->errlen, "%s must be %s", key, kind_word(fields[f].kind));
        return APPLY_INVALID;
    }
    return set_field(&a->next, f, value, a->err, a->errlen) < 0 ? APPLY_INVALID : 0;
}

int mqtt_config_apply_json(struct mqtt_config *c, const char *body, size_t len, unsigned locked, char *password,
                           size_t password_size, int *password_change, char *bad_field, char *err, size_t errlen) {
    struct apply_ctx a = {*c, locked, password, password_size, password_change, err, errlen, NULL};
    *password_change = 0;
    password[0] = 0;
    /* flatjson_each names the member of a malformed value in bad_field; a
     * refused value is named here, from the key being applied. */
    char last_key[32] = "";
    a.last_key = last_key;
    int rc = flatjson_each(body, len, apply_member, &a, bad_field, 32);
    if (rc != 0 && rc != FLATJSON_SYNTAX) snprintf(bad_field, 32, "%s", last_key);
    if (rc == 0) {
        *c = a.next;
        return 0;
    }
    if (rc == FLATJSON_SYNTAX && bad_field[0]) snprintf(err, errlen, "%s: malformed or too long value", bad_field);
    else if (rc == FLATJSON_SYNTAX) snprintf(err, errlen, "the body must be one flat JSON object of settings");
    *password_change = 0;
    password[0] = 0;
    return rc == APPLY_LOCKED ? -2 : -1;
}
