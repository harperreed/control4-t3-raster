/* ABOUTME: Is the wall clock right? Reads the NTP sync marker that tt7-ntp-hook writes, and picks the
 * ABOUTME: POSIX TZ string tt7d formats local time with (--tz, else <data-dir>/tz, else the default). */
#ifndef TT7D_TIMESYNC_H
#define TT7D_TIMESYNC_H

#include <stddef.h>
#include <time.h>

/* America/Chicago as a POSIX TZ string: musl reads TZ rules like this
 * directly, and the image ships no zoneinfo files. */
#define TIMESYNC_DEFAULT_TZ "CST6CDT,M3.2.0,M11.1.0"

/* Where tt7-ntp-hook (probe/tt7-ntp-hook.sh) writes the marker. /run is on
 * the ramdisk, so the marker never outlives the boot it was written in. */
#define TIMESYNC_DEFAULT_MARKER "/run/tt7/ntp-synced"

/* No clock before this is believable (2026-01-01T00:00:00Z): a guard for a
 * marker that somehow survived while the clock went back to 1970 or 2011. */
#define TIMESYNC_EARLIEST 1767225600

/* Parse the marker's contents: "synced <unix seconds> <ntpd action> ...".
 * Returns 0 with *synced_at set, or -1 if it is not a valid marker. */
int timesync_parse_marker(const char *text, time_t *synced_at);

/* 1 if the marker at `path` is valid and `now` is plausible, else 0.
 * *synced_at is set when it returns 1. */
int timesync_synced(const char *path, time_t now, time_t *synced_at);

/* 1 if s looks like a POSIX TZ string: up to 63 printable, non-space ASCII
 * characters, a standard-time name (letters, or "<...>") and then an offset
 * (digit or sign). That rules out ":file", paths and zone names such as
 * America/Chicago, which need zoneinfo files the image does not have. */
int timesync_tz_valid(const char *s);

/* The TZ to use: `flag` if given, else the first line of <data_dir>/tz, else
 * TIMESYNC_DEFAULT_TZ. Returns 0, or -1 with a reason in err (the value is
 * then the default, so the caller can warn and go on). */
int timesync_tz_choose(const char *flag, const char *data_dir, char *out, size_t n, char *err, size_t errlen);

/* setenv("TZ") + tzset(). */
void timesync_tz_apply(const char *tz);

#endif
