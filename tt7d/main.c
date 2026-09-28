/* ABOUTME: tt7d, the TT7 network display daemon: shows PNG frames PUT over HTTP on the framebuffer.
 * ABOUTME: Flags, startup (identity, display, restore last frame, HTTP), routing, and the /info and /state documents. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "display.h"
#include "events.h"
#include "fallback_screen.h"
#include "frame.h"
#include "ident.h"
#include "mqtt.h"
#include "panel.h"
#include "render.h"
#include "server.h"
#include "sysinfo.h"
#include "timesync.h"

#define FIRMWARE_VERSION "0.1.0"
#ifndef TT7D_VERSION
#define TT7D_VERSION "unknown"
#endif
#define MODEL "C4-TT7"

struct config {
    const char *listen;
    const char *fb;
    const char *fb_file;
    const char *fb_geometry;
    unsigned fb_stride;
    const char *fb_format;
    int rotation;
    const char *data_dir;
    const char *sysfs_root;
    const char *proc_root;
    const char *log_file;
    const char *reboot_cmd;
    const char *input_dir;
    size_t max_frame_bytes;
    int timeout_ms;
    const char *mqtt_flags[2 * MQF_COUNT]; /* --mqtt-KEY VALUE, as key/value pairs */
    char mqtt_keys[MQF_COUNT][32];
    int n_mqtt_flags;
    /* The fallback clock (SPEC 41.1). */
    unsigned fallback_timeout_s;
    int clock_hour12;
    const char *tz; /* NULL: <data-dir>/tz, else the default */
    const char *ntp_marker;
};

struct app {
    struct config cfg;
    struct display disp;
    struct frame_store frames;
    struct panel panel;
    struct mqtt_app mqtt;
    struct events events;
    struct fallback_screen fallback;
    char token[256];
    char device_id[32]; /* "" if device.json is unusable: reported as null */
    struct timespec started;
};

static void usage(FILE *out) {
    fprintf(out,
            "usage: tt7d [options]\n"
            "  --listen IPV4:PORT        HTTP address (default 0.0.0.0:80)\n"
            "  --fb PATH                 framebuffer device (default /dev/fb0)\n"
            "  --fb-file PATH            use a regular file as the framebuffer instead (host tests)\n"
            "  --fb-geometry WxHxBPP     with --fb-file: e.g. 800x1280x16\n"
            "  --fb-stride BYTES         with --fb-file: bytes per line (default W*BPP/8)\n"
            "  --fb-format NAME          with --fb-file: rgb565 or xrgb8888 (default from BPP)\n"
            "  --rotation DEG            0, 90, 180 or 270: degrees clockwise the logical image is turned\n"
            "                            to land on the native framebuffer (default 270: upright on a docked TT7)\n"
            "  --data-dir PATH           token, device.json, last-frame.png (default /data/tt7/tt7d)\n"
            "  --sysfs-root PATH         where to read sysfs from (default /sys)\n"
            "  --proc-root PATH          where to read /proc files from (default /proc)\n"
            "  --log-file PATH           tt7d's own log, for GET /api/v1/logs (default /data/tt7/app.log,\n"
            "                            where tt7-app sends tt7d's output)\n"
            "  --reboot-cmd CMD          run with /bin/sh -c by POST /api/v1/system/reboot (default 'reboot -f')\n"
            "  --input-dir PATH          where the evdev eventN nodes are (default /dev/input); --sysfs-root's\n"
            "                            class/input names them. Host tests point it at FIFOs (tt7d/README.md)\n"
            "  --max-frame-bytes N       largest accepted PNG (default 8388608)\n"
            "  --request-timeout-ms N    time to receive a request, and to send its reply (default 30000)\n"
            "  --mqtt-KEY VALUE          override one <data-dir>/mqtt.conf setting (tt7d/README.md): enabled,\n"
            "                            host, port, username, password-file, prefix, client-id, keepalive,\n"
            "                            telemetry-interval, ha-discovery, allow-reboot-cmd\n"
            "  --fallback-timeout S      show the fallback clock when no frame or heartbeat came for S seconds,\n"
            "                            and before the first frame (default 300; 0 = never)\n"
            "  --clock-format 24|12      the fallback clock's hours (default 24)\n"
            "  --tz POSIX-TZ             timezone as a POSIX TZ string (default: first line of <data-dir>/tz,\n"
            "                            else " TIMESYNC_DEFAULT_TZ ", America/Chicago)\n"
            "  --ntp-marker PATH         the file tt7-ntp-hook writes once NTP set the clock; no time is shown\n"
            "                            before it exists (default " TIMESYNC_DEFAULT_MARKER ")\n"
            "  --version, --help\n");
}

