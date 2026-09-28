/* ABOUTME: Host unit tests for tt7d's MQTT pieces: packet bytes, remaining-length boundaries,
 * ABOUTME: topic building, mqtt.conf parsing, JSON config bodies, and command payload parsing. */
#include <stdlib.h>
#include <string.h>

#include "mqtt.h"
#include "mqtt_config.h"
#include "mqtt_packet.h"
#include "test_common.h"

static int bytes_eq(const struct sbuf *sb, const uint8_t *want, size_t n) {
    return sb->len == n && memcmp(sb->buf, want, n) == 0;
}

static void dump(const char *what, const struct sbuf *sb) {
    fprintf(stderr, "  %s (%zu bytes):", what, sb->len);
    for (size_t i = 0; i < sb->len; i++) fprintf(stderr, " %02x", (uint8_t)sb->buf[i]);
    fputc('\n', stderr);
}

static void expect_remaining(size_t n, const uint8_t *want, int want_len) {
    uint8_t out[4];
    int got = mqtt_encode_remaining(n, out);
    CHECK(got == want_len && (want_len < 0 || memcmp(out, want, (size_t)want_len) == 0), "encode %zu: got %d bytes",
          n, got);
    if (want_len < 0) return;
    size_t value = 0, used = 0;
    CHECK(mqtt_decode_remaining(want, (size_t)want_len, &value, &used) == 1 && value == n &&
              used == (size_t)want_len,
          "decode %zu: value %zu used %zu", n, value, used);
    /* One byte short is "incomplete", never a wrong value. */
    if (want_len > 1) CHECK(mqtt_decode_remaining(want, (size_t)want_len - 1, &value, &used) == 0, "short %zu", n);
}

static void test_remaining_length(void) {
    expect_remaining(0, (const uint8_t[]){0x00}, 1);
    expect_remaining(127, (const uint8_t[]){0x7f}, 1);
    expect_remaining(128, (const uint8_t[]){0x80, 0x01}, 2);
    expect_remaining(16383, (const uint8_t[]){0xff, 0x7f}, 2);
    expect_remaining(16384, (const uint8_t[]){0x80, 0x80, 0x01}, 3);
    expect_remaining(2097151, (const uint8_t[]){0xff, 0xff, 0x7f}, 3);
    expect_remaining(2097152, (const uint8_t[]){0x80, 0x80, 0x80, 0x01}, 4);
    expect_remaining(268435455, (const uint8_t[]){0xff, 0xff, 0xff, 0x7f}, 4);
    expect_remaining(268435456, NULL, -1);

    size_t value, used;
    const uint8_t five[] = {0xff, 0xff, 0xff, 0xff, 0x01};
    CHECK(mqtt_decode_remaining(five, sizeof five, &value, &used) == -1, "a 5th length byte is malformed");
    CHECK(mqtt_decode_remaining(five, 0, &value, &used) == 0, "no bytes yet is incomplete");
}

static void test_connect(void) {
    struct sbuf sb;
    sb_init(&sb);
    struct mqtt_connect_opts o = {.client_id = "tt7-abc123", .keepalive_s = 60};
    CHECK(mqtt_encode_connect(&sb, &o) == 0, "encode plain connect");
    const uint8_t plain[] = {0x10, 0x16, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, 0x00, 0x3c,
                             0x00, 0x0a, 't', 't', '7', '-', 'a', 'b', 'c', '1', '2', '3'};
    CHECK(bytes_eq(&sb, plain, sizeof plain), "CONNECT, clean session, no auth, no will");
    if (!bytes_eq(&sb, plain, sizeof plain)) dump("got", &sb);

    sb.len = 0;
    o = (struct mqtt_connect_opts){.client_id = "tt7-abc123", .keepalive_s = 60, .username = "u", .password = "p",
                                   .will_topic = "t/a", .will_payload = "offline", .will_retain = 1};
    CHECK(mqtt_encode_connect(&sb, &o) == 0, "encode full connect");
    const uint8_t full[] = {0x10, 0x2a, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0xe6, 0x00, 0x3c,
                            0x00, 0x0a, 't', 't', '7', '-', 'a', 'b', 'c', '1', '2', '3',
                            0x00, 0x03, 't', '/', 'a',
                            0x00, 0x07, 'o', 'f', 'f', 'l', 'i', 'n', 'e',
                            0x00, 0x01, 'u',
                            0x00, 0x01, 'p'};
    CHECK(bytes_eq(&sb, full, sizeof full), "CONNECT with username, password and a retained will");
    if (!bytes_eq(&sb, full, sizeof full)) dump("got", &sb);

    /* MQTT 3.1.1 [MQTT-3.1.2-22]: no password flag without a username. */
    sb.len = 0;
    o = (struct mqtt_connect_opts){.client_id = "c", .keepalive_s = 10, .password = "p"};
    CHECK(mqtt_encode_connect(&sb, &o) == 0 && sb.len > 9 && (uint8_t)sb.buf[9] == 0x02,
          "a password without a username is left out");

    /* A will without retain: flag 0x04 only. */
    sb.len = 0;
    o = (struct mqtt_connect_opts){.client_id = "c", .keepalive_s = 10, .will_topic = "w", .will_payload = ""};
    CHECK(mqtt_encode_connect(&sb, &o) == 0 && (uint8_t)sb.buf[9] == 0x06, "will, not retained");
    sb_free(&sb);
}

