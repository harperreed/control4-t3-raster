/* ABOUTME: Host unit tests for control.c: the brightness body parser and range rules, log tails,
 * ABOUTME: the ?lines= query, JSON line arrays, and the "is the clock set" check. */
#include <string.h>
#include <time.h>

#include "control.h"
#include "test_common.h"

static int parse(const char *body, long *v, int *pct) { return brightness_parse(body, strlen(body), v, pct); }

static void test_brightness_parse(void) {
    long v = -99;
    int pct = -1;
    CHECK(parse("{\"value\": 200}", &v, &pct) == 0 && v == 200 && pct == 0, "raw by default: %ld %d", v, pct);
    CHECK(parse("{\"value\":72,\"unit\":\"percent\"}", &v, &pct) == 0 && v == 72 && pct == 1, "percent");
    CHECK(parse(" {\n \"unit\" : \"raw\" , \"value\" : 0 } \n", &v, &pct) == 0 && v == 0 && pct == 0, "raw, spaced");
    CHECK(parse("{\"value\":-5}", &v, &pct) == 0 && v == -5, "negative parses; the range check refuses it");

    const char *bad[] = {
        "",                               /* empty body */
        "{}",                             /* no value */
        "{\"unit\":\"percent\"}",         /* still no value */
        "{\"value\":\"50\"}",             /* a string, not a number */
        "{\"value\":50.5}",               /* not an integer */
        "{\"value\":5e1}",                /* exponent */
        "{\"value\":1234567890}",         /* too many digits */
        "{\"value\":50,\"unit\":\"nits\"}", /* unknown unit */
        "{\"value\":50,\"extra\":1}",     /* unknown member */
        "{\"value\":50,\"value\":60}",    /* duplicate */
        "{\"value\":50",                  /* unterminated */
        "{\"value\":50} x",               /* trailing junk */
        "[50]",                           /* not an object */
        "{\"value\":-}",                  /* sign without digits */
        "{\"val\\\"ue\":50}",             /* escapes are not supported */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(parse(bad[i], &v, &pct) == -1, "should refuse: %s", bad[i]);
}

static void test_brightness_to_raw(void) {
    /* 0 never reaches the backlight: rk28_bl treats brightness 0 as "full
     * bright", so a requested 0 becomes 1, the dimmest level. Blank turns it off. */
    CHECK(brightness_to_raw(0, 0, 255) == 1, "raw 0 clamps to 1, got %ld", brightness_to_raw(0, 0, 255));
    CHECK(brightness_to_raw(1, 0, 255) == 1, "raw 1");
    CHECK(brightness_to_raw(255, 0, 255) == 255, "raw max");
    CHECK(brightness_to_raw(256, 0, 255) == -1, "raw above max");
    CHECK(brightness_to_raw(-1, 0, 255) == -1, "raw below 0");
    CHECK(brightness_to_raw(100, 1, 255) == 255, "100 percent = max");
    CHECK(brightness_to_raw(50, 1, 255) == 128, "50 percent of 255 rounds to 128, got %ld", brightness_to_raw(50, 1, 255));
    CHECK(brightness_to_raw(0, 1, 255) == 1, "0 percent clamps to 1, got %ld", brightness_to_raw(0, 1, 255));
    CHECK(brightness_to_raw(1, 1, 255) == 3, "1 percent of 255 rounds to 3");
    CHECK(brightness_to_raw(101, 1, 255) == -1, "above 100 percent");
    CHECK(brightness_to_raw(-1, 1, 255) == -1, "below 0 percent");
    CHECK(brightness_to_raw(10, 0, 0) == -1 && brightness_to_raw(10, 0, -1) == -1, "unknown max refuses all");
}

static void test_tail_start(void) {
    const char *log = "one\ntwo\nthree\n";
    size_t n = strlen(log);
    CHECK(tail_start(log, n, 1) == 8, "last line starts at 8, got %zu", tail_start(log, n, 1));
    CHECK(tail_start(log, n, 2) == 4, "two lines");
    CHECK(tail_start(log, n, 3) == 0 && tail_start(log, n, 50) == 0, "all of it");
    CHECK(tail_start(log, n, 0) == n, "zero lines");
    CHECK(tail_start("a\nb", 3, 1) == 2, "no trailing newline");
    CHECK(tail_start("", 0, 5) == 0, "empty");
}

static void test_json_lines(void) {
    struct sbuf sb;
    sb_init(&sb);
    const char text[] = "plain\n\"quoted\" \\ tab\there\r\nbyte \x01 and \xc3\xa9\n";
    json_lines(&sb, text, sizeof text - 1, 0);
    CHECK(sb.buf && !strcmp(sb.buf, "[\"plain\",\"\\\"quoted\\\" \\\\ tab\\there\",\"byte ? and ??\"]"), "%s",
          sb.buf ? sb.buf : "(nil)");
    sb_free(&sb);

    sb_init(&sb);
    json_lines(&sb, "", 0, 0);
    CHECK(sb.buf && !strcmp(sb.buf, "[]"), "empty: %s", sb.buf ? sb.buf : "(nil)");
    sb_free(&sb);

    sb_init(&sb); /* klogctl text: "<level>" prefixes are dropped */
    const char klog[] = "<6>[    0.000000] Linux version 3.0.36+\n<4>warn\nno prefix\n";
    json_lines(&sb, klog, sizeof klog - 1, 1);
    CHECK(sb.buf && !strcmp(sb.buf, "[\"[    0.000000] Linux version 3.0.36+\",\"warn\",\"no prefix\"]"), "%s",
          sb.buf ? sb.buf : "(nil)");
    sb_free(&sb);
}

static void test_lines_query(void) {
    unsigned n = 0;
    CHECK(lines_query("", 200, 2000, &n) == 0 && n == 200, "default");
    CHECK(lines_query("lines=5", 200, 2000, &n) == 0 && n == 5, "five");
    CHECK(lines_query("x=1&lines=17&y", 200, 2000, &n) == 0 && n == 17, "among others");
    CHECK(lines_query("lines=2000", 200, 2000, &n) == 0 && n == 2000, "max");
    CHECK(lines_query("lines=2001", 200, 2000, &n) == -1, "above max");
    CHECK(lines_query("lines=0", 200, 2000, &n) == -1, "zero");
    CHECK(lines_query("lines=", 200, 2000, &n) == -1, "empty");
    CHECK(lines_query("lines=1x", 200, 2000, &n) == -1, "junk");
    CHECK(lines_query("lines=-3", 200, 2000, &n) == -1, "negative");
    CHECK(lines_query("mylines=3", 200, 2000, &n) == 0 && n == 200, "other key");
}

static void test_clock_plausible(void) {
    CHECK(!clock_plausible(0), "1970 is not set");
    CHECK(!clock_plausible(1700000000), "2023-11 is before 2024");
    CHECK(clock_plausible(1704067200), "2024-01-01T00:00:00Z counts");
    CHECK(clock_plausible(1790000000), "2026");
}

int main(void) {
    test_brightness_parse();
    test_brightness_to_raw();
    test_tail_start();
    test_json_lines();
    test_lines_query();
    test_clock_plausible();
    return test_finish("test_control");
}