static int parse_uint(const char *s, unsigned long max, unsigned long *out) {
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (!*s || *end || errno || v > max || *s == '-') return -1;
    *out = v;
    return 0;
}

static int parse_args(int argc, char **argv, struct config *c) {
    *c = (struct config){.listen = "0.0.0.0:80", .fb = "/dev/fb0", .rotation = 270, .data_dir = "/data/tt7/tt7d",
                         .sysfs_root = "/sys", .proc_root = "/proc", .log_file = "/data/tt7/app.log",
                         .reboot_cmd = "reboot -f", .input_dir = "/dev/input", .max_frame_bytes = 8u << 20,
                         .timeout_ms = 30000, .fallback_timeout_s = 300, .ntp_marker = TIMESYNC_DEFAULT_MARKER};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            exit(0);
        }
        if (!strcmp(a, "--version")) {
            printf("tt7d %s (%s)\n", FIRMWARE_VERSION, TT7D_VERSION);
            exit(0);
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "tt7d: %s needs a value (or is unknown)\n", a);
            return -1;
        }
        const char *v = argv[++i];
        unsigned long n;
        if (!strcmp(a, "--listen")) c->listen = v;
        else if (!strcmp(a, "--fb")) c->fb = v;
        else if (!strcmp(a, "--fb-file")) c->fb_file = v;
        else if (!strcmp(a, "--fb-geometry")) c->fb_geometry = v;
        else if (!strcmp(a, "--fb-format")) c->fb_format = v;
        else if (!strcmp(a, "--data-dir")) c->data_dir = v;
        else if (!strcmp(a, "--sysfs-root")) c->sysfs_root = v;
        else if (!strcmp(a, "--proc-root")) c->proc_root = v;
        else if (!strcmp(a, "--log-file")) c->log_file = v;
        else if (!strcmp(a, "--reboot-cmd") && *v) c->reboot_cmd = v;
        else if (!strcmp(a, "--input-dir")) c->input_dir = v;
        else if (!strcmp(a, "--fallback-timeout") && parse_uint(v, 7 * 86400, &n) == 0) c->fallback_timeout_s = (unsigned)n;
        else if (!strcmp(a, "--clock-format") && (!strcmp(v, "24") || !strcmp(v, "12"))) c->clock_hour12 = v[0] == '1';
        else if (!strcmp(a, "--tz") && timesync_tz_valid(v)) c->tz = v;
        else if (!strcmp(a, "--ntp-marker") && *v) c->ntp_marker = v;
        else if (!strcmp(a, "--fb-stride") && parse_uint(v, 1u << 20, &n) == 0) c->fb_stride = (unsigned)n;
        else if (!strcmp(a, "--rotation") && parse_uint(v, 270, &n) == 0 && render_rotation_valid((int)n))
            c->rotation = (int)n;
        else if (!strcmp(a, "--max-frame-bytes") && parse_uint(v, 256u << 20, &n) == 0 && n > 0)
            c->max_frame_bytes = n;
        else if (!strcmp(a, "--request-timeout-ms") && parse_uint(v, 600000, &n) == 0 && n >= 100)
            c->timeout_ms = (int)n;
        else if (!strncmp(a, "--mqtt-", 7) && strlen(a + 7) < sizeof c->mqtt_keys[0] &&
                 c->n_mqtt_flags < MQF_COUNT) {
            /* --mqtt-password-file -> password_file; mqtt.c validates the pair. */
            char *key = c->mqtt_keys[c->n_mqtt_flags];
            snprintf(key, sizeof c->mqtt_keys[0], "%s", a + 7);
            for (char *p = key; *p; p++)
                if (*p == '-') *p = '_';
            c->mqtt_flags[2 * c->n_mqtt_flags] = key;
            c->mqtt_flags[2 * c->n_mqtt_flags + 1] = v;
            c->n_mqtt_flags++;
        }
        else {
            fprintf(stderr, "tt7d: bad option or value: %s %s\n", a, v);
            return -1;
        }
    }
    return 0;
}