static void test_publish_subscribe(void) {
    struct sbuf sb;
    sb_init(&sb);
    CHECK(mqtt_encode_publish(&sb, "a/b", "hi", 2, 1) == 0, "publish");
    const uint8_t pub[] = {0x31, 0x07, 0x00, 0x03, 'a', '/', 'b', 'h', 'i'};
    CHECK(bytes_eq(&sb, pub, sizeof pub), "PUBLISH QoS 0 retained");

    sb.len = 0;
    CHECK(mqtt_encode_publish(&sb, "a/b", "", 0, 0) == 0, "empty publish");
    const uint8_t empty[] = {0x30, 0x05, 0x00, 0x03, 'a', '/', 'b'};
    CHECK(bytes_eq(&sb, empty, sizeof empty), "PUBLISH with an empty payload (HA discovery removal)");

    /* A body of exactly 128 bytes needs the two-byte length. */
    char payload[125];
    memset(payload, 'x', sizeof payload);
    sb.len = 0;
    CHECK(mqtt_encode_publish(&sb, "t", payload, sizeof payload, 0) == 0, "publish 128");
    CHECK(sb.len == 3 + 128 && (uint8_t)sb.buf[1] == 0x80 && (uint8_t)sb.buf[2] == 0x01, "128-byte body header");

    /* And 16384 bytes, the first three-byte length. */
    char *big = malloc(16381);
    memset(big, 'y', 16381);
    sb.len = 0;
    CHECK(mqtt_encode_publish(&sb, "t", big, 16381, 0) == 0, "publish 16384");
    CHECK(sb.len == 4 + 16384 && (uint8_t)sb.buf[1] == 0x80 && (uint8_t)sb.buf[2] == 0x80 &&
              (uint8_t)sb.buf[3] == 0x01,
          "16384-byte body header");
    struct mqtt_header h;
    CHECK(mqtt_parse_header((const uint8_t *)sb.buf, sb.len, &h) == 1 && h.type == MQTT_PUBLISH &&
              h.header_len == 4 && h.body_len == 16384,
          "parse the header back");
    struct mqtt_publish p;
    CHECK(mqtt_parse_publish(h.flags, (const uint8_t *)sb.buf + h.header_len, h.body_len, &p) == 0 &&
              p.topic_len == 1 && p.topic[0] == 't' && p.payload_len == 16381 && p.qos == 0 && !p.retain,
          "parse the publish back");
    free(big);

    sb.len = 0;
    const char *filters[] = {"a/+"};
    CHECK(mqtt_encode_subscribe(&sb, 1, filters, 1) == 0, "subscribe");
    const uint8_t sub[] = {0x82, 0x08, 0x00, 0x01, 0x00, 0x03, 'a', '/', '+', 0x00};
    CHECK(bytes_eq(&sb, sub, sizeof sub), "SUBSCRIBE (reserved flags 0010, QoS 0)");

    sb.len = 0;
    mqtt_encode_pingreq(&sb);
    mqtt_encode_disconnect(&sb);
    mqtt_encode_puback(&sb, 0x1234);
    const uint8_t ctl[] = {0xc0, 0x00, 0xe0, 0x00, 0x40, 0x02, 0x12, 0x34};
    CHECK(bytes_eq(&sb, ctl, sizeof ctl), "PINGREQ, DISCONNECT, PUBACK");
    sb_free(&sb);
}

