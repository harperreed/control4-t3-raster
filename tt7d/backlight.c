/* ABOUTME: Backlight blank/wake/brightness over sysfs (see backlight.h). Blank uses bl_power when the
 * ABOUTME: backlight has one and never writes brightness 0 there; a level set while blank waits for wake. */
#include "backlight.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BL_DIR "class/backlight"
#define FB_BLANK_UNBLANK 0
#define FB_BLANK_POWERDOWN 4

static long get(const struct backlight *b, const char *attr, long fallback) {
    return read_long(b->sysfs_root, BL_DIR, b->name, attr, fallback);
}

static int put(const struct backlight *b, const char *attr, long value) {
    char path[512];
    snprintf(path, sizeof path, "%s/" BL_DIR "/%s/%s", b->sysfs_root, b->name, attr);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int ok = fprintf(f, "%ld\n", value) > 0;
    return fclose(f) == 0 && ok ? 0 : -1;
}

void backlight_init(struct backlight *b, const char *sysfs_root) {
    struct names bl;
    list_dir(sysfs_root, BL_DIR, &bl);
    memset(b, 0, sizeof *b);
    b->sysfs_root = sysfs_root;
    snprintf(b->name, sizeof b->name, "%s", bl.n ? bl.v[0] : "");
    b->level = -1;
    if (!b->name[0]) return;
    char path[512];
    snprintf(path, sizeof path, "%s/" BL_DIR "/%s/bl_power", sysfs_root, b->name);
    b->method = access(path, W_OK) == 0 ? BLANK_BL_POWER : BLANK_BRIGHTNESS;
    long level = get(b, "brightness", -1);
    if (level > 0) b->level = level;
}

const char *backlight_blank_method(const struct backlight *b) {
    return b->method == BLANK_BL_POWER ? "bl_power" : "brightness";
}

int backlight_is_blank(const struct backlight *b) {
    if (b->method == BLANK_BL_POWER) return get(b, "bl_power", FB_BLANK_UNBLANK) != FB_BLANK_UNBLANK;
    return get(b, "brightness", -1) == 0;
}

void backlight_state(const struct backlight *b, int *on, int *percent) {
    sysinfo_backlight(b->sysfs_root, on, percent);
    long max = b->name[0] ? get(b, "max_brightness", -1) : -1;
    if (*on == 0 && b->level > 0 && max > 0) *percent = (int)((b->level * 100 + max / 2) / max);
}

int backlight_set(struct backlight *b, long raw) {
    if (!backlight_is_blank(b) && put(b, "brightness", raw) != 0) return -1;
    b->level = raw;
    return 0;
}

int backlight_blank(struct backlight *b) {
    if (backlight_is_blank(b)) return 0; /* keeps the level, including one set while blank */
    long cur = get(b, "brightness", -1);
    if (cur > 0) b->level = cur;
    if (b->method == BLANK_BL_POWER) return put(b, "bl_power", FB_BLANK_POWERDOWN);
    return put(b, "brightness", 0);
}

int backlight_wake(struct backlight *b) {
    long cur = get(b, "brightness", -1);
    long power = b->method == BLANK_BL_POWER ? get(b, "bl_power", FB_BLANK_UNBLANK) : FB_BLANK_UNBLANK;
    if (power == FB_BLANK_UNBLANK && cur > 0) return 0; /* awake */
    long level = b->level > 0 ? b->level : get(b, "max_brightness", -1);
    /* The level first, so bl_power comes back on at it. A brightness of 0
     * here (someone else wrote it) is fixed too: on rk28_bl it means bright. */
    if (level > 0 && cur != level && put(b, "brightness", level) != 0) return -1;
    if (power != FB_BLANK_UNBLANK && put(b, "bl_power", FB_BLANK_UNBLANK) != 0) return -1;
    if (level > 0) b->level = level;
    return 0;
}