/* mkdir -p, mode 0700 for anything it creates. */
static int make_dirs(const char *path) {
    char buf[512];
    if (snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf) return -1;
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buf, 0700) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(buf, 0700) != 0 && errno != EEXIST ? -1 : 0;
}

/* ---- documents ------------------------------------------------------------- */

static void info_json(struct app *a, struct sbuf *sb) {
    const struct display *d = &a->disp;
    sb_puts(sb, "{\"device_id\":");
    sb_json_str(sb, a->device_id[0] ? a->device_id : NULL);
    sb_printf(sb, ",\"name\":null,\"model\":\"%s\",\"firmware_version\":\"%s\",\"build\":", MODEL, FIRMWARE_VERSION);
    sb_json_str(sb, TT7D_VERSION);
    sb_printf(sb,
              ",\"display\":{\"width\":%u,\"height\":%u,\"rotation\":%d,\"frame_formats\":[\"image/png\"],"
              "\"max_frame_bytes\":%zu,\"native\":{\"width\":%u,\"height\":%u,\"format\":\"%s\",\"stride\":%u,"
              "\"bits_per_pixel\":%u}}",
              d->logical_w, d->logical_h, d->rotation, a->cfg.max_frame_bytes, d->back.width, d->back.height,
              render_format_name(&d->back), d->back.stride, d->back.bpp);
    sb_puts(sb, ",\"capabilities\":{");
    sysinfo_capabilities(sb, a->cfg.sysfs_root);
    sb_puts(sb, ",");
    events_info_member(&a->events, sb);
    sb_puts(sb, "},\"auth\":{\"scheme\":\"bearer\",\"required_for\":[\"PUT /api/v1/frame\",\"GET /api/v1/logs\","
                "\"PUT /api/v1/display/brightness\",\"POST /api/v1/display/blank\",\"POST /api/v1/display/wake\","
                "\"POST /api/v1/display/test-pattern\",\"POST /api/v1/system/reboot\",\"GET /api/v1/config/mqtt\","
                "\"PUT /api/v1/config/mqtt\",\"GET /api/v1/events\",\"POST /api/v1/heartbeat\"]}}");
}

static double seconds_since(const struct timespec *t, clockid_t clock) {
    struct timespec now;
    clock_gettime(clock, &now);
    return (double)(now.tv_sec - t->tv_sec) + (double)(now.tv_nsec - t->tv_nsec) / 1e9;
}

static void state_json(struct app *a, struct sbuf *sb) {
    struct timespec now, boot;
    clock_gettime(CLOCK_REALTIME, &now);
    clock_gettime(CLOCK_BOOTTIME, &boot);
    sb_puts(sb, "{\"time\":");
    sb_json_time(sb, &now);
    sb_printf(sb, ",\"uptime_s\":%lld,\"daemon_uptime_s\":%lld", (long long)boot.tv_sec,
              (long long)seconds_since(&a->started, CLOCK_MONOTONIC));

    int on, pct;
    sysinfo_backlight(a->cfg.sysfs_root, &on, &pct);
    sb_printf(sb, ",\"display\":{\"on\":%s", on < 0 ? "null" : on ? "true" : "false");
    if (pct >= 0) sb_printf(sb, ",\"brightness\":{\"value\":%d,\"unit\":\"percent\",\"available\":true}", pct);
    else sb_puts(sb, ",\"brightness\":{\"value\":null,\"unit\":\"percent\",\"available\":false}");
    const struct frame_store *fs = &a->frames;
    const char *id; /* what the screen shows: a frame, or the fallback clock */
    double age;
    fallback_screen_current(&a->fallback, fs, &id, &age);
    sb_puts(sb, ",\"frame_id\":");
    sb_json_str(sb, id);
    if (age >= 0) sb_printf(sb, ",\"frame_age_s\":%.3f}", age);
    else sb_puts(sb, ",\"frame_age_s\":null}");

    sb_puts(sb, ",");
    fallback_screen_state_members(&a->fallback, sb);
    sb_puts(sb, ",");
    sysinfo_power(sb, a->cfg.sysfs_root);
    sb_puts(sb, ",");
    sysinfo_network(sb, a->cfg.sysfs_root);
    sb_printf(sb, ",\"frames\":{\"accepted\":%lu,\"deduplicated\":%lu,\"rejected\":%lu,\"last_error\":",
              fs->accepted, fs->dedup_count, fs->rejected);
    sb_json_str(sb, fs->last_error);
    sb_puts(sb, "},");
    mqtt_app_state_member(&a->mqtt, sb);
    sb_puts(sb, ",");
    events_state_member(&a->events, sb);
    sb_puts(sb, "}");
}

