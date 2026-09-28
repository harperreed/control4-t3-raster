/* ABOUTME: Parses and validates tt7d's MQTT settings from mqtt.conf, --mqtt-* flags and JSON PUT bodies.
 * ABOUTME: Includes a small parser for flat JSON objects (strings, integers, booleans, null). */
#include "mqtt_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

int mqtt_config_parse(struct mqtt_config *c, const char *text, char *err, size_t errlen) {
    char *copy = strdup(text);
    if (!copy) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    int lineno = 0, rc = 0;
    char *save = NULL;
    /* strtok_r would skip blank lines and so miscount; walk by hand. */
    for (char *line = copy; line && rc == 0; line = save) {
        lineno++;
        char *nl = strchr(line, '\n');
        save = nl ? nl + 1 : NULL;
        if (nl) *nl = 0;
        char *t = trim(line);
        if (!*t || *t == '#') continue;
        char *eq = strchr(t, '=');
        char msg[160];
        if (!eq) {
            snprintf(err, errlen, "line %d: expected KEY=VALUE", lineno);
            rc = -1;
            break;
        }
        *eq = 0;
        if (mqtt_config_set(c, trim(t), trim(eq + 1), msg, sizeof msg) < 0) {
            snprintf(err, errlen, "line %d: %s", lineno, msg);
            rc = -1;
        }
    }
    free(copy);
    return rc;
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

/* ---- a flat JSON object parser --------------------------------------------- */

enum jtype { J_STR, J_INT, J_BOOL, J_NULL, J_OTHER };

struct jparser {
    const char *p, *end;
};

static void skip_ws(struct jparser *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* A JSON string into out (UTF-8, NUL-terminated). Returns 0, -1 if malformed
 * or too long for out. A \u0000 escape sets *has_nul. */
static int parse_string(struct jparser *j, char *out, size_t size, int *has_nul) {
    if (j->p >= j->end || *j->p != '"') return -1;
    j->p++;
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        unsigned cp = (unsigned char)*j->p++;
        int raw = 1; /* a byte as it came: UTF-8 in the body passes through */
        if (cp < 0x20) return -1;
        if (cp == '\\') {
            if (j->p >= j->end) return -1;
            char e = *j->p++;
            raw = 0;
            switch (e) {
            case '"': case '\\': case '/': cp = (unsigned char)e; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break;
            case 't': cp = '\t'; break;
            case 'u': {
                if (j->end - j->p < 4) return -1;
                cp = 0;
                for (int i = 0; i < 4; i++) {
                    int h = hexval(j->p[i]);
                    if (h < 0) return -1;
                    cp = cp << 4 | (unsigned)h;
                }
                j->p += 4;
                if (cp >= 0xd800 && cp <= 0xdfff) return -1; /* no surrogate pairs needed here */
                if (cp == 0) *has_nul = 1;
                break;
            }
            default: return -1;
            }
        }
        char buf[3];
        size_t len = 1;
        if (raw || cp < 0x80) buf[0] = (char)cp;
        else if (cp < 0x800) {
            buf[0] = (char)(0xc0 | cp >> 6);
            buf[1] = (char)(0x80 | (cp & 0x3f));
            len = 2;
        } else {
            buf[0] = (char)(0xe0 | cp >> 12);
            buf[1] = (char)(0x80 | (cp >> 6 & 0x3f));
            buf[2] = (char)(0x80 | (cp & 0x3f));
            len = 3;
        }
        if (n + len >= size) return -1;
        memcpy(out + n, buf, len);
        n += len;
    }
    if (j->p >= j->end) return -1;
    j->p++;
    out[n] = 0;
    return 0;
}

/* One value. Strings go to text; integers and booleans are written as text too. */
static int parse_value(struct jparser *j, enum jtype *type, char *text, size_t size, int *has_nul) {
    skip_ws(j);
    if (j->p >= j->end) return -1;
    if (*j->p == '"') {
        *type = J_STR;
        return parse_string(j, text, size, has_nul);
    }
    static const struct {
        const char *word;
        enum jtype type;
    } words[] = {{"true", J_BOOL}, {"false", J_BOOL}, {"null", J_NULL}};
    for (size_t i = 0; i < 3; i++) {
        size_t n = strlen(words[i].word);
        if ((size_t)(j->end - j->p) >= n && !strncmp(j->p, words[i].word, n)) {
            j->p += n;
            *type = words[i].type;
            snprintf(text, size, "%s", words[i].word);
            return 0;
        }
    }
    /* A number: take the JSON number characters, then decide if it is an integer. */
    const char *start = j->p;
    while (j->p < j->end && strchr("-+0123456789.eE", *j->p)) j->p++;
    size_t n = (size_t)(j->p - start);
    if (n == 0) {
        /* An object or array value: not a setting, but skip nothing; refuse. */
        return -1;
    }
    if (n >= size) return -1;
    memcpy(text, start, n);
    text[n] = 0;
    *type = strspn(text + (text[0] == '-'), "0123456789") == strlen(text + (text[0] == '-')) && n > (text[0] == '-')
                ? J_INT
                : J_OTHER;
    return 0;
}

static const char *kind_word(enum kind k) { return k == BOOL ? "a JSON boolean" : k == INT ? "a whole JSON number" : "a JSON string"; }

int mqtt_config_apply_json(struct mqtt_config *c, const char *body, size_t len, unsigned locked, char *password,
                           size_t password_size, int *password_change, char *bad_field, char *err, size_t errlen) {
    struct mqtt_config next = *c;
    struct jparser j = {body, body + len};
    char key[32], value[300];
    int rc = -1;
    *password_change = 0;
    bad_field[0] = 0;
    password[0] = 0;

    skip_ws(&j);
    if (j.p >= j.end || *j.p != '{') goto syntax;
    j.p++;
    skip_ws(&j);
    if (j.p < j.end && *j.p == '}') {
        j.p++;
        goto done;
    }
    for (;;) {
        int nul = 0;
        skip_ws(&j);
        if (parse_string(&j, key, sizeof key, &nul) != 0 || nul) goto syntax;
        skip_ws(&j);
        if (j.p >= j.end || *j.p != ':') goto syntax;
        j.p++;
        enum jtype type;
        if (parse_value(&j, &type, value, sizeof value, &nul) != 0) {
            /* A syntax error inside a known member's value still names it. */
            snprintf(bad_field, 32, "%s", key);
            snprintf(err, errlen, "%s: malformed or too long value", key);
            goto fail;
        }
        snprintf(bad_field, 32, "%s", key);
        if (!strcmp(key, "password")) {
            if (type == J_NULL) value[0] = 0;
            else if (type != J_STR || nul || !mqtt_password_valid(value)) {
                snprintf(err, errlen, "password must be a string of at most 255 bytes without NUL, CR or LF, or null");
                goto fail;
            }
            snprintf(password, password_size, "%s", value);
            *password_change = 1;
        } else {
            int f = mqtt_field_by_name(key);
            if (f < 0) {
                snprintf(err, errlen, "unknown setting '%s'", key);
                goto fail;
            }
            if (f == MQF_PASSWORD_FILE) {
                snprintf(err, errlen, "password_file can only be set in mqtt.conf or with --mqtt-password-file");
                goto fail;
            }
            if (locked & (1u << f)) {
                snprintf(err, errlen, "%s is set by a --mqtt-%s flag on the tt7d command line", key, key);
                rc = -2;
                goto fail;
            }
            if (f == MQF_CLIENT_ID && type == J_NULL) { /* null = back to the device id */
                type = J_STR;
                value[0] = 0;
            }
            enum jtype want = fields[f].kind == BOOL ? J_BOOL : fields[f].kind == INT ? J_INT : J_STR;
            if (type != want || nul) {
                snprintf(err, errlen, "%s must be %s", key, kind_word(fields[f].kind));
                goto fail;
            }
            if (set_field(&next, f, value, err, errlen) < 0) goto fail;
        }
        bad_field[0] = 0;
        skip_ws(&j);
        if (j.p < j.end && *j.p == ',') {
            j.p++;
            continue;
        }
        if (j.p < j.end && *j.p == '}') {
            j.p++;
            break;
        }
        goto syntax;
    }
done:
    skip_ws(&j);
    if (j.p != j.end) goto syntax;
    *c = next;
    return 0;
syntax:
    bad_field[0] = 0;
    snprintf(err, errlen, "the body must be one flat JSON object of settings");
fail:
    *password_change = 0;
    password[0] = 0;
    return rc;
}
