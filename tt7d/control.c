/* ABOUTME: Pure helpers for the control panel endpoints (see control.h): a strict parser for the one
 * ABOUTME: small JSON body tt7d accepts, brightness range rules, and log tail formatting. */
#include "control.h"

#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* A string without escapes at *p. Sets *s and *n; returns the byte after the
 * closing quote, or NULL. */
static const char *plain_string(const char *p, const char *end, const char **s, size_t *n) {
    if (p >= end || *p != '"') return NULL;
    *s = ++p;
    while (p < end && *p != '"' && *p != '\\') p++;
    if (p >= end || *p != '"') return NULL;
    *n = (size_t)(p - *s);
    return p + 1;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* An integer of at most 9 digits (fits a 32-bit long), optionally negative.
 * Returns the byte after it, or NULL. */
static const char *small_integer(const char *p, const char *end, long *v) {
    int neg = p < end && *p == '-';
    if (neg) p++;
    const char *digits = p;
    long n = 0;
    while (p < end && is_digit(*p) && p - digits < 9) n = n * 10 + (*p++ - '0');
    if (p == digits || (p < end && (is_digit(*p) || *p == '.' || *p == 'e' || *p == 'E'))) return NULL;
    *v = neg ? -n : n;
    return p;
}

static int is_key(const char *k, size_t n, const char *want) { return n == strlen(want) && !memcmp(k, want, n); }

int brightness_parse(const char *body, size_t len, long *value, int *percent) {
    const char *p = body, *end = body + len;
    int have_value = 0, have_unit = 0;
    *percent = 0;
    p = skip_ws(p, end);
    if (p >= end || *p++ != '{') return -1;
    for (;;) {
        const char *key, *s;
        size_t klen, n;
        p = plain_string(skip_ws(p, end), end, &key, &klen);
        if (!p) return -1;
        p = skip_ws(p, end);
        if (p >= end || *p++ != ':') return -1;
        p = skip_ws(p, end);
        if (is_key(key, klen, "value") && !have_value) {
            p = small_integer(p, end, value);
            have_value = 1;
        } else if (is_key(key, klen, "unit") && !have_unit) {
            p = plain_string(p, end, &s, &n);
            if (p && is_key(s, n, "percent")) *percent = 1;
            else if (!p || !is_key(s, n, "raw")) return -1;
            have_unit = 1;
        } else {
            return -1;
        }
        if (!p) return -1;
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        if (p < end && *p == '}') break;
        return -1;
    }
    return skip_ws(p + 1, end) == end && have_value ? 0 : -1;
}

long brightness_to_raw(long value, int percent, long max) {
    if (max <= 0 || value < 0 || value > (percent ? 100 : max)) return -1;
    return percent ? (value * max + 50) / 100 : value;
}

size_t tail_start(const char *buf, size_t len, unsigned n) {
    if (n == 0) return len;
    size_t i = len;
    if (i > 0 && buf[i - 1] == '\n') i--;
    for (; i > 0; i--)
        if (buf[i - 1] == '\n' && --n == 0) return i;
    return 0;
}

void json_lines(struct sbuf *sb, const char *buf, size_t len, int strip_level) {
    sb_puts(sb, "[");
    char line[1024];
    size_t pos = 0;
    int first = 1;
    while (pos < len) {
        const char *nl = memchr(buf + pos, '\n', len - pos);
        size_t end = nl ? (size_t)(nl - buf) : len, i = pos, k = 0;
        if (strip_level && i < end && buf[i] == '<') {
            size_t j = i + 1;
            while (j < end && is_digit(buf[j])) j++;
            if (j > i + 1 && j < end && buf[j] == '>') i = j + 1;
        }
        for (; i < end && k < sizeof line - 1; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (c == '\r') continue;
            line[k++] = (c == '\t' || (c >= 0x20 && c < 0x7f)) ? (char)c : '?';
        }
        line[k] = 0;
        if (!first) sb_puts(sb, ",");
        sb_json_str(sb, line);
        first = 0;
        pos = end + 1;
    }
    sb_puts(sb, "]");
}

int lines_query(const char *query, unsigned def, unsigned max, unsigned *out) {
    *out = def;
    for (const char *p = query; p && *p;) {
        size_t n = strcspn(p, "&");
        if (n >= 6 && !strncmp(p, "lines=", 6)) {
            const char *v = p + 6;
            size_t vlen = n - 6;
            if (vlen == 0 || vlen > 9 || strspn(v, "0123456789") < vlen) return -1;
            unsigned long x = strtoul(v, NULL, 10);
            if (x < 1 || x > max) return -1;
            *out = (unsigned)x;
            return 0;
        }
        p += n;
        if (*p == '&') p++;
    }
    return 0;
}

int clock_plausible(time_t t) { return t >= (time_t)1704067200; /* 2024-01-01T00:00:00Z */ }
