/* ABOUTME: Growable string buffer plus JSON string escaping and ISO-8601 UTC times for tt7d.
 * ABOUTME: See json.h; an allocation failure latches `oom` so callers check once at the end. */
#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(struct sbuf *sb) { memset(sb, 0, sizeof *sb); }

void sb_free(struct sbuf *sb) {
    free(sb->buf);
    sb_init(sb);
}

/* Make room for n more bytes plus the NUL. */
static int reserve(struct sbuf *sb, size_t n) {
    if (sb->oom) return -1;
    if (sb->len + n + 1 <= sb->cap) return 0;
    size_t cap = sb->cap ? sb->cap : 256;
    while (cap < sb->len + n + 1) cap *= 2;
    char *p = realloc(sb->buf, cap);
    if (!p) {
        sb->oom = 1;
        return -1;
    }
    sb->buf = p;
    sb->cap = cap;
    return 0;
}

void sb_add(struct sbuf *sb, const void *data, size_t n) {
    if (reserve(sb, n) != 0) return;
    memcpy(sb->buf + sb->len, data, n);
    sb->len += n;
    sb->buf[sb->len] = 0;
}

void sb_puts(struct sbuf *sb, const char *s) { sb_add(sb, s, strlen(s)); }

void sb_printf(struct sbuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || reserve(sb, (size_t)n) != 0) return;
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
}

void sb_json_str(struct sbuf *sb, const char *s) {
    if (!s) {
        sb_puts(sb, "null");
        return;
    }
    sb_add(sb, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': sb_add(sb, "\\\"", 2); break;
        case '\\': sb_add(sb, "\\\\", 2); break;
        case '\n': sb_add(sb, "\\n", 2); break;
        case '\r': sb_add(sb, "\\r", 2); break;
        case '\t': sb_add(sb, "\\t", 2); break;
        default:
            if (c < 0x20 || c == 0x7f) sb_printf(sb, "\\u%04x", c);
            else sb_add(sb, s, 1);
        }
    }
    sb_add(sb, "\"", 1);
}

void sb_json_time(struct sbuf *sb, const struct timespec *t) {
    struct tm tm;
    time_t sec = t->tv_sec;
    gmtime_r(&sec, &tm);
    sb_printf(sb, "\"%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ\"", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
              tm.tm_hour, tm.tm_min, tm.tm_sec, t->tv_nsec / 1000000);
}
