/* ABOUTME: Control panel routes: embedded static files, /hardware, /system, /logs, and the display and
 * ABOUTME: reboot actions. Every mutation needs the bearer token; blank/wake drive the backlight in sysfs. */
#include "panel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "assets.h"
#include "control.h"
#include "hardware.h"
#include "ident.h"
#include "render.h"
#include "sha256.h"
#include "sysinfo.h"
#include "timesync.h"

#define LOG_TAIL_BYTES (256 * 1024) /* how much of the log file end is read for a tail */

enum route_id { R_HARDWARE, R_SYSTEM, R_LOGS, R_BRIGHTNESS, R_BLANK, R_WAKE, R_TEST_PATTERN, R_REBOOT };

static const struct {
    enum route_id id;
    const char *path;
    const char *method; /* the only one allowed */
    int auth;
} routes[] = {
    {R_HARDWARE, "/api/v1/hardware", "GET", 0},
    {R_SYSTEM, "/api/v1/system", "GET", 0},
    {R_LOGS, "/api/v1/logs", "GET", 1}, /* diagnostics: SPEC 36 wants these authenticated */
    {R_BRIGHTNESS, "/api/v1/display/brightness", "PUT", 1},
    {R_BLANK, "/api/v1/display/blank", "POST", 1},
    {R_WAKE, "/api/v1/display/wake", "POST", 1},
    {R_TEST_PATTERN, "/api/v1/display/test-pattern", "POST", 1},
    {R_REBOOT, "/api/v1/system/reboot", "POST", 1},
};
#define NROUTES (sizeof routes / sizeof routes[0])

static int find_route(const char *path) {
    for (size_t i = 0; i < NROUTES; i++)
        if (!strcmp(routes[i].path, path)) return (int)i;
    return -1;
}

static void method_not_allowed(struct response *resp, const char *allow) {
    resp_error(resp, 405, "method_not_allowed", "this method is not supported on this path");
    snprintf(resp->extra_headers, sizeof resp->extra_headers, "Allow: %s\r\n", allow);
}

int panel_check_head(struct panel *p, const struct http_request *req, struct response *resp) {
    int r = find_route(req->path);
    if (r < 0) {
        if (!asset_find(req->path)) return PANEL_NOT_MINE;
        if (strcmp(req->method, "GET") != 0) {
            method_not_allowed(resp, "GET");
            return -1;
        }
        return 0;
    }
    if (strcmp(req->method, routes[r].method) != 0) {
        method_not_allowed(resp, routes[r].method);
        return -1;
    }
    return routes[r].auth ? resp_require_bearer(p->token, req, resp) : 0;
}

/* ---- backlight --------------------------------------------------------------- */

void panel_init(struct panel *p) { backlight_init(&p->bl, p->sysfs_root); }

/* The display power state after an action: what /state reports, plus the
 * raw levels the action worked with. */
static void display_state(const struct panel *p, struct response *resp) {
    const char *bl = p->bl.name;
    int on, pct;
    backlight_state(&p->bl, &on, &pct);
    long raw = read_long(p->sysfs_root, "class/backlight", bl, "brightness", -1);
    long max = read_long(p->sysfs_root, "class/backlight", bl, "max_brightness", -1);
    struct sbuf *sb = &resp->body;
    resp->status = 200;
    sb_printf(sb, "{\"on\":%s,\"brightness\":", on < 0 ? "null" : on ? "true" : "false");
    if (pct >= 0) sb_printf(sb, "{\"value\":%d,\"unit\":\"percent\",\"available\":true}", pct);
    else sb_puts(sb, "{\"value\":null,\"unit\":\"percent\",\"available\":false}");
    sb_puts(sb, ",\"brightness_raw\":");
    if (raw >= 0) sb_printf(sb, "%ld", raw);
    else sb_puts(sb, "null");
    sb_puts(sb, ",\"max_brightness\":");
    if (max >= 0) sb_printf(sb, "%ld", max);
    else sb_puts(sb, "null");
    sb_puts(sb, ",\"wake_brightness_raw\":");
    if (p->bl.level > 0) sb_printf(sb, "%ld", p->bl.level);
    else sb_puts(sb, "null");
    sb_puts(sb, ",\"blank_method\":");
    sb_json_str(sb, backlight_blank_method(&p->bl));
    sb_puts(sb, "}");
}

