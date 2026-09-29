/* ABOUTME: The display backlight in sysfs: blank, wake, brightness, and the level wake restores.
 * ABOUTME: Blanks with bl_power where the backlight has it (rk28_bl), else by writing brightness 0. */
#ifndef TT7D_BACKLIGHT_H
#define TT7D_BACKLIGHT_H

#include "sysinfo.h"

/* How blank turns the backlight off, chosen at backlight_init. On the TT7's
 * rk28_bl, brightness 0 does NOT go dark: the driver treats 0 as a special
 * case and the screen goes bright (seen on the panel 2026-09-28). bl_power 4
 * (FB_BLANK_POWERDOWN) turns it fully off, and bl_power 0 back on at the
 * level it had. A backlight without bl_power falls back to brightness 0. */
enum blank_method { BLANK_BL_POWER, BLANK_BRIGHTNESS };

struct backlight {
    const char *sysfs_root;
    char name[NAME_LEN]; /* the first backlight in class/backlight, "" if none */
    enum blank_method method;
    /* The raw level the user wants: what wake restores, and what state reports
     * while blank. A level set while blank lives only here until wake.
     * -1 until one is seen. */
    long level;
};

/* Find the backlight and its blank method, and take its brightness as the level. */
void backlight_init(struct backlight *b, const char *sysfs_root);

/* "bl_power" or "brightness": the blank_method the API reports. */
const char *backlight_blank_method(const struct backlight *b);

/* 1 if the backlight is blank by its method: bl_power != 0, or brightness 0. */
int backlight_is_blank(const struct backlight *b);

/* on (1, 0, or -1 without a backlight) and brightness percent (-1 if
 * unknown). While blank the percent is the stored level's, not sysfs's. */
void backlight_state(const struct backlight *b, int *on, int *percent);

/* Set raw level (1..max_brightness; callers clamp with brightness_to_raw, so
 * 0 never gets here). Awake, it goes to sysfs. Blank, it is only stored:
 * the screen stays dark and wake applies it. 0, or -1 with errno. */
int backlight_set(struct backlight *b, long raw);

/* Turn the backlight off, remembering its level. Blank again is a no-op. 0 or -1 with errno. */
int backlight_blank(struct backlight *b);

/* Turn it back on at the stored level (max_brightness if none was ever
 * seen, as tt7-app does at boot). Awake already is a no-op. 0 or -1 with errno. */
int backlight_wake(struct backlight *b);

#endif