/* ---- routes ---------------------------------------------------------------- */

struct route {
    const char *path;
    const char *allow; /* for the Allow header */
};

static const struct route routes[] = {
    {"/api/v1/info", "GET"},
    {"/api/v1/state", "GET"},
    {"/api/v1/frame", "GET, PUT"},
    {"/api/v1/frame/image", "GET"},
    {"/api/v1/config/mqtt", "GET, PUT"},
};

static int find_route(const char *path) {
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++)
        if (!strcmp(path, routes[i].path)) return (int)i;
    return -1;
}

static int is_frame_put(const struct http_request *req) {
    return !strcmp(req->path, "/api/v1/frame") && !strcmp(req->method, "PUT");
}

/* 1 if method is in the comma-separated allow list. */
static int allowed(const char *allow, const char *method) {
    size_t n = strlen(method);
    for (const char *p = allow; *p; p += strspn(p, ", ")) {
        size_t len = strcspn(p, ", ");
        if (len == n && !strncmp(p, method, n)) return 1;
        p += len;
    }
    return 0;
}

static int app_check_head(void *ctx, const struct http_request *req, struct response *resp) {
    struct app *a = ctx;
    int rc = events_check_head(&a->events, req, resp);
    if (rc != EVENTS_NOT_MINE) return rc;
    rc = panel_check_head(&a->panel, req, resp);
    if (rc != PANEL_NOT_MINE) return rc;
    rc = fallback_screen_check_head(a->token, req, resp);
    if (rc != FALLBACK_NOT_MINE) return rc;
    int i = find_route(req->path);
    if (i < 0) {
        resp_error(resp, 404, "not_found", "no such endpoint; see GET /api/v1/info");
        return -1;
    }
    if (!allowed(routes[i].allow, req->method)) {
        resp_error(resp, 405, "method_not_allowed", "this method is not supported on this path");
        snprintf(resp->extra_headers, sizeof resp->extra_headers, "Allow: %s\r\n", routes[i].allow);
        return -1;
    }
    if (!strcmp(req->path, "/api/v1/config/mqtt")) return mqtt_http_check_head(a->token, req, resp);
    return is_frame_put(req) ? frame_check_head(a->token, req, resp) : 0;
}

static void app_handle(void *ctx, const struct http_request *req, const uint8_t *body, size_t len,
                       struct response *resp) {
    struct app *a = ctx;
    resp->status = 200;
    if (!strcmp(req->path, "/api/v1/heartbeat")) {
        fallback_screen_handle(&a->fallback, resp);
    } else if (find_route(req->path) < 0) {
        panel_handle(&a->panel, req, body, len, resp);
    } else if (is_frame_put(req)) {
        frame_put(&a->frames, req, body, len, resp);
    } else if (!strcmp(req->path, "/api/v1/info")) {
        info_json(a, &resp->body);
    } else if (!strcmp(req->path, "/api/v1/state")) {
        state_json(a, &resp->body);
    } else if (!strcmp(req->path, "/api/v1/config/mqtt")) {
        mqtt_http_handle(&a->mqtt, req, body, len, resp);
    } else if (fallback_screen_showing(&a->fallback)) { /* never a server frame: no dedup, no persistence */
        if (!strcmp(req->path, "/api/v1/frame")) fallback_screen_frame_json(&a->fallback, &resp->body);
        else fallback_screen_image(&a->fallback, resp);
    } else if (!a->frames.have) {
        resp_error(resp, 404, "no_frame", "no frame has been shown since tt7d started");
    } else if (!strcmp(req->path, "/api/v1/frame")) {
        frame_json(&a->frames, &resp->body);
    } else { /* /api/v1/frame/image: the PNG exactly as it was received */
        resp->content_type = "image/png";
        sb_add(&resp->body, a->frames.png, a->frames.png_len);
    }
}

