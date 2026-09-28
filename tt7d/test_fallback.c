/* ABOUTME: Unit tests for the fallback state machine (fallback.c): when tt7d shows its own clock
 * ABOUTME: instead of the server's frame. Times are plain monotonic milliseconds handed in by the test. */
#include <string.h>

#include "fallback.h"
#include "test_common.h"

#define S 1000 /* one second in ms */

static void boot_without_frames_is_fallback(void) {
    struct fallback f;
    fallback_init(&f, 300, 5 * S);
    CHECK(f.active, "no frame since boot must show the fallback");
    CHECK(f.reason == FALLBACK_NO_FRAME, "reason %d", f.reason);
    CHECK(f.since_ms == 5 * S, "since %lld", (long long)f.since_ms);
    CHECK(!strcmp(fallback_reason_name(f.reason), "no_frame_since_boot"), "%s", fallback_reason_name(f.reason));
    CHECK(fallback_update(&f, 400 * S) == 0 && f.active, "still waiting, and not a new entry");
}

static void frame_leaves_fallback_then_timeout_returns(void) {
    struct fallback f;
    fallback_init(&f, 300, 0);
    fallback_frame(&f, 10 * S);
    CHECK(!f.active && f.reason == FALLBACK_NONE, "a frame must leave the fallback at once");
    CHECK(fallback_reason_name(f.reason) == NULL, "no reason while the server frame shows");
    CHECK(fallback_update(&f, 309 * S) == 0 && !f.active, "299 s old: keep the frame");
    CHECK(fallback_ms_until_timeout(&f, 309 * S) == 1 * S, "%lld", (long long)fallback_ms_until_timeout(&f, 309 * S));
    CHECK(fallback_update(&f, 310 * S) == 1, "300 s old: this call enters the fallback");
    CHECK(f.active && f.reason == FALLBACK_TIMEOUT && f.since_ms == 310 * S, "active %d reason %d", f.active,
          f.reason);
    CHECK(!strcmp(fallback_reason_name(f.reason), "server_timeout"), "%s", fallback_reason_name(f.reason));
    CHECK(fallback_update(&f, 311 * S) == 0, "entering is reported once");
    fallback_frame(&f, 400 * S);
    CHECK(!f.active, "the next frame leaves the fallback");
}

static void heartbeat_keeps_the_server_frame(void) {
    struct fallback f;
    fallback_init(&f, 300, 0);
    fallback_frame(&f, 0);
    fallback_heartbeat(&f, 200 * S);
    CHECK(fallback_update(&f, 450 * S) == 0 && !f.active, "heartbeat at 200 s: the frame stays until 500 s");
    CHECK(fallback_update(&f, 500 * S) == 1 && f.active, "and falls back at 500 s");
}

static void heartbeat_during_fallback_stays(void) {
    struct fallback f;
    fallback_init(&f, 300, 0);
    fallback_heartbeat(&f, 1 * S);
    CHECK(f.active && f.reason == FALLBACK_NO_FRAME, "a heartbeat alone has nothing to show");

    fallback_frame(&f, 2 * S);
    fallback_update(&f, 302 * S);
    CHECK(f.active && f.reason == FALLBACK_TIMEOUT, "timed out");
    fallback_heartbeat(&f, 303 * S);
    CHECK(f.active && f.reason == FALLBACK_TIMEOUT && f.since_ms == 302 * S, "heartbeat keeps the fallback as it was");
    fallback_frame(&f, 304 * S);
    CHECK(!f.active, "only a frame brings the server back");
}

static void timeout_zero_never_falls_back(void) {
    struct fallback f;
    fallback_init(&f, 0, 0);
    CHECK(!f.active, "0 disables the fallback, even at boot");
    CHECK(fallback_update(&f, 1000000 * (int64_t)S) == 0 && !f.active, "never");
    CHECK(fallback_ms_until_timeout(&f, 0) == -1, "no deadline when disabled");
    fallback_frame(&f, 5);
    CHECK(fallback_update(&f, 1000000 * (int64_t)S) == 0 && !f.active, "never, after a frame either");
}

static void no_deadline_while_active(void) {
    struct fallback f;
    fallback_init(&f, 60, 0);
    CHECK(fallback_ms_until_timeout(&f, 0) == -1, "already in fallback: nothing to wait for");
    fallback_frame(&f, 0);
    CHECK(fallback_ms_until_timeout(&f, 90 * S) == 0, "overdue: wake at once, not a negative wait");
}

static void json_member(void) {
    struct fallback f;
    struct sbuf sb;
    struct timespec wall = {.tv_sec = 1790622300, .tv_nsec = 0}; /* 2026-09-28T19:05:00Z */
    fallback_init(&f, 300, 1 * S);
    sb_init(&sb);
    fallback_json(&f, 61 * S + 250, &wall, 1, &sb);
    CHECK(sb.buf && !strcmp(sb.buf, "\"fallback\":{\"active\":true,\"reason\":\"no_frame_since_boot\",\"timeout_s\":300,"
                                    "\"since\":\"2026-09-28T19:03:59.750Z\"}"),
          "%s", sb.buf);
    sb_free(&sb);
    fallback_frame(&f, 70 * S);
    sb_init(&sb);
    fallback_json(&f, 80 * S, &wall, 1, &sb);
    CHECK(sb.buf && !strcmp(sb.buf, "\"fallback\":{\"active\":false,\"reason\":null,\"timeout_s\":300,\"since\":null}"),
          "%s", sb.buf);
    sb_free(&sb);
    sb_init(&sb);
    fallback_json(&f, 80 * S, &wall, 0, &sb);
    CHECK(sb.buf && !strstr(sb.buf, "since"), "no since in the signature form: %s", sb.buf);
    sb_free(&sb);
}

int main(void) {
    json_member();
    boot_without_frames_is_fallback();
    frame_leaves_fallback_then_timeout_returns();
    heartbeat_keeps_the_server_frame();
    heartbeat_during_fallback_stays();
    timeout_zero_never_falls_back();
    no_deadline_while_active();
    return test_finish("test_fallback");
}
