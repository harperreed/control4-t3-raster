/* ABOUTME: Host unit tests for hardware.c against the panel's sysfs and /proc fixtures.
 * ABOUTME: Checks the /api/v1/hardware members, the input role rule, and /proc/asound/cards parsing. */
#include <string.h>

#include "hardware.h"
#include "sysinfo.h"
#include "test_common.h"

#ifndef FIXTURE
#define FIXTURE "tt7d/test/fixtures/sysfs-tt7"
#endif
#define PROC_FIXTURE "tt7d/test/fixtures/proc-tt7"

#define KEYPAD "input:b0019v0001p0001e0100-e0,1,k72,73,74,8F,ramlsfw"
#define GSL "input:b0018v0000p0000e0000-e0,1,3,14,kra2F,30,32,35,36,39,mlsfw"

static void expect_in(const struct sbuf *sb, const char *needle) {
    CHECK(sb->buf && strstr(sb->buf, needle) != NULL, "missing %s in %s", needle, sb->buf ? sb->buf : "(nil)");
}

static void test_input_role(void) {
    CHECK(!strcmp(sysinfo_input_role("gslX680", GSL), "touchscreen"), "gsl");
    CHECK(!strcmp(sysinfo_input_role("some-touch", "input:b0-e0,3,a35,36"), "touchscreen"), "MT axes");
    CHECK(!strcmp(sysinfo_input_role("rk29-keypad", KEYPAD), "buttons"), "keypad");
    CHECK(!strcmp(sysinfo_input_role("accel", "input:b0-e0,3,a0,1,2"), "other"), "abs without MT");
}

static void test_asound_card_parse(void) {
    int idx = -1;
    char id[32], name[128];
    CHECK(asound_card_parse(" 0 [RK29RT3261     ]: RK29_RT3261 - RK29_RT3261", &idx, id, sizeof id, name,
                            sizeof name) == 0,
          "parse");
    CHECK(idx == 0 && !strcmp(id, "RK29RT3261") && !strcmp(name, "RK29_RT3261 - RK29_RT3261"), "%d '%s' '%s'", idx,
          id, name);
    CHECK(asound_card_parse("                      RK29_RT3261", &idx, id, sizeof id, name, sizeof name) == -1,
          "the long-name line is not a card line");
    CHECK(asound_card_parse("--- no soundcards ---", &idx, id, sizeof id, name, sizeof name) == -1, "none");
}

static void test_members(void) {
    struct sbuf sb;
    sb_init(&sb);
    sb_puts(&sb, "{");
    hardware_members(&sb, FIXTURE, PROC_FIXTURE);
    sb_puts(&sb, "}");
    expect_in(&sb, "\"input\":[{\"sysfs\":\"input0\",\"device\":\"/dev/input/event0\",\"name\":\"rk29-keypad\","
                   "\"role\":\"buttons\",\"keys\":[\"volume_down\",\"volume_up\",\"power\",\"wakeup\"],"
                   "\"modalias\":\"" KEYPAD "\"}");
    expect_in(&sb, "{\"sysfs\":\"input1\",\"device\":\"/dev/input/event1\",\"name\":\"gslX680\","
                   "\"role\":\"touchscreen\",\"keys\":[],\"modalias\":\"" GSL "\"}]");
    expect_in(&sb, "\"backlight\":{\"device\":\"rk28_bl\",\"attributes\":{\"brightness\":\"127\","
                   "\"max_brightness\":\"255\",\"actual_brightness\":\"67\",\"bl_power\":\"0\",\"type\":\"raw\"}}");
    expect_in(&sb, "\"power_supplies\":[{\"name\":\"ac\",\"attributes\":{\"type\":\"Mains\",\"online\":\"0\"");
    expect_in(&sb, "{\"name\":\"battery\",\"attributes\":{\"type\":\"Battery\",\"online\":null,\"present\":\"1\","
                   "\"status\":\"Not charging\",\"capacity\":\"82\",\"voltage_now\":\"3839000\"");
    expect_in(&sb, "\"thermal_zones\":[]");
    expect_in(&sb, "{\"name\":\"rndis0\",\"operstate\":\"up\",\"mac\":null,\"ipv4\":");
    CHECK(strstr(sb.buf, "\"name\":\"lo\"") == NULL, "loopback left out: %s", sb.buf);
    expect_in(&sb, "\"audio\":{\"cards\":[{\"index\":0,\"id\":\"RK29RT3261\",\"name\":\"RK29_RT3261 - RK29_RT3261\"}],"
                   "\"devices\":[\"card0\",\"controlC0\",\"hwC0D0\",\"pcmC0D0c\",\"pcmC0D0p\",\"pcmC0D1c\",\"pcmC0D1p\",\"timer\"]}");
    expect_in(&sb, "\"video_devices\":[{\"device\":\"/dev/video0\",\"name\":null}]");
    sb_free(&sb);

    /* Nothing there: empty lists and nulls, nothing invented. */
    sb_init(&sb);
    hardware_members(&sb, "/nonexistent-sysfs-root", "/nonexistent-proc-root");
    expect_in(&sb, "\"input\":[]");
    expect_in(&sb, "\"backlight\":null");
    expect_in(&sb, "\"power_supplies\":[]");
    expect_in(&sb, "\"network_interfaces\":[]");
    expect_in(&sb, "\"audio\":{\"cards\":null,\"devices\":[]}");
    expect_in(&sb, "\"video_devices\":[]");
    sb_free(&sb);
}

int main(void) {
    test_input_role();
    test_asound_card_parse();
    test_members();
    return test_finish("test_hardware");
}
