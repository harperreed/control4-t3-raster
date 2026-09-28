/* ABOUTME: Growable string buffer plus the JSON pieces tt7d needs: escaped strings and ISO-8601 times.
 * ABOUTME: Writers only; tt7d never parses JSON. An allocation failure sets `oom` and stops appending. */
#ifndef TT7D_JSON_H
#define TT7D_JSON_H

#include <stddef.h>
#include <time.h>

struct sbuf {
    char *buf; /* always NUL-terminated once anything was added */
    size_t len;
    size_t cap;
    int oom;
};

void sb_init(struct sbuf *sb);
void sb_free(struct sbuf *sb);
void sb_add(struct sbuf *sb, const void *data, size_t n);
void sb_puts(struct sbuf *sb, const char *s);
void sb_printf(struct sbuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* A JSON string literal with quotes; NULL writes `null`. Bytes >= 0x80 pass
 * through unchanged (callers hand in UTF-8 or ASCII). */
void sb_json_str(struct sbuf *sb, const char *s);

/* A quoted UTC ISO-8601 time with milliseconds, e.g. "2026-09-27T20:03:31.123Z". */
void sb_json_time(struct sbuf *sb, const struct timespec *t);

#endif
