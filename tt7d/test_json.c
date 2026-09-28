/* ABOUTME: Host unit tests for json.c: string escaping, NULL as null, times, and buffer growth.
 * ABOUTME: Run by `make test-host`. */
#include <string.h>

#include "json.h"
#include "test_common.h"

static void expect_str(const char *in, const char *want) {
    struct sbuf sb;
    sb_init(&sb);
    sb_json_str(&sb, in);
    CHECK(sb.buf && strcmp(sb.buf, want) == 0, "escape: got %s want %s", sb.buf ? sb.buf : "(nil)", want);
    sb_free(&sb);
}

static void test_escaping(void) {
    expect_str("plain", "\"plain\"");
    expect_str("", "\"\"");
    expect_str(NULL, "null");
    expect_str("a\"b\\c", "\"a\\\"b\\\\c\"");
    expect_str("t\tn\nr\r", "\"t\\tn\\nr\\r\"");
    expect_str("\x01\x1f", "\"\\u0001\\u001f\"");
    expect_str("\x7f", "\"\\u007f\"");
    expect_str("caf\xc3\xa9", "\"caf\xc3\xa9\""); /* UTF-8 passes through */
    expect_str("</script>", "\"</script>\"");
}

static void test_time(void) {
    struct sbuf sb;
    sb_init(&sb);
    struct timespec t = {.tv_sec = 1790539411, .tv_nsec = 123456789}; /* 2026-09-27T20:03:31.123Z */
    sb_json_time(&sb, &t);
    CHECK(strcmp(sb.buf, "\"2026-09-27T20:03:31.123Z\"") == 0, "time %s", sb.buf);
    sb_free(&sb);
}

static void test_growth(void) {
    struct sbuf sb;
    sb_init(&sb);
    for (int i = 0; i < 10000; i++) sb_printf(&sb, "%d,", i % 10);
    CHECK(sb.len == 20000 && strlen(sb.buf) == 20000 && !sb.oom, "len %zu", sb.len);
    CHECK(memcmp(sb.buf, "0,1,2,", 6) == 0 && memcmp(sb.buf + 19994, "7,8,9,", 6) == 0, "content");
    sb_free(&sb);
    CHECK(sb.buf == NULL && sb.len == 0, "freed");
}

int main(void) {
    test_escaping();
    test_time();
    test_growth();
    return test_finish("test_json");
}
