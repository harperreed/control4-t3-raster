/* ABOUTME: Host unit tests for tt7d's camera pieces: camera.conf and JSON settings, the worker message
 * ABOUTME: framing, and the presence wake / idle-blank decisions. */
#include <stdlib.h>
#include <string.h>

#include "camera_config.h"
#include "camera_proto.h"
#include "presence.h"
#include "test_common.h"

/* ---- settings ---- */

static void test_defaults(void) {
    struct camera_config c;
    camera_config_defaults(&c);
    CHECK(!c.enabled && !c.presence && !c.presence_wake, "camera, presence and wake are off by default");
    CHECK(c.presence_idle_blank_s == 0 && c.snapshot_interval_s == 0, "no idle blank, no periodic snapshots");
    CHECK(c.presence_interval_ms == 500 && c.presence_threshold == 8, "interval %d threshold %d",
          c.presence_interval_ms, c.presence_threshold);
    CHECK(c.snapshot_max_age_s == 2 && c.settle_frames == 29 && c.worker_timeout_s == 10, "cache %d settle %d timeout %d",
          c.snapshot_max_age_s, c.settle_frames, c.worker_timeout_s);
}

static void test_conf_text(void) {
    struct camera_config c;
    char err[200];
    camera_config_defaults(&c);
    const char *text = "# comment\n\ncamera = on\npresence=true\npresence_wake=off\npresence_idle_blank_s=120\n"
                       "snapshot_interval=30\nsettle_frames=0\n";
    CHECK(camera_config_parse(&c, text, err, sizeof err) == 0, "parse: %s", err);
    CHECK(c.enabled && c.presence && !c.presence_wake, "switches");
    CHECK(c.presence_idle_blank_s == 120 && c.snapshot_interval_s == 30 && c.settle_frames == 0, "numbers");

    struct sbuf sb;
    sb_init(&sb);
    camera_config_format(&c, &sb);
    CHECK(sb.buf && strstr(sb.buf, "\ncamera=on\n") && strstr(sb.buf, "\npresence_wake=off\n"), "format: %s", sb.buf);
    struct camera_config back;
    camera_config_defaults(&back);
    CHECK(camera_config_parse(&back, sb.buf, err, sizeof err) == 0 && memcmp(&back, &c, sizeof c) == 0,
          "format then parse gives the same settings: %s", err);
    sb_free(&sb);

    static const struct {
        const char *text, *want;
    } bad[] = {
        {"camera=yes\n", "line 1: camera must be on or off"},
        {"\nenabled=on\n", "line 2: unknown key 'enabled'"},
        {"presence_interval_ms=50\n", "presence_interval_ms must be a whole number from 100 to 10000"},
        {"presence_threshold=abc\n", "presence_threshold must be a whole number"},
        {"camera\n", "line 1: expected KEY=VALUE"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        camera_config_defaults(&c);
        CHECK(camera_config_parse(&c, bad[i].text, err, sizeof err) == -1 && strstr(err, bad[i].want),
              "%s: got '%s'", bad[i].text, err);
    }
}

static void test_json(void) {
    struct camera_config c;
    char field[32], err[200];
    camera_config_defaults(&c);
    const char *body = "{\"enabled\": true, \"presence\": true, \"presence_wake\": true, \"presence_idle_blank_s\": 60}";
    CHECK(camera_config_apply_json(&c, body, strlen(body), 0, field, err, sizeof err) == 0, "apply: %s", err);
    CHECK(c.enabled && c.presence && c.presence_wake && c.presence_idle_blank_s == 60, "applied");
    CHECK(camera_config_apply_json(&c, "{}", 2, 0, field, err, sizeof err) == 0, "an empty object changes nothing");

    static const struct {
        const char *body, *field, *want;
        int rc;
    } bad[] = {
        {"{\"presence\": \"on\"}", "presence", "presence must be a JSON boolean", -1},
        {"{\"camera\": true}", "camera", "unknown setting 'camera'", -1},
        {"{\"presence_interval_ms\": 1.5}", "presence_interval_ms", "must be a whole JSON number", -1},
        {"{\"worker_timeout_s\": 1}", "worker_timeout_s", "from 2 to 600", -1},
        {"{\"presence\": true, \"enabled\": false}", "enabled", "set by --camera", -2},
        {"[1]", "", "one flat JSON object", -1},
        {"{\"presence\": tru}", "presence", "malformed", -1},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        struct camera_config before = c;
        int rc = camera_config_apply_json(&c, bad[i].body, strlen(bad[i].body), 1u << CAMF_ENABLED, field, err,
                                          sizeof err);
        CHECK(rc == bad[i].rc && !strcmp(field, bad[i].field) && strstr(err, bad[i].want),
              "%s: rc %d field '%s' err '%s'", bad[i].body, rc, field, err);
        CHECK(memcmp(&before, &c, sizeof c) == 0, "%s: a refused body changes nothing", bad[i].body);
    }

    struct sbuf sb;
    sb_init(&sb);
    camera_config_json_members(&c, &sb);
    CHECK(sb.buf && strstr(sb.buf, "\"enabled\":true") && strstr(sb.buf, "\"presence_idle_blank_s\":60") &&
              !strstr(sb.buf, "\"camera\""),
          "json members: %s", sb.buf);
    sb_free(&sb);
}

/* ---- worker messages ---- */

static void test_proto(void) {
    uint8_t buf[64];
    struct camera_tick t = {.present = 1, .frame = 7, .score = 12.5f};
    camera_msg_header(buf, CMSG_TICK, sizeof t);
    memcpy(buf + CAMERA_MSG_HEADER, &t, sizeof t);
    size_t total = CAMERA_MSG_HEADER + sizeof t;
    memcpy(buf + total, "\x03\x00\x00\x00", 4); /* the start of the next header */

    uint32_t type, plen;
    const uint8_t *payload;
    for (size_t n = 0; n < total; n++)
        CHECK(camera_msg_parse(buf, n, &type, &payload, &plen) == 0, "%zu bytes is incomplete", n);
    CHECK(camera_msg_parse(buf, total + 4, &type, &payload, &plen) == (long)total, "a whole tick");
    struct camera_tick got;
    memcpy(&got, payload, sizeof got);
    CHECK(type == CMSG_TICK && plen == sizeof t && got.present == 1 && got.frame == 7 && got.score == 12.5f, "tick");

    camera_msg_header(buf, CMSG_JPEG, 3);
    memcpy(buf + CAMERA_MSG_HEADER, "abc", 3);
    CHECK(camera_msg_parse(buf, CAMERA_MSG_HEADER + 3, &type, &payload, &plen) == CAMERA_MSG_HEADER + 3 &&
              type == CMSG_JPEG && plen == 3 && !memcmp(payload, "abc", 3),
          "jpeg");
    camera_msg_header(buf, CMSG_SNAPSHOT, 0);
    CHECK(camera_msg_parse(buf, CAMERA_MSG_HEADER, &type, &payload, &plen) == CAMERA_MSG_HEADER && plen == 0,
          "an empty payload");

    camera_msg_header(buf, 99, 0);
    CHECK(camera_msg_parse(buf, CAMERA_MSG_HEADER, &type, &payload, &plen) == -1, "unknown type");
    camera_msg_header(buf, CMSG_JPEG, CAMERA_MSG_MAX_PAYLOAD + 1);
    CHECK(camera_msg_parse(buf, CAMERA_MSG_HEADER, &type, &payload, &plen) == -1, "too long, known from the header");
    camera_msg_header(buf, CMSG_TICK, 3);
    CHECK(camera_msg_parse(buf, CAMERA_MSG_HEADER + 3, &type, &payload, &plen) == -1, "a tick of the wrong size");
}

/* ---- presence and the display ---- */

static void test_presence_wake(void) {
    struct presence_policy p = {.wake = 1, .idle_blank_s = 0};
    struct presence_ctl s;
    presence_ctl_init(&s);
    CHECK(presence_update(&s, &p, 0, 1, 0) == PRESENCE_NOTHING, "nobody, blank: nothing");
    CHECK(presence_update(&s, &p, 1, 1, 100) == PRESENCE_WAKE, "arrival wakes a blank display");
    CHECK(presence_update(&s, &p, 1, 0, 600) == PRESENCE_NOTHING, "still present: nothing more");
    CHECK(presence_update(&s, &p, 0, 0, 1100) == PRESENCE_NOTHING, "leaving: nothing");
    CHECK(presence_update(&s, &p, 0, 0, 1000000) == PRESENCE_NOTHING, "idle blank 0 never blanks");
    CHECK(presence_next_ms(&s, &p, 1000000) == -1, "nothing pending");

    presence_ctl_init(&s);
    CHECK(presence_update(&s, &p, 1, 0, 0) == PRESENCE_NOTHING, "arrival on a lit display does nothing");
    p.wake = 0;
    presence_ctl_init(&s);
    CHECK(presence_update(&s, &p, 1, 1, 0) == PRESENCE_NOTHING, "presence_wake off: a blank display stays blank");
}

static void test_presence_idle_blank(void) {
    struct presence_policy p = {.wake = 1, .idle_blank_s = 60};
    struct presence_ctl s;
    presence_ctl_init(&s);
    CHECK(presence_update(&s, &p, 1, 1, 1000) == PRESENCE_WAKE, "wake");
    CHECK(presence_next_ms(&s, &p, 2000) == -1, "no blank pending while present");
    CHECK(presence_update(&s, &p, 0, 0, 5000) == PRESENCE_NOTHING, "left at 5 s");
    CHECK(presence_next_ms(&s, &p, 6000) == 59000, "due 60 s after leaving: %lld",
          (long long)presence_next_ms(&s, &p, 6000));
    CHECK(presence_update(&s, &p, 0, 0, 64999) == PRESENCE_NOTHING, "not yet at 59.999 s");
    CHECK(presence_update(&s, &p, 0, 0, 65000) == PRESENCE_BLANK, "blank at 60 s");
    CHECK(presence_update(&s, &p, 0, 1, 200000) == PRESENCE_NOTHING && presence_next_ms(&s, &p, 200000) == -1,
          "only once");

    /* Back before the idle time runs out: the countdown starts again. */
    presence_ctl_init(&s);
    presence_update(&s, &p, 1, 1, 0);
    presence_update(&s, &p, 0, 0, 1000);
    presence_update(&s, &p, 1, 0, 30000);
    presence_update(&s, &p, 0, 0, 40000);
    CHECK(presence_update(&s, &p, 0, 0, 61000) == PRESENCE_NOTHING, "the countdown restarted at 40 s");
    CHECK(presence_update(&s, &p, 0, 0, 100000) == PRESENCE_BLANK, "blank 60 s after the last departure");

    /* Someone else blanked it meanwhile: presence no longer owns it. */
    presence_ctl_init(&s);
    presence_update(&s, &p, 1, 1, 0);
    presence_update(&s, &p, 0, 0, 1000);
    CHECK(presence_update(&s, &p, 0, 1, 2000) == PRESENCE_NOTHING, "blanked by hand");
    CHECK(presence_update(&s, &p, 0, 0, 100000) == PRESENCE_NOTHING, "then woken by hand: not presence's to blank");

    /* A display that was lit before the arrival is never blanked. */
    presence_ctl_init(&s);
    presence_update(&s, &p, 1, 0, 0);
    presence_update(&s, &p, 0, 0, 1000);
    CHECK(presence_update(&s, &p, 0, 0, 100000) == PRESENCE_NOTHING, "lit before: left alone");

    /* Turning presence_wake off drops the pending blank. */
    presence_ctl_init(&s);
    presence_update(&s, &p, 1, 1, 0);
    presence_update(&s, &p, 0, 0, 1000);
    p.wake = 0;
    CHECK(presence_update(&s, &p, 0, 0, 100000) == PRESENCE_NOTHING && presence_next_ms(&s, &p, 100000) == -1,
          "wake off: no blank");
}

int main(void) {
    test_defaults();
    test_conf_text();
    test_json();
    test_proto();
    test_presence_wake();
    test_presence_idle_blank();
    return test_finish("test_camera");
}