static void test_parse_incoming(void) {
    struct mqtt_header h;
    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x05};
    CHECK(mqtt_parse_header(connack, sizeof connack, &h) == 1 && h.type == MQTT_CONNACK && h.body_len == 2,
          "connack header");
    CHECK(mqtt_parse_connack(connack + 2, 2) == 5, "connack refused: not authorized");
    CHECK(mqtt_parse_connack(connack + 2, 1) == -1, "short connack");
    CHECK(mqtt_parse_header(connack, 1, &h) == 0, "header needs its length byte");

    /* QoS 1 retained PUBLISH with packet id 7. */
    const uint8_t pub1[] = {0x33, 0x08, 0x00, 0x02, 'c', '/', 0x00, 0x07, 'o', 'n'};
    CHECK(mqtt_parse_header(pub1, sizeof pub1, &h) == 1 && h.flags == 3, "qos1 header");
    struct mqtt_publish p;
    CHECK(mqtt_parse_publish(h.flags, pub1 + 2, h.body_len, &p) == 0 && p.qos == 1 && p.retain &&
              p.packet_id == 7 && p.payload_len == 2 && memcmp(p.payload, "on", 2) == 0,
          "qos1 publish fields");
    const uint8_t bad_topic[] = {0x00, 0x09, 'a'};
    CHECK(mqtt_parse_publish(0, bad_topic, sizeof bad_topic, &p) == -1, "topic longer than the packet");
    CHECK(mqtt_parse_publish(6, pub1 + 2, 8, &p) == -1, "QoS 3 is malformed");
}