/* Failure telemetry (SPEC 42): every refused frame PUT, whoever refused it. */
static void app_on_reply(void *ctx, const struct http_request *req, const struct response *resp) {
    struct app *a = ctx;
    if (is_frame_put(req) && resp->status >= 400) {
        a->frames.rejected++;
        a->frames.last_error = resp->error;
    }
    if (is_frame_put(req) && resp->status == 200) {
        fallback_screen_frame_accepted(&a->fallback); /* a duplicate counts too: it is a heartbeat */
        mqtt_app_frame_accepted(&a->mqtt);
    }
    /* Brightness, blank, wake or the test pattern changed what MQTT reports. */
    if (!strncmp(req->path, "/api/v1/display/", 16) && resp->status == 200) mqtt_app_state_changed(&a->mqtt);
}

/* MQTT commands run the control panel's own actions (panel.h). */
static int mqtt_set_brightness(void *ctx, long value, int percent) {
    return panel_set_brightness(ctx, value, percent) == PANEL_OK ? 0 : -1;
}
static int mqtt_blank(void *ctx) { return panel_blank(ctx) == PANEL_OK ? 0 : -1; }
static int mqtt_wake(void *ctx) { return panel_wake(ctx) == PANEL_OK ? 0 : -1; }
static int mqtt_reboot(void *ctx) { return panel_reboot(ctx) == PANEL_OK ? 0 : -1; }

/* Poll entries: the MQTT socket first (fd -1 when there is none, which
 * poll() skips), then the input devices and WebSocket clients. The fallback
 * clock has no descriptor, only a deadline in *wait_ms. */
static int app_poll_prepare(void *ctx, struct pollfd *pfd, int max, int64_t *wait_ms) {
    struct app *a = ctx;
    mqtt_app_prepare(&a->mqtt, &pfd[0], wait_ms);
    fallback_screen_prepare(&a->fallback, wait_ms);
    return 1 + events_prepare(&a->events, pfd + 1, max - 1, wait_ms);
}

/* The fallback clock before the input, so an event's frame_id names what
 * is on screen after this wakeup's switch, if any. */
static void app_poll_service(void *ctx, const struct pollfd *pfd, int n) {
    struct app *a = ctx;
    mqtt_app_service(&a->mqtt, pfd[0].revents);
    fallback_screen_service(&a->fallback);
    events_service(&a->events, pfd + 1, n - 1);
}

static int app_take_over(void *ctx, int fd, const struct http_request *req) {
    return events_take_over(&((struct app *)ctx)->events, fd, req);
}

