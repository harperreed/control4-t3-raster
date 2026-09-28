/* ABOUTME: Pure helpers behind the control panel endpoints: brightness body parsing and range rules,
 * ABOUTME: log tails as JSON line arrays, the ?lines= query, and whether the wall clock looks set. */
#ifndef TT7D_CONTROL_H
#define TT7D_CONTROL_H

#include <stddef.h>
#include <time.h>

#include "json.h"

/* Parse a PUT /api/v1/display/brightness body: a JSON object with an integer
 * "value" and an optional "unit" of "raw" (the default: the backlight's own
 * 0..max_brightness scale) or "percent" (0..100). Nothing else is allowed:
 * no other members, no string escapes, no fractions. Returns 0 with *value
 * and *percent (1 for percent) set, or -1 if the body is malformed. */
int brightness_parse(const char *body, size_t len, long *value, int *percent);

/* The raw backlight level for a parsed value, or -1 if it is out of range
 * (raw: 0..max; percent: 0..100) or max is unknown (<= 0). Percent rounds to
 * the nearest raw level. */
long brightness_to_raw(long value, int percent, long max);

/* Offset in buf[0..len) where its last n lines start (a final newline does
 * not start an empty line). 0 if there are n lines or fewer. */
size_t tail_start(const char *buf, size_t len, unsigned n);

/* Append buf[0..len) as a JSON array of lines. \r is dropped; tabs are kept;
 * other control bytes and every byte >= 0x80 become '?', so the output is
 * always valid JSON and ASCII. With strip_level, a leading "<N>" (klogctl's
 * log level) is dropped from each line. */
void json_lines(struct sbuf *sb, const char *buf, size_t len, int strip_level);

/* The "lines" member of a query string: *out = def if absent; 0, or -1 if
 * present but not an integer in 1..max. */
int lines_query(const char *query, unsigned def, unsigned max, unsigned *out);

/* 1 if t is on or after 2024-01-01 UTC. Earlier means nothing has set the
 * clock since boot (the panel has no time sync yet). */
int clock_plausible(time_t t);

#endif
