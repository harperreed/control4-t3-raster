/* ABOUTME: Host unit tests for backlight.c on a temporary sysfs tree of real files: blank/wake with bl_power
 * ABOUTME: (rk28_bl) and the brightness-0 fallback, the level wake restores, and brightness set while blank. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backlight.h"
#include "sysinfo.h"
#include "test_common.h"

#define BL_DIR "class/backlight"
#define BL "rk28_bl"

/* A fresh sysfs root holding one backlight, like the panel's rk28_bl. With
 * bl_power < 0 the backlight has no bl_power file. */
static void make_root(char *root, long brightness, long bl_power) {
    char path[512];
    strcpy(root, "/tmp/tt7d-backlight-XXXXXX");
    CHECK(mkdtemp(root) != NULL, "mkdtemp");
    snprintf(path, sizeof path, "%s/class", root);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/" BL_DIR, root);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/" BL_DIR "/" BL, root);
    mkdir(path, 0755);
    const char *attrs[] = {"brightness", "max_brightness", "bl_power"};
    long values[] = {brightness, 255, bl_power};
    for (int i = 0; i < 3; i++) {
        if (values[i] < 0) continue;
        snprintf(path, sizeof path, "%s/" BL_DIR "/" BL "/%s", root, attrs[i]);
        FILE *f = fopen(path, "w");
        fprintf(f, "%ld\n", values[i]);
        fclose(f);
    }
}

static void remove_root(const char *root) {
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    CHECK(system(cmd) == 0, "rm %s", root);
}

static long attr(const char *root, const char *name) { return read_long(root, BL_DIR, BL, name, -99); }

static void expect_state(const struct backlight *b, int want_on, int want_pct) {
    int on, pct;
    backlight_state(b, &on, &pct);
    CHECK(on == want_on && pct == want_pct, "state on=%d pct=%d, want on=%d pct=%d", on, pct, want_on, want_pct);
}

static void test_bl_power(void) {
    char root[64];
    make_root(root, 127, 0);
    struct backlight b;
    backlight_init(&b, root);
    CHECK(!strcmp(b.name, BL) && b.method == BLANK_BL_POWER, "found %s, method %d", b.name, b.method);
    CHECK(!strcmp(backlight_blank_method(&b), "bl_power"), "%s", backlight_blank_method(&b));
    CHECK(b.level == 127, "level %ld", b.level);
    expect_state(&b, 1, 50);

    /* Blank powers the backlight down and leaves brightness alone: writing 0
     * there makes the rk28_bl screen bright. */
    CHECK(backlight_blank(&b) == 0, "blank");
    CHECK(attr(root, "bl_power") == 4 && attr(root, "brightness") == 127, "bl_power %ld brightness %ld",
          attr(root, "bl_power"), attr(root, "brightness"));
    expect_state(&b, 0, 50);
    CHECK(backlight_blank(&b) == 0 && b.level == 127 && attr(root, "bl_power") == 4, "blank again changes nothing");

    /* A brightness set while blank waits for wake; the screen stays dark. */
    CHECK(backlight_set(&b, 200) == 0, "set while blank");
    CHECK(attr(root, "bl_power") == 4 && attr(root, "brightness") == 127, "set while blank touched sysfs");
    CHECK(b.level == 200, "level %ld", b.level);
    expect_state(&b, 0, 78);
    CHECK(backlight_blank(&b) == 0 && b.level == 200, "blank while blank keeps the stored level");

    CHECK(backlight_wake(&b) == 0, "wake");
    CHECK(attr(root, "bl_power") == 0 && attr(root, "brightness") == 200, "bl_power %ld brightness %ld",
          attr(root, "bl_power"), attr(root, "brightness"));
    expect_state(&b, 1, 78);
    CHECK(backlight_wake(&b) == 0 && attr(root, "brightness") == 200, "wake while awake changes nothing");

    /* Awake, a brightness goes straight to sysfs. */
    CHECK(backlight_set(&b, 64) == 0 && attr(root, "brightness") == 64 && b.level == 64, "set while awake");
    expect_state(&b, 1, 25);

    /* Blank then wake with no change in between restores the same level. */
    CHECK(backlight_blank(&b) == 0 && backlight_wake(&b) == 0, "round trip");
    CHECK(attr(root, "bl_power") == 0 && attr(root, "brightness") == 64, "brightness %ld", attr(root, "brightness"));
    remove_root(root);
}

static void test_bl_power_started_blank(void) {
    /* tt7d restarted while blank: brightness still holds the level. */
    char root[64];
    make_root(root, 90, 4);
    struct backlight b;
    backlight_init(&b, root);
    CHECK(b.level == 90, "level %ld", b.level);
    expect_state(&b, 0, 35);
    CHECK(backlight_wake(&b) == 0 && attr(root, "bl_power") == 0 && attr(root, "brightness") == 90, "wake");
    remove_root(root);
}

static void test_brightness_fallback(void) {
    char root[64];
    make_root(root, 127, -1);
    struct backlight b;
    backlight_init(&b, root);
    CHECK(b.method == BLANK_BRIGHTNESS && !strcmp(backlight_blank_method(&b), "brightness"), "method %d",
          b.method);

    CHECK(backlight_blank(&b) == 0 && attr(root, "brightness") == 0 && b.level == 127, "blank writes 0");
    expect_state(&b, 0, 50); /* the stored level, not the 0 in sysfs */
    CHECK(backlight_set(&b, 200) == 0 && attr(root, "brightness") == 0 && b.level == 200, "set while blank waits");
    CHECK(backlight_wake(&b) == 0 && attr(root, "brightness") == 200, "wake restores %ld", attr(root, "brightness"));
    expect_state(&b, 1, 78);
    remove_root(root);

    /* Started at 0 with no level seen: wake uses max_brightness, as tt7-app does at boot. */
    make_root(root, 0, -1);
    backlight_init(&b, root);
    CHECK(b.level == -1, "level %ld", b.level);
    CHECK(backlight_wake(&b) == 0 && attr(root, "brightness") == 255, "wake to max: %ld", attr(root, "brightness"));
    remove_root(root);
}

static void test_no_backlight(void) {
    char root[] = "/tmp/tt7d-backlight-XXXXXX";
    CHECK(mkdtemp(root) != NULL, "mkdtemp");
    struct backlight b;
    backlight_init(&b, root);
    CHECK(b.name[0] == 0 && b.level == -1, "name '%s'", b.name);
    expect_state(&b, -1, -1);
    remove_root(root);
}

int main(void) {
    test_bl_power();
    test_bl_power_started_blank();
    test_brightness_fallback();
    test_no_backlight();
    return test_finish("test_backlight");
}