/* The HTTP answer for a panel_* action result. */
static void action_reply(struct panel *p, int rc, struct response *resp) {
    int saved = errno;
    switch (rc) {
    case PANEL_OK: display_state(p, resp); break;
    case PANEL_NO_BACKLIGHT: resp_error(resp, 503, "no_backlight", "no backlight device in sysfs"); break;
    default: /* PANEL_WRITE_FAILED */
        resp_error_begin(resp, 500, "backlight_write_failed", "could not write the backlight's sysfs file");
        sb_puts(&resp->body, ",\"device\":");
        sb_json_str(&resp->body, p->bl.name);
        sb_puts(&resp->body, ",\"detail\":");
        sb_json_str(&resp->body, strerror(saved));
        resp_error_end(resp);
    }
}

int panel_set_brightness(struct panel *p, long value, int percent) {
    if (!p->bl.name[0]) return PANEL_NO_BACKLIGHT;
    long max = read_long(p->sysfs_root, "class/backlight", p->bl.name, "max_brightness", -1);
    long raw = brightness_to_raw(value, percent, max);
    if (raw < 0) return PANEL_OUT_OF_RANGE;
    return backlight_set(&p->bl, raw) == 0 ? PANEL_OK : PANEL_WRITE_FAILED;
}

static void set_brightness(struct panel *p, const uint8_t *body, size_t len, struct response *resp) {
    long value;
    int percent;
    if (brightness_parse((const char *)body, len, &value, &percent) != 0) {
        resp_error(resp, 400, "invalid_brightness",
                   "send {\"value\": <integer>} (raw backlight level) or {\"value\": <0-100>, \"unit\": \"percent\"}");
        return;
    }
    int rc = panel_set_brightness(p, value, percent);
    if (rc == PANEL_OUT_OF_RANGE) {
        long max = read_long(p->sysfs_root, "class/backlight", p->bl.name, "max_brightness", -1);
        resp_error_begin(resp, 400, "brightness_out_of_range", "the value is outside the allowed range");
        sb_printf(&resp->body, ",\"unit\":\"%s\",\"min\":0,\"max\":", percent ? "percent" : "raw");
        if (percent || max > 0) sb_printf(&resp->body, "%ld", percent ? 100L : max);
        else sb_puts(&resp->body, "null");
        resp_error_end(resp);
        return;
    }
    action_reply(p, rc, resp);
}

/* Blank and wake are backlight.c's. The fb blank ioctl is not used: on this
 * Rockchip 3.0 kernel it is unverified what it powers down and whether
 * unblank brings the LCD controller back. */
int panel_blank(struct panel *p) {
    if (!p->bl.name[0]) return PANEL_NO_BACKLIGHT;
    return backlight_blank(&p->bl) == 0 ? PANEL_OK : PANEL_WRITE_FAILED;
}

int panel_wake(struct panel *p) {
    if (!p->bl.name[0]) return PANEL_NO_BACKLIGHT;
    return backlight_wake(&p->bl) == 0 ? PANEL_OK : PANEL_WRITE_FAILED;
}

/* ---- test pattern -------------------------------------------------------------- */

static void test_pattern(struct panel *p, struct response *resp) {
    const struct asset *a = asset_find(ASSET_TEST_PATTERN);
    char sha[65], hex[17], id[64];
    if (!a || random_hex(hex, 8) != 0) {
        resp_error(resp, 500, "internal_error", "the built-in test pattern is unavailable");
        return;
    }
    sha256_hex(a->data, a->len, sha);
    snprintf(id, sizeof id, "test-pattern-%s", hex);
    frame_show(p->frames, a->data, a->len, sha, id, 0, resp);
}

/* ---- reboot -------------------------------------------------------------------- */

/* Returns at once, so the caller can reply (HTTP 202, or MQTT); a detached
 * grandchild waits, syncs, and runs the reboot command. Detached because a
 * reboot attached to a session once hung for minutes (gotchas.md), and
 * because plain `reboot` only signals PID 1, which our init ignores: the
 * default command is `reboot -f`. */