static void test_config_file(void) {
    struct mqtt_config c;
    char err[200];
    mqtt_config_defaults(&c, "/data/tt7/tt7d");
    CHECK(c.enabled && !c.host[0] && c.port == 1883 && !strcmp(c.prefix, "tt7") && !c.client_id[0] &&
              c.keepalive == 60 && c.telemetry_interval == 10 && c.ha_discovery && !c.allow_reboot_cmd,
          "defaults");
    CHECK(!strcmp(c.password_file, "/data/tt7/tt7d/mqtt-password"), "default password file %s", c.password_file);

    const char *text = "# broker\n"
                       "\n"
                       "host=192.168.23.123\n"
                       "  port = 1884  \n"
                       "username=tt7\r\n"
                       "prefix=home/tt7\n"
                       "ha_discovery=false\n"
                       "allow_reboot_cmd=true\n"
                       "telemetry_interval=30";
    CHECK(mqtt_config_parse(&c, text, err, sizeof err) == 0, "parse: %s", err);
    CHECK(!strcmp(c.host, "192.168.23.123") && c.port == 1884 && !strcmp(c.username, "tt7") &&
              !strcmp(c.prefix, "home/tt7") && !c.ha_discovery && c.allow_reboot_cmd && c.telemetry_interval == 30,
          "parsed values");

    /* Round trip: what format writes, parse reads back identically. */
    struct sbuf sb;
    sb_init(&sb);
    mqtt_config_format(&c, &sb);
    CHECK(!strstr(sb.buf, "password="), "mqtt.conf never holds a password");
    struct mqtt_config back;
    mqtt_config_defaults(&back, "/elsewhere");
    CHECK(mqtt_config_parse(&back, sb.buf, err, sizeof err) == 0 && memcmp(&back, &c, sizeof c) == 0,
          "format/parse round trip: %s", err);
    sb_free(&sb);

    struct {
        const char *text, *want;
    } bad[] = {
        {"host=mqtt.example.com\n", "line 1: host"},
        {"# ok\nport=0\n", "line 2: port"},
        {"port=65536", "port"},
        {"port=12x", "port"},
        {"enabled=yes", "enabled"},
        {"prefix=tt7/#", "prefix"},
        {"prefix=/tt7", "prefix"},
        {"prefix=tt7/", "prefix"},
        {"prefix=a//b", "prefix"},
        {"prefix=", "prefix"},
        {"client_id=has space", "client_id"},
        {"keepalive=2", "keepalive"},
        {"telemetry_interval=0", "telemetry_interval"},
        {"password=hunter2", "line 1: unknown key"},
        {"nonsense", "line 1: expected KEY=VALUE"},
        {"username=a\x01b", "username"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        mqtt_config_defaults(&c, "/d");
        err[0] = 0;
        CHECK(mqtt_config_parse(&c, bad[i].text, err, sizeof err) == -1 && strstr(err, bad[i].want),
              "bad %s: err '%s'", bad[i].text, err);
    }
    mqtt_config_defaults(&c, "/d");
    CHECK(mqtt_config_set(&c, "host", "", err, sizeof err) == MQF_HOST && !c.host[0], "empty host = none");
    CHECK(mqtt_field_by_name("telemetry_interval") == MQF_TELEMETRY_INTERVAL && mqtt_field_by_name("x") == -1,
          "field names");
}

static void test_config_json(void) {
    struct mqtt_config c;
    char err[200], field[32], pw[256];
    int pw_change;
    mqtt_config_defaults(&c, "/d");
    const char *body = "{\"host\": \"10.0.0.2\", \"port\": 8883, \"enabled\": true, \"username\": \"tt\\u00e9\\\"x\","
                       " \"password\": \"s3cr\\\"et\", \"ha_discovery\": false, \"client_id\": null}";
    CHECK(mqtt_config_apply_json(&c, body, strlen(body), 0, pw, sizeof pw, &pw_change, field, err, sizeof err) == 0,
          "json: %s", err);
    CHECK(!strcmp(c.host, "10.0.0.2") && c.port == 8883 && !c.ha_discovery && !strcmp(c.username, "tt\xc3\xa9\"x"),
          "json values");
    CHECK(pw_change && !strcmp(pw, "s3cr\"et"), "password comes out separately");

    const char *null_pw = "{\"password\": null}";
    CHECK(mqtt_config_apply_json(&c, null_pw, strlen(null_pw), 0, pw, sizeof pw, &pw_change, field, err,
                                 sizeof err) == 0 && pw_change && pw[0] == 0,
          "null password = remove it");
    const char *none = "{}";
    CHECK(mqtt_config_apply_json(&c, none, 2, 0, pw, sizeof pw, &pw_change, field, err, sizeof err) == 0 &&
              !pw_change,
          "empty object changes nothing");
    (void)none;

    struct {
        const char *body, *field;
        int rc;
    } bad[] = {
        {"{\"port\": \"1883\"}", "port", -1},
        {"{\"port\": 1.5}", "port", -1},
        {"{\"enabled\": 1}", "enabled", -1},
        {"{\"host\": 5}", "host", -1},
        {"{\"host\": \"example.com\"}", "host", -1},
        {"{\"password_file\": \"/etc/shadow\"}", "password_file", -1},
        {"{\"bogus\": 1}", "bogus", -1},
        {"{\"password\": \"a\\nb\"}", "password", -1},
        {"{\"password\": \"a\\u0000b\"}", "password", -1},
        {"{\"host\": \"10.0.0.1\"", "", -1},
        {"[1]", "", -1},
        {"{\"host\": \"10.0.0.1\"} x", "", -1},
        {"{\"host\" \"10.0.0.1\"}", "", -1},
        {"{\"host\": \"10.0.0.3\"}", "host", -2},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        struct mqtt_config before = c;
        field[0] = 'X';
        field[1] = 0;
        int rc = mqtt_config_apply_json(&c, bad[i].body, strlen(bad[i].body),
                                        bad[i].rc == -2 ? 1u << MQF_HOST : 0, pw, sizeof pw,
                                        &pw_change, field, err, sizeof err);
        CHECK(rc == bad[i].rc && !strcmp(field, bad[i].field) && memcmp(&before, &c, sizeof c) == 0,
              "bad json %s: rc %d field '%s' err '%s'", bad[i].body, rc, field, err);
    }
    /* A failure after a good member leaves everything unchanged. */
    struct mqtt_config before = c;
    const char *half = "{\"port\": 1999, \"keepalive\": 1}";
    CHECK(mqtt_config_apply_json(&c, half, strlen(half), 0, pw, sizeof pw, &pw_change, field, err, sizeof err) == -1 &&
              memcmp(&before, &c, sizeof c) == 0 && !strcmp(field, "keepalive"),
          "all or nothing");
}

static void test_topics(void) {
    char t[64];
    CHECK(mqtt_topic(t, sizeof t, "tt7", "tt7-7f38a2", "availability") == 0 && !strcmp(t, "tt7/tt7-7f38a2/availability"),
          "availability topic %s", t);
    CHECK(mqtt_topic(t, sizeof t, "home/panels", "tt7-7f38a2", "cmd/+") == 0 &&
              !strcmp(t, "home/panels/tt7-7f38a2/cmd/+"),
          "nested prefix %s", t);
    CHECK(mqtt_topic(t, 10, "tt7", "tt7-7f38a2", "state") == -1, "truncation is an error");
    CHECK(mqtt_discovery_topic(t, sizeof t, "sensor", "tt7-7f38a2", "battery") == 0 &&
              !strcmp(t, "homeassistant/sensor/tt7-7f38a2/battery/config"),
          "discovery topic %s", t);

    const char *base = "tt7/tt7-7f38a2";
    CHECK(mqtt_command_of("tt7/tt7-7f38a2/cmd/brightness", base) == CMD_BRIGHTNESS, "brightness");
    CHECK(mqtt_command_of("tt7/tt7-7f38a2/cmd/wake", base) == CMD_WAKE, "wake");
    CHECK(mqtt_command_of("tt7/tt7-7f38a2/cmd/blank", base) == CMD_BLANK, "blank");
    CHECK(mqtt_command_of("tt7/tt7-7f38a2/cmd/reboot", base) == CMD_REBOOT, "reboot");
    CHECK(mqtt_command_of("tt7/tt7-7f38a2/cmd/reboot/x", base) == CMD_NONE, "deeper topic");
    CHECK(mqtt_command_of("tt7/tt7-000000/cmd/reboot", base) == CMD_NONE, "other device");
    CHECK(mqtt_command_of("tt7/tt7-7f38a2/cmd/format", base) == CMD_NONE, "unknown command");
    CHECK(mqtt_command_of("tt7/tt7-7f38a2", base) == CMD_NONE, "the base itself");
}

static int brightness(const char *payload, int max, int *pct) {
    return mqtt_parse_brightness((const uint8_t *)payload, strlen(payload), max, pct);
}

static void test_brightness_payload(void) {
    int pct = -1;
    CHECK(brightness("50%", 255, &pct) == 0 && pct == 50, "percent: %d", pct);
    CHECK(brightness(" 100% \n", 255, &pct) == 0 && pct == 100, "spaces: %d", pct);
    CHECK(brightness("0%", 255, &pct) == 0 && pct == 0, "zero percent");
    CHECK(brightness("255", 255, &pct) == 0 && pct == 100, "raw max: %d", pct);
    CHECK(brightness("128", 255, &pct) == 0 && pct == 50, "raw 128 rounds to 50: %d", pct);
    CHECK(brightness("0", 255, &pct) == 0 && pct == 0, "raw 0");
    CHECK(brightness("1", 255, &pct) == 0 && pct == 0, "raw 1 rounds down: %d", pct);
    CHECK(brightness("256", 255, &pct) == -1, "raw above max");
    CHECK(brightness("101%", 255, &pct) == -1, "percent above 100");
    CHECK(brightness("-1", 255, &pct) == -1, "negative");
    CHECK(brightness("", 255, &pct) == -1, "empty");
    CHECK(brightness("50%%", 255, &pct) == -1, "double percent");
    CHECK(brightness("5 0", 255, &pct) == -1, "inner space");
    CHECK(brightness("0x10", 255, &pct) == -1, "hex");
    CHECK(brightness("12.5%", 255, &pct) == -1, "fraction");
    CHECK(brightness("99999999999999999999", 255, &pct) == -1, "overflow");
    CHECK(brightness("200", -1, &pct) == -1, "raw needs a known max");
    CHECK(brightness("20%", -1, &pct) == 0 && pct == 20, "percent works without a max");
    CHECK(mqtt_parse_brightness((const uint8_t *)"5\0" "0", 3, 255, &pct) == -1, "embedded NUL");
}

int main(void) {
    test_remaining_length();
    test_connect();
    test_publish_subscribe();
    test_parse_incoming();
    test_config_file();
    test_config_json();
    test_topics();
    test_brightness_payload();
    return test_finish("test_mqtt");
}
