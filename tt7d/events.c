/* ABOUTME: Builds the touch/button event JSON (logical coordinates, frame_id, timestamps) and fans it out:
 * ABOUTME: every event to the WebSocket clients, button events also to MQTT. Also the auth for GET /api/v1/events. */
#include "events.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ident.h"

static int64_t mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ,"frame_id":...,"timestamp":...,"monotonic_ms":...} closing an event object. */
static void event_tail(const struct events *e, struct sbuf *sb, const struct timespec *wall, int64_t mono) {
    const struct frame_store *fs = e->frames;
    sb_puts(sb, ",\"frame_id\":");
    sb_json_str(sb, fs->have && fs->id[0] ? fs->id : NULL);
    sb_puts(sb, ",\"timestamp\":");
    sb_json_time(sb, wall);
    sb_printf(sb, ",\"monotonic_ms\":%lld}", (long long)mono);
}

static void on_touch(void *ctx, const struct input_device *d, const struct touch_out *o) {
    struct events *e = ctx;
    const struct display *disp = e->disp;
    uint32_t x, y;
    if (touch_to_logical(d->ax, d->ay, disp->rotation, disp->back.width, disp->back.height, o->x, o->y, &x, &y) != 0)
        return;
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    struct sbuf sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"type\":\"touch\",\"action\":\"%s\",\"pointer\":%d,\"x\":%u,\"y\":%u,\"nx\":%.4f,\"ny\":%.4f",
              touch_action_name(o->action), o->pointer, x, y, (double)x / disp->logical_w,
              (double)y / disp->logical_h);
    event_tail(e, &sb, &wall, mono_ms());
    if (!sb.oom) ws_hub_broadcast(&e->hub, sb.buf, sb.len, mono_ms());
    sb_free(&sb);
    e->last_touch = wall;
    e->have_last_touch = 1;
}

static void on_button(void *ctx, const struct input_device *d, int code, int pressed) {
    struct events *e = ctx;
    (void)d;
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    char buf[16];
    struct sbuf sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"type\":\"button\",\"button\":");
    sb_json_str(&sb, input_button_name((unsigned)code, buf, sizeof buf));
    sb_printf(&sb, ",\"action\":\"%s\",\"code\":%d", pressed ? "press" : "release", code);
    event_tail(e, &sb, &wall, mono_ms());
    if (!sb.oom) {
        ws_hub_broadcast(&e->hub, sb.buf, sb.len, mono_ms());
        mqtt_app_event(e->mqtt, "button", sb.buf); /* not retained; dropped while disconnected */
    }
    sb_free(&sb);
    e->last_button = wall;
    e->have_last_button = 1;
}

void events_init(struct events *e, const char *input_dir, const char *sysfs_root, const struct display *disp,
                 const struct frame_store *frames, struct mqtt_app *mqtt, const char *token, const char *device_id) {
    memset(e, 0, sizeof *e);
    e->disp = disp;
    e->frames = frames;
    e->mqtt = mqtt;
    e->token = token;
    e->device_id = device_id;
    ws_hub_init(&e->hub, WS_QUEUE_BYTES);
    struct input_sink sink = {.ctx = e, .touch = on_touch, .button = on_button};
    input_init(&e->input, input_dir, sysfs_root, &sink, stderr);
    input_scan(&e->input, mono_ms());
}

/* Browsers cannot put an Authorization header on a WebSocket, so the token
 * may also come as ?token=. The server logs only the path, never the query,
 * so the token stays out of the log either way. */
static int authorized(const struct events *e, const struct http_request *req, struct response *resp) {
    char qtoken[256];
    if (!http_header(req, "Authorization") && ws_query_param(req->query, "token", qtoken, sizeof qtoken) == 0) {
        int ok = token_equal(e->token, qtoken);
        memset(qtoken, 0, sizeof qtoken);
        if (ok) return 0;
        resp_error(resp, 401, "unauthorized", "this request needs Authorization: Bearer <token> or ?token=<token>");
        snprintf(resp->extra_headers, sizeof resp->extra_headers, "WWW-Authenticate: Bearer realm=\"tt7d\"\r\n");
        return -1;
    }
    return resp_require_bearer(e->token, req, resp);
}

int events_check_head(struct events *e, const struct http_request *req, struct response *resp) {
    if (strcmp(req->path, EVENTS_PATH) != 0) return EVENTS_NOT_MINE;
    if (strcmp(req->method, "GET") != 0) {
        resp_error(resp, 405, "method_not_allowed", "this method is not supported on this path");
        snprintf(resp->extra_headers, sizeof resp->extra_headers, "Allow: GET\r\n");
        return -1;
    }
    if (authorized(e, req, resp) != 0) return -1;
    const char *version = http_header(req, "Sec-WebSocket-Version");
    if (!ws_header_has_token(http_header(req, "Upgrade"), "websocket") ||
        !ws_header_has_token(http_header(req, "Connection"), "upgrade") || !version || strcmp(version, "13") != 0) {
        resp_error(resp, 426, "upgrade_required", "GET /api/v1/events is a WebSocket (RFC 6455, version 13)");
        snprintf(resp->extra_headers, sizeof resp->extra_headers,
                 "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n");
        return -1;
    }
    if (!ws_key_valid(http_header(req, "Sec-WebSocket-Key"))) {
        resp_error(resp, 400, "bad_request", "Sec-WebSocket-Key must be base64 of 16 bytes");
        return -1;
    }
    if (ws_hub_count(&e->hub) >= WS_MAX_CLIENTS) {
        resp_error(resp, 503, "too_many_clients", "every event stream slot is taken; try again later");
        return -1;
    }
    return 0;
}

