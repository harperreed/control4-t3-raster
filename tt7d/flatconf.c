/* ABOUTME: KEY=VALUE line reader and flat JSON object walker shared by the MQTT and camera settings.
 * ABOUTME: The JSON side takes strings (with escapes), integers, booleans and null; nothing nested. */
#include "flatconf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- KEY=VALUE lines ------------------------------------------------------- */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

int flatconf_parse_lines(const char *text, flatconf_set_fn set, void *ctx, char *err, size_t errlen) {
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
        if (set(ctx, trim(t), trim(eq + 1), msg, sizeof msg) < 0) {
            snprintf(err, errlen, "line %d: %s", lineno, msg);
            rc = -1;
        }
    }
    free(copy);
    return rc;
}

/* ---- a flat JSON object parser --------------------------------------------- */

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

int flatjson_each(const char *body, size_t len, flatjson_member_fn member, void *ctx, char *bad_key,
                  size_t bad_key_size) {
    struct jparser j = {body, body + len};
    char key[32], value[300];
    bad_key[0] = 0;

    skip_ws(&j);
    if (j.p >= j.end || *j.p != '{') return FLATJSON_SYNTAX;
    j.p++;
    skip_ws(&j);
    if (j.p < j.end && *j.p == '}') {
        j.p++;
        goto done;
    }
    for (;;) {
        int nul = 0;
        skip_ws(&j);
        if (parse_string(&j, key, sizeof key, &nul) != 0 || nul) return FLATJSON_SYNTAX;
        skip_ws(&j);
        if (j.p >= j.end || *j.p != ':') return FLATJSON_SYNTAX;
        j.p++;
        enum jtype type;
        if (parse_value(&j, &type, value, sizeof value, &nul) != 0) {
            /* A syntax error inside a member's value still names it. */
            snprintf(bad_key, bad_key_size, "%s", key);
            return FLATJSON_SYNTAX;
        }
        int rc = member(ctx, key, type, value, nul);
        if (rc != 0) return rc;
        skip_ws(&j);
        if (j.p < j.end && *j.p == ',') {
            j.p++;
            continue;
        }
        if (j.p < j.end && *j.p == '}') {
            j.p++;
            break;
        }
        return FLATJSON_SYNTAX;
    }
done:
    skip_ws(&j);
    return j.p == j.end ? 0 : FLATJSON_SYNTAX;
}