int panel_reboot(struct panel *p) {
    sync();
    pid_t pid = fork();
    if (pid < 0) return PANEL_ACTION_FAILED;
    if (pid == 0) {
        setsid();
        pid_t grandchild = fork();
        if (grandchild != 0) _exit(grandchild < 0 ? 1 : 0);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        sleep(REBOOT_DELAY_S);
        sync();
        execl("/bin/sh", "sh", "-c", p->reboot_cmd, (char *)NULL);
        fprintf(stderr, "tt7d: exec /bin/sh for the reboot command: %s\n", strerror(errno));
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    fprintf(stderr, "tt7d: reboot requested; running '%s' in %d s\n", p->reboot_cmd, REBOOT_DELAY_S);
    return PANEL_OK;
}

static void reboot_later(struct panel *p, struct response *resp) {
    if (panel_reboot(p) != PANEL_OK) {
        resp_error(resp, 500, "reboot_failed", strerror(errno));
        return;
    }
    resp->status = 202;
    sb_printf(&resp->body, "{\"rebooting\":true,\"delay\":{\"value\":%d,\"unit\":\"second\"}}", REBOOT_DELAY_S);
}

/* ---- logs ---------------------------------------------------------------------- */

/* The last n lines of a file as a JSON array, reading at most the last
 * LOG_TAIL_BYTES. Returns -1 (nothing appended) if it cannot be read. */
static int file_tail_json(struct sbuf *sb, const char *path, unsigned n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    off_t off = st.st_size > LOG_TAIL_BYTES ? st.st_size - LOG_TAIL_BYTES : 0;
    size_t want = (size_t)(st.st_size - off), got = 0;
    char *buf = malloc(want ? want : 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    while (got < want) {
        ssize_t r = pread(fd, buf + got, want - got, off + (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    size_t start = tail_start(buf, got, n);
    if (start == 0 && off > 0) { /* the window starts mid-line: drop that partial line */
        char *nl = memchr(buf, '\n', got);
        start = nl ? (size_t)(nl - buf) + 1 : got;
    }
    json_lines(sb, buf + start, got - start, 0);
    free(buf);
    return 0;
}

static void kernel_log_json(struct sbuf *sb, unsigned n) {
    int size = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
    char *buf = size > 0 ? malloc((size_t)size) : NULL;
    int got = buf ? klogctl(3 /* SYSLOG_ACTION_READ_ALL */, buf, size) : -1;
    if (got < 0) {
        const char *why = size < 0 || buf ? strerror(errno) : "out of memory";
        sb_puts(sb, "\"kernel\":{\"available\":false,\"lines\":[],\"error\":");
        sb_json_str(sb, why);
        sb_puts(sb, "}");
        free(buf);
        return;
    }
    size_t start = tail_start(buf, (size_t)got, n);
    sb_puts(sb, "\"kernel\":{\"available\":true,\"lines\":");
    json_lines(sb, buf + start, (size_t)got - start, 1);
    sb_puts(sb, ",\"error\":null}");
    free(buf);
}

static void logs(struct panel *p, const struct http_request *req, struct response *resp) {
    unsigned n;
    if (lines_query(req->query, 200, 2000, &n) != 0) {
        resp_error(resp, 400, "invalid_lines", "lines must be an integer from 1 to 2000");
        return;
    }
    struct sbuf *sb = &resp->body;
    resp->status = 200;
    sb_printf(sb, "{\"lines_requested\":%u,\"tt7d\":{\"path\":", n);
    sb_json_str(sb, p->log_file);
    sb_puts(sb, ",\"available\":");
    size_t mark = sb->len;
    sb_puts(sb, "true,\"lines\":");
    if (file_tail_json(sb, p->log_file, n) != 0) {
        sb->len = mark;
        sb->buf[mark] = 0;
        sb_puts(sb, "false,\"lines\":[]");
    }
    sb_puts(sb, "},");
    kernel_log_json(sb, n);
    sb_puts(sb, "}");
}

/* ---- /system and /hardware ------------------------------------------------------- */

/* A /proc/meminfo value in KiB, or -1. */
long meminfo_kib(const char *proc_root, const char *key) {
    char path[512], line[128];
    snprintf(path, sizeof path, "%s/meminfo", proc_root);
    FILE *f = fopen(path, "r");
    long v = -1;
    size_t klen = strlen(key);
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, key, klen) && line[klen] == ':') {
            v = strtol(line + klen + 1, NULL, 10);
            break;
        }
    if (f) fclose(f);
    return v;
}

static void quantity(struct sbuf *sb, const char *key, long long v, const char *unit) {
    sb_printf(sb, "\"%s\":", key);
    if (v >= 0) sb_printf(sb, "{\"value\":%lld,\"unit\":\"%s\"}", v, unit);
    else sb_printf(sb, "{\"value\":null,\"unit\":\"%s\"}", unit);
}

static void system_json(struct panel *p, struct sbuf *sb) {
    struct utsname u;
    int have_uname = uname(&u) == 0;
    struct timespec now, boot;
    clock_gettime(CLOCK_REALTIME, &now);
    clock_gettime(CLOCK_BOOTTIME, &boot);

    sb_puts(sb, "{\"firmware_version\":");
    sb_json_str(sb, p->firmware_version);
    sb_puts(sb, ",\"build\":");
    sb_json_str(sb, p->build);
    sb_puts(sb, ",\"kernel\":{\"release\":");
    sb_json_str(sb, have_uname ? u.release : NULL);
    sb_puts(sb, ",\"version\":");
    sb_json_str(sb, have_uname ? u.version : NULL);
    sb_puts(sb, ",\"machine\":");
    sb_json_str(sb, have_uname ? u.machine : NULL);
    sb_printf(sb, "},\"uptime_s\":%lld,\"memory\":{", (long long)boot.tv_sec);
    quantity(sb, "total", meminfo_kib(p->proc_root, "MemTotal"), "kibibyte");
    sb_puts(sb, ",");
    quantity(sb, "free", meminfo_kib(p->proc_root, "MemFree"), "kibibyte");
    sb_puts(sb, ",");
    quantity(sb, "available", meminfo_kib(p->proc_root, "MemAvailable"), "kibibyte"); /* newer than 3.0 */

    struct statvfs vfs;
    int have_vfs = statvfs(p->data_dir, &vfs) == 0;
    sb_puts(sb, "},\"storage\":[{\"path\":");
    sb_json_str(sb, p->data_dir);
    sb_puts(sb, ",");
    quantity(sb, "total", have_vfs ? (long long)vfs.f_blocks * (long long)vfs.f_frsize : -1, "byte");
    sb_puts(sb, ",");
    quantity(sb, "available", have_vfs ? (long long)vfs.f_bavail * (long long)vfs.f_frsize : -1, "byte");

    sb_puts(sb, "}],\"time\":{\"now\":");
    sb_json_time(sb, &now);
    /* `now` is UTC. A 1970 date means "never set"; synchronized means NTP set
     * it this boot (the tt7-ntp-hook marker, timesync.h). */
    time_t synced_at;
    sb_printf(sb, ",\"plausible\":%s,\"timezone\":\"UTC\",\"synchronized\":%s}}",
              clock_plausible(now.tv_sec) ? "true" : "false",
              !p->ntp_marker ? "null" : timesync_synced(p->ntp_marker, now.tv_sec, &synced_at) ? "true" : "false");
}

static void hardware_json(struct panel *p, struct sbuf *sb) {
    const struct display *d = p->disp;
    sb_puts(sb, "{\"display\":{\"device\":");
    sb_json_str(sb, d->device);
    sb_printf(sb,
              ",\"native\":{\"width\":%u,\"height\":%u,\"format\":\"%s\",\"stride\":%u,\"bits_per_pixel\":%u},"
              "\"rotation\":%d,\"logical\":{\"width\":%u,\"height\":%u},\"blank_method\":\"%s\"},",
              d->back.width, d->back.height, render_format_name(&d->back), d->back.stride, d->back.bpp, d->rotation,
              d->logical_w, d->logical_h, backlight_blank_method(&p->bl));
    hardware_members(sb, p->sysfs_root, p->proc_root);
    sb_puts(sb, "}");
}

void panel_handle(struct panel *p, const struct http_request *req, const uint8_t *body, size_t len,
                  struct response *resp) {
    int r = find_route(req->path);
    resp->status = 200;
    if (r < 0) { /* an embedded file; panel_check_head() made sure it exists */
        const struct asset *a = asset_find(req->path);
        resp->content_type = a->content_type;
        sb_add(&resp->body, a->data, a->len);
        return;
    }
    switch (routes[r].id) {
    case R_HARDWARE: hardware_json(p, &resp->body); break;
    case R_SYSTEM: system_json(p, &resp->body); break;
    case R_LOGS: logs(p, req, resp); break;
    case R_BRIGHTNESS: set_brightness(p, body, len, resp); break;
    case R_BLANK: action_reply(p, panel_blank(p), resp); break;
    case R_WAKE: action_reply(p, panel_wake(p), resp); break;
    case R_TEST_PATTERN: test_pattern(p, resp); break;
    case R_REBOOT: reboot_later(p, resp); break;
    }
}