/* The first message on a new stream: what the coordinates mean. */
static void hello_json(const struct events *e, struct sbuf *sb) {
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    sb_puts(sb, "{\"type\":\"hello\",\"device_id\":");
    sb_json_str(sb, e->device_id && e->device_id[0] ? e->device_id : NULL);
    sb_printf(sb, ",\"width\":%u,\"height\":%u,\"rotation\":%d,\"touch\":%s", e->disp->logical_w,
              e->disp->logical_h, e->disp->rotation, input_find(&e->input, INPUT_ROLE_TOUCH, 0) ? "true" : "false");
    event_tail(e, sb, &wall, mono_ms());
}

int events_take_over(struct events *e, int fd, const struct http_request *req) {
    if (strcmp(req->path, EVENTS_PATH) != 0) return 0; /* events_check_head() passed everything else */
    int sndbuf = EVENTS_SNDBUF;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    struct sbuf hello;
    sb_init(&hello);
    hello_json(e, &hello);
    if (hello.oom || ws_hub_add(&e->hub, fd, http_header(req, "Sec-WebSocket-Key"), hello.buf, mono_ms()) != 0) {
        static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        if (send(fd, busy, sizeof busy - 1, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) { /* closing anyway */
        }
        close(fd);
    }
    sb_free(&hello);
    return 1;
}

int events_prepare(struct events *e, struct pollfd *pfd, int max, int64_t *wait_ms) {
    int64_t now = mono_ms();
    e->n_input_fds = input_prepare(&e->input, pfd, max, wait_ms, now);
    return e->n_input_fds + ws_hub_prepare(&e->hub, pfd + e->n_input_fds, max - e->n_input_fds, wait_ms, now);
}

void events_service(struct events *e, const struct pollfd *pfd, int n) {
    int64_t now = mono_ms();
    int ni = e->n_input_fds < n ? e->n_input_fds : n;
    /* Clients first: one dropped here must not get the input's new events. */
    ws_hub_service(&e->hub, pfd + ni, n - ni, now);
    input_service(&e->input, pfd, ni, now);
}

void events_info_member(const struct events *e, struct sbuf *sb) {
    sb_puts(sb, "\"input\":{\"events\":\"" EVENTS_PATH "\",\"touch\":");
    const struct input_device *t = input_find(&e->input, INPUT_ROLE_TOUCH, 0);
    if (t) {
        sb_puts(sb, "{\"device\":");
        sb_json_str(sb, t->name);
        sb_printf(sb,
                  ",\"protocol\":\"%s\",\"pointers\":%d,\"raw\":{\"x\":{\"min\":%d,\"max\":%d},\"y\":{\"min\":%d,"
                  "\"max\":%d}},\"coordinates\":{\"width\":%u,\"height\":%u,\"space\":\"logical\"},"
                  "\"move_interval_ms\":%d}",
                  t->proto == TOUCH_MT_B ? "mt_b" : t->proto == TOUCH_MT_A ? "mt_a" : "single", t->touch.nslots,
                  t->ax.min, t->ax.max, t->ay.min, t->ay.max, e->disp->logical_w, e->disp->logical_h,
                  INPUT_THROTTLE_MS);
    } else {
        sb_puts(sb, "null");
    }
    /* Every key code the button devices can send, by name (SPEC 15: only real ones). */
    sb_puts(sb, ",\"buttons\":[");
    int first = 1;
    const struct input_device *b;
    for (int i = 0; (b = input_find(&e->input, INPUT_ROLE_BUTTONS, i)); i++)
        for (int k = 0; k < b->nkeys; k++) {
            char buf[16];
            if (!first) sb_puts(sb, ",");
            sb_json_str(sb, input_button_name((unsigned)b->keys[k], buf, sizeof buf));
            first = 0;
        }
    sb_puts(sb, "]}");
}

void events_state_member(const struct events *e, struct sbuf *sb) {
    sb_puts(sb, "\"input\":{\"last_touch\":");
    if (e->have_last_touch) sb_json_time(sb, &e->last_touch);
    else sb_puts(sb, "null");
    sb_puts(sb, ",\"last_button\":");
    if (e->have_last_button) sb_json_time(sb, &e->last_button);
    else sb_puts(sb, "null");
    sb_printf(sb, ",\"event_clients\":%d,\"event_clients_dropped_slow\":%lu}", ws_hub_count(&e->hub),
              e->hub.dropped_slow);
}