int main(int argc, char **argv) {
    static struct app a;
    if (parse_args(argc, argv, &a.cfg) != 0) {
        usage(stderr);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    clock_gettime(CLOCK_MONOTONIC, &a.started);
    fprintf(stderr, "tt7d %s (%s) starting\n", FIRMWARE_VERSION, TT7D_VERSION);

    char err[512];
    if (make_dirs(a.cfg.data_dir) != 0) {
        fprintf(stderr, "tt7d: cannot create %s: %s\n", a.cfg.data_dir, strerror(errno));
        return 1;
    }
    if (token_load(a.cfg.data_dir, a.token, sizeof a.token, err, sizeof err) != 0) {
        fprintf(stderr, "tt7d: %s\n", err);
        return 1;
    }
    if (device_id_load(a.cfg.data_dir, a.device_id, sizeof a.device_id, err, sizeof err) != 0)
        fprintf(stderr, "tt7d: device id unknown, reporting null: %s\n", err);

    int rc = a.cfg.fb_file ? display_open_file(&a.disp, a.cfg.fb_file, a.cfg.fb_geometry, a.cfg.fb_stride,
                                               a.cfg.fb_format, a.cfg.rotation, err, sizeof err)
                           : display_open_fb(&a.disp, a.cfg.fb, a.cfg.rotation, err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "tt7d: display: %s\n", err);
        return 1;
    }
    fprintf(stderr, "tt7d: %s native %ux%u %s stride %u, rotation %d -> logical %ux%u\n", a.disp.device,
            a.disp.back.width, a.disp.back.height, render_format_name(&a.disp.back), a.disp.back.stride,
            a.disp.rotation, a.disp.logical_w, a.disp.logical_h);

    frame_store_init(&a.frames, a.cfg.data_dir, &a.disp);
    rc = frame_restore(&a.frames, a.cfg.max_frame_bytes, err, sizeof err);
    if (rc < 0) fprintf(stderr, "tt7d: not restoring the last frame: %s\n", err);
    if (rc > 0) fprintf(stderr, "tt7d: restored last-frame.png (frame id %s)\n", a.frames.id[0] ? a.frames.id : "unknown");

    /* The fallback clock (SPEC 41.1). A restored frame counts as a frame, so
     * it stays up for one timeout before the clock replaces it. */
    char tz[64];
    if (timesync_tz_choose(a.cfg.tz, a.cfg.data_dir, tz, sizeof tz, err, sizeof err) != 0)
        fprintf(stderr, "tt7d: %s\n", err);
    timesync_tz_apply(tz);
    struct fallback_screen_config fc = {.timeout_s = a.cfg.fallback_timeout_s, .hour12 = a.cfg.clock_hour12,
                                        .marker = a.cfg.ntp_marker, .tz = tz};
    if (fallback_screen_init(&a.fallback, &fc, &a.disp, &a.frames, rc > 0, err, sizeof err) != 0) {
        fprintf(stderr, "tt7d: %s\n", err);
        return 1;
    }
    fallback_screen_service(&a.fallback); /* the clock goes up now, before HTTP and MQTT */

    a.panel = (struct panel){.sysfs_root = a.cfg.sysfs_root, .proc_root = a.cfg.proc_root, .data_dir = a.cfg.data_dir,
                             .log_file = a.cfg.log_file, .reboot_cmd = a.cfg.reboot_cmd, .token = a.token,
                             .firmware_version = FIRMWARE_VERSION, .build = TT7D_VERSION, .disp = &a.disp,
                             .frames = &a.frames, .ntp_marker = a.cfg.ntp_marker};
    panel_init(&a.panel);

    /* MQTT after the display is up: a broker problem never delays the screen.
     * cmd/reboot stays refused unless allow_reboot_cmd is set (mqtt.c). */
    struct mqtt_actions actions = {.ctx = &a.panel, .set_brightness = mqtt_set_brightness, .wake = mqtt_wake,
                                   .blank = mqtt_blank, .reboot = mqtt_reboot};
    if (mqtt_app_init(&a.mqtt, a.cfg.data_dir, a.cfg.sysfs_root, a.device_id, FIRMWARE_VERSION " (" TT7D_VERSION ")",
                      &a.frames, &actions, a.cfg.mqtt_flags, a.cfg.n_mqtt_flags, err, sizeof err) != 0) {
        fprintf(stderr, "tt7d: %s\n", err);
        return 2;
    }
    a.mqtt.screen = &a.fallback;

    /* Input after MQTT (button events go there too); like MQTT, a missing
     * device never stops the display. */
    events_init(&a.events, a.cfg.input_dir, a.cfg.sysfs_root, &a.disp, &a.frames, &a.fallback, &a.mqtt, a.token,
                a.device_id);

    struct server_config sc = {.listen = a.cfg.listen, .max_head = 8192, .max_body = a.cfg.max_frame_bytes,
                               .timeout_ms = a.cfg.timeout_ms, .max_connections = 8};
    struct server_handlers h = {.ctx = &a, .check_head = app_check_head, .handle = app_handle, .on_reply = app_on_reply,
                                .poll_prepare = app_poll_prepare, .poll_service = app_poll_service,
                                .take_over = app_take_over};
    server_run(&sc, &h, err, sizeof err);
    fprintf(stderr, "tt7d: %s\n", err);
    return 1;
}
