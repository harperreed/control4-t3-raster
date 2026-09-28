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

/* Answer a request that panel_check_head() accepted. */
void panel_handle(struct panel *p, const struct http_request *req, const uint8_t *body, size_t len,
                  struct response *resp);

#endif
