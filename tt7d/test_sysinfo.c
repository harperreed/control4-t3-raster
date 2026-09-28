/* ABOUTME: Host unit tests for sysinfo.c against test/fixtures/sysfs-tt7 (a copy of the real panel's sysfs).
 * ABOUTME: Also covers modalias parsing with the panel's own modalias strings. */
#include <string.h>

#include "sysinfo.h"
#include "test_common.h"

#ifndef FIXTURE
#define FIXTURE "tt7d/test/fixtures/sysfs-tt7"
#endif

/* From the TT7's /sys/class/input/input{0,1}/modalias. */
#define KEYPAD "input:b0019v0001p0001e0100-e0,1,k72,73,74,8F,ramlsfw"
#define GSL "input:b0018v0000p0000e0000-e0,1,3,14,kra2F,30,32,35,36,39,mlsfw"

static void test_modalias(void) {
    CHECK(modalias_has(KEYPAD, 'e', 1), "keypad EV_KEY");
    CHECK(!modalias_has(KEYPAD, 'e', 3), "keypad has no EV_ABS");
    CHECK(modalias_has(KEYPAD, 'k', 0x72) && modalias_has(KEYPAD, 'k', 0x8F), "keypad keys");
    CHECK(!modalias_has(KEYPAD, 'k', 0x71), "keypad lacks 0x71");
    CHECK(!modalias_has(KEYPAD, 'a', 0x35), "keypad has no MT axes");
    CHECK(modalias_has(GSL, 'e', 3), "gsl EV_ABS");
    CHECK(modalias_has(GSL, 'a', 0x35) && modalias_has(GSL, 'a', 0x36), "gsl ABS_MT_POSITION_X/Y");
    CHECK(modalias_has(GSL, 'a', 0x2F), "gsl first abs code after empty k and r sections");
    CHECK(!modalias_has(GSL, 'k', 0x2F), "0x2F is an abs code, not a key");
    CHECK(modalias_has(GSL, 'e', 0x14), "0x14 = EV_REP in the e section");
    CHECK(!modalias_has("", 'e', 0) && !modalias_has("garbage", 'e', 0), "no dash, no match");
}

static void expect_in(const struct sbuf *sb, const char *needle) {
    CHECK(sb->buf && strstr(sb->buf, needle) != NULL, "missing %s in %s", needle, sb->buf ? sb->buf : "(nil)");
}

static void expect_out(const struct sbuf *sb, const char *needle) {
    CHECK(sb->buf && strstr(sb->buf, needle) == NULL, "unexpected %s in %s", needle, sb->buf ? sb->buf : "(nil)");
}

static void test_capabilities(void) {
    struct sbuf sb;
    sb_init(&sb);
    sysinfo_capabilities(&sb, FIXTURE);
    expect_in(&sb, "\"touch\":{\"available\":true,\"device\":\"gslX680\"}");
    expect_in(&sb, "\"buttons\":{\"available\":true,\"devices\":[\"rk29-keypad\"]}");
    expect_in(&sb, "\"backlight\":{\"available\":true,\"device\":\"rk28_bl\",\"max_brightness\":255}");
    expect_in(&sb, "\"battery\":{\"available\":true,\"device\":\"battery\"}");
    expect_in(&sb, "\"external_power\":{\"available\":true,\"device\":\"ac\"}");
    expect_in(&sb, "\"dock_detection\":{\"available\":null}");
    expect_in(&sb, "\"wifi\":{\"available\":true,\"interface\":\"wlan0\"}");
    expect_in(&sb, "\"ethernet\":{\"available\":false,\"interface\":null}");
    expect_in(&sb, "\"usb_network\":{\"available\":true,\"interface\":\"rndis0\"}");
    expect_in(&sb, "\"audio_output\":{\"available\":true,\"devices\":[\"pcmC0D0p\",\"pcmC0D1p\"]}");
    expect_in(&sb, "\"audio_input\":{\"available\":true,\"devices\":[\"pcmC0D0c\",\"pcmC0D1c\"]}");
    expect_out(&sb, "\"camera\""); /* camera.c reports it */
    sb_free(&sb);

    /* A root with nothing in it: everything unavailable, nothing invented. */
    sb_init(&sb);
    sysinfo_capabilities(&sb, "/nonexistent-sysfs-root");
    expect_in(&sb, "\"touch\":{\"available\":false,\"device\":null}");
    expect_in(&sb, "\"buttons\":{\"available\":false,\"devices\":[]}");
    expect_in(&sb, "\"backlight\":{\"available\":false,\"device\":null,\"max_brightness\":null}");
    expect_in(&sb, "\"battery\":{\"available\":false,\"device\":null}");
    expect_in(&sb, "\"wifi\":{\"available\":false,\"interface\":null}");
    expect_out(&sb, "\"camera\"");
    sb_free(&sb);
}

