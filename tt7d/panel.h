/* ABOUTME: The local control panel (SPEC 29-31): serves the embedded web UI and its admin API
 * ABOUTME: (/hardware, /system, /logs, brightness, blank/wake, test pattern, reboot). main.c routes to it. */
#ifndef TT7D_PANEL_H
#define TT7D_PANEL_H

#include <stddef.h>
#include <stdint.h>

#include "display.h"
#include "frame.h"
#include "http.h"
#include "server.h"

struct panel {
    const char *sysfs_root;
    const char *proc_root;
    const char *data_dir;
    const char *log_file;   /* tt7d's own log (tt7-app sends its stderr there) */
    const char *reboot_cmd; /* run with /bin/sh -c by a detached child */
    const char *ntp_marker; /* for /system time.synchronized; NULL reports null */
    const char *token;
    const char *firmware_version, *build;
    struct display *disp;
    struct frame_store *frames;
    long wake_level; /* raw backlight level that wake restores; -1 until one is seen */
};

/* Remember the current brightness as the level to wake to. */
void panel_init(struct panel *p);

/* Head-time routing for the panel's paths. Returns PANEL_NOT_MINE for any
 * other path, 0 to go on to panel_handle(), or -1 with resp filled (405, or
 * 401 for a mutation without the right bearer token). */
#define PANEL_NOT_MINE 1
int panel_check_head(struct panel *p, const struct http_request *req, struct response *resp);

/* The display and reboot actions, shared by the HTTP routes and MQTT
 * commands. They return PANEL_OK or one of the negative codes; on
 * PANEL_WRITE_FAILED and PANEL_ACTION_FAILED errno says why. */
enum panel_result {
    PANEL_OK = 0,
    PANEL_NO_BACKLIGHT = -1,
    PANEL_OUT_OF_RANGE = -2,
    PANEL_WRITE_FAILED = -3,
    PANEL_ACTION_FAILED = -4,
};

/* value is a percentage (0-100) if percent, else a raw backlight level
 * (0..max_brightness). A non-zero level also becomes the level wake restores. */
int panel_set_brightness(struct panel *p, long value, int percent);

/* Backlight to 0, remembering the level for wake. */
int panel_blank(struct panel *p);

/* bl_power on, and the remembered level if the backlight is at 0. */
int panel_wake(struct panel *p);

/* Run reboot_cmd from a detached process after REBOOT_DELAY_S seconds. */
#define REBOOT_DELAY_S 1 /* lets an HTTP reply or MQTT publish get out first */
int panel_reboot(struct panel *p);

/* Answer a request that panel_check_head() accepted. */
void panel_handle(struct panel *p, const struct http_request *req, const uint8_t *body, size_t len,
                  struct response *resp);

#endif
