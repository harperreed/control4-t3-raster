/* ABOUTME: Clock trust and timezone for the fallback clock: parses the NTP sync marker written by
 * ABOUTME: tt7-ntp-hook, and chooses and applies the POSIX TZ string. */
#include "timesync.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int timesync_parse_marker(const char *text, time_t *synced_at) {
    char word[8], action[16];
    long long when;
    int used = 0;
    /* %lld accepts a sign, so check the digits first. */
    if (sscanf(text, "%7s %n", word, &used) != 1 || strcmp(word, "synced") != 0) return -1;
    const char *num = text + used;
    size_t digits = strspn(num, "0123456789");
    if (!digits || !isspace((unsigned char)num[digits])) return -1;
    if (sscanf(num, "%lld %15s", &when, action) != 2 || when <= 0) return -1;
    *synced_at = (time_t)when;
    return 0;
}

int timesync_synced(const char *path, time_t now, time_t *synced_at) {
    if (now < TIMESYNC_EARLIEST) return 0;
    char buf[256];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return timesync_parse_marker(buf, synced_at) == 0;
}

int timesync_tz_valid(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 63 || !(isalpha((unsigned char)s[0]) || s[0] == '<')) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] <= ' ' || s[i] > '~') return 0;
    /* The standard-time name, then its offset: "CST6...", "<+0530>-5:30".
     * This is what tells a zone name like America/Chicago apart. */
    size_t name = s[0] == '<' ? strcspn(s, ">") + 1 : strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
    return name < n && (isdigit((unsigned char)s[name]) || s[name] == '+' || s[name] == '-');
}

int timesync_tz_choose(const char *flag, const char *data_dir, char *out, size_t n, char *err, size_t errlen) {
    char line[128] = "", path[512];
    const char *src = "--tz";
    const char *v = flag;
    if (!v) {
        snprintf(path, sizeof path, "%s/tz", data_dir);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(line, sizeof line, f)) line[strcspn(line, "\r\n")] = 0;
            fclose(f);
            v = line;
            src = path;
        }
    }
    snprintf(out, n, "%s", TIMESYNC_DEFAULT_TZ);
    if (!v) return 0;
    if (!timesync_tz_valid(v)) {
        snprintf(err, errlen, "%s: '%s' is not a POSIX TZ string (e.g. %s); using the default", src, v,
                 TIMESYNC_DEFAULT_TZ);
        return -1;
    }
    snprintf(out, n, "%s", v);
    return 0;
}

void timesync_tz_apply(const char *tz) {
    setenv("TZ", tz, 1);
    tzset();
}