static void test_power(void) {
    struct sbuf sb;
    sb_init(&sb);
    sysinfo_power(&sb, FIXTURE);
    expect_in(&sb, "\"source\":\"battery\"");
    expect_in(&sb, "\"battery_percent\":{\"value\":82,\"unit\":\"percent\",\"available\":true,\"estimate\":true}");
    expect_in(&sb, "\"battery_voltage\":{\"value\":3.839,\"unit\":\"volt\",\"available\":true}");
    expect_in(&sb, "\"charging\":false");
    expect_in(&sb, "\"battery_status\":\"Not charging\"");
    expect_in(&sb, "\"external_power_online\":false");
    sb_free(&sb);

    sb_init(&sb);
    sysinfo_power(&sb, "/nonexistent-sysfs-root");
    expect_in(&sb, "\"source\":null");
    expect_in(&sb, "\"battery_percent\":{\"value\":null,\"unit\":\"percent\",\"available\":false,\"estimate\":true}");
    expect_in(&sb, "\"charging\":null");
    expect_in(&sb, "\"external_power_online\":null");
    sb_free(&sb);
}

static void test_network(void) {
    struct sbuf sb;
    sb_init(&sb);
    sysinfo_network(&sb, FIXTURE);
    expect_in(&sb, "\"interfaces\":{");
    expect_in(&sb, "\"rndis0\":{\"operstate\":\"up\",\"ipv4\":");
    expect_in(&sb, "\"wlan0\":{\"operstate\":\"down\",\"ipv4\":");
    CHECK(strstr(sb.buf, "\"lo\"") == NULL, "loopback is left out: %s", sb.buf);
    sb_free(&sb);
}

static void test_backlight(void) {
    int on, pct;
    sysinfo_backlight(FIXTURE, &on, &pct);
    CHECK(on == 1 && pct == 50, "on %d pct %d (127/255 rounds to 50)", on, pct);
    sysinfo_backlight("/nonexistent-sysfs-root", &on, &pct);
    CHECK(on == -1 && pct == -1, "unknown: on %d pct %d", on, pct);
}

static void test_values(void) {
    struct sysinfo_values v;
    sysinfo_read(FIXTURE, &v);
    CHECK(v.has_battery && v.battery_percent == 82 && v.charging == 0 && v.external_power == 0,
          "power: battery %d %d charging %d external %d", v.has_battery, v.battery_percent, v.charging,
          v.external_power);
    CHECK(v.has_backlight && v.backlight_max == 255 && v.brightness_percent == 50 && v.display_on == 1,
          "backlight: %d max %d pct %d on %d", v.has_backlight, v.backlight_max, v.brightness_percent, v.display_on);
    CHECK(v.has_wifi && !v.has_ethernet && !v.ethernet_ip[0], "network: wifi %d eth %d", v.has_wifi, v.has_ethernet);

    sysinfo_read("/nonexistent-sysfs-root", &v);
    CHECK(!v.has_battery && v.battery_percent == -1 && v.charging == -1 && v.external_power == -1 &&
              !v.has_backlight && v.backlight_max == -1 && v.brightness_percent == -1 && !v.has_wifi,
          "nothing present");
}

int main(void) {
    test_modalias();
    test_capabilities();
    test_power();
    test_network();
    test_backlight();
    test_values();
    return test_finish("test_sysinfo");
}
