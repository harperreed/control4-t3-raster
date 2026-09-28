/* ABOUTME: Unit tests for timesync.c: the NTP sync marker, the plausibility floor, and choosing the TZ
 * ABOUTME: from the flag, <data-dir>/tz or the default. Uses real files in a temp dir. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_common.h"
#include "timesync.h"

#define NOW 1790000000 /* 2026-09-21 */

static char dir[64];

static void write_text(const char *name, const char *text) {
    char path[128];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static void marker_parsing(void) {
    time_t t = 0;
    CHECK(timesync_parse_marker("synced 1790000000 step stratum=16 offset=1.5e9\n", &t) == 0 && t == 1790000000,
          "t %lld", (long long)t);
    CHECK(timesync_parse_marker("synced 1790000000 stratum", &t) == 0, "the rest of the line is informational");
    CHECK(timesync_parse_marker("synced 1790000000", &t) == -1, "the action is required");
    CHECK(timesync_parse_marker("", &t) == -1, "empty");
    CHECK(timesync_parse_marker("unsync 1790000000 unsync", &t) == -1, "only 'synced' counts");
    CHECK(timesync_parse_marker("synced soon step", &t) == -1, "not a number");
    CHECK(timesync_parse_marker("synced -5 step", &t) == -1, "negative");
    CHECK(timesync_parse_marker("synced 17x step", &t) == -1, "trailing junk in the number");
    CHECK(timesync_parse_marker("syncedx 1790000000 step", &t) == -1, "wrong word");
}

static void synced_from_file(void) {
    char path[128];
    time_t t = 0;
    snprintf(path, sizeof path, "%s/ntp-synced", dir);
    CHECK(timesync_synced(path, NOW, &t) == 0, "no marker: not synced");
    write_text("ntp-synced", "synced 1790000000 step stratum=16 offset=12.5\n");
    CHECK(timesync_synced(path, NOW, &t) == 1 && t == 1790000000, "marker present");
    CHECK(timesync_synced(path, 1293840000, &t) == 0, "a 2011 clock is never believed, marker or not");
    write_text("ntp-synced", "garbage\n");
    CHECK(timesync_synced(path, NOW, &t) == 0, "a bad marker is not synced");
    unlink(path);
}

static void tz_validation(void) {
    CHECK(timesync_tz_valid(TIMESYNC_DEFAULT_TZ), "default");
    CHECK(timesync_tz_valid("UTC0"), "UTC0");
    CHECK(timesync_tz_valid("<+0530>-5:30"), "quoted name");
    CHECK(!timesync_tz_valid(""), "empty");
    CHECK(!timesync_tz_valid(":America/Chicago"), "zoneinfo form");
    CHECK(!timesync_tz_valid("/etc/localtime"), "path");
    CHECK(!timesync_tz_valid("CST6 CDT"), "space");
    CHECK(!timesync_tz_valid("0CST"), "starts with a digit");
}

static void tz_choice(void) {
    char tz[64], err[128];
    CHECK(timesync_tz_choose(NULL, dir, tz, sizeof tz, err, sizeof err) == 0 && !strcmp(tz, TIMESYNC_DEFAULT_TZ),
          "no file: default, got %s", tz);
    write_text("tz", "EST5EDT,M3.2.0,M11.1.0\n# comment lines after the first are ignored\n");
    CHECK(timesync_tz_choose(NULL, dir, tz, sizeof tz, err, sizeof err) == 0 && !strcmp(tz, "EST5EDT,M3.2.0,M11.1.0"),
          "file: %s", tz);
    CHECK(timesync_tz_choose("UTC0", dir, tz, sizeof tz, err, sizeof err) == 0 && !strcmp(tz, "UTC0"),
          "the flag beats the file: %s", tz);
    write_text("tz", "America/Chicago\n");
    CHECK(timesync_tz_choose(NULL, dir, tz, sizeof tz, err, sizeof err) == -1 && !strcmp(tz, TIMESYNC_DEFAULT_TZ),
          "a zone name is not a POSIX TZ string: default, got %s", tz);
    CHECK(strstr(err, "America/Chicago") != NULL, "the error names the bad value: %s", err);
    CHECK(timesync_tz_choose(":bad", dir, tz, sizeof tz, err, sizeof err) == -1 && !strcmp(tz, TIMESYNC_DEFAULT_TZ),
          "bad flag: default");
}

int main(void) {
    snprintf(dir, sizeof dir, "/tmp/tt7d-test-timesync-XXXXXX");
    if (!mkdtemp(dir)) return 1;
    marker_parsing();
    synced_from_file();
    tz_validation();
    tz_choice();
    char path[128];
    snprintf(path, sizeof path, "%s/tz", dir);
    unlink(path);
    rmdir(dir);
    return test_finish("test_timesync");
}
