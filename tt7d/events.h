/* ABOUTME: tt7d's input events (SPEC 14-16): touch and button events as JSON on the WebSocket GET /api/v1/events,
 * ABOUTME: buttons also on MQTT event/button (touch never goes to MQTT). Glue between input.c, ws.c and main.c. */
#ifndef TT7D_EVENTS_H
#define TT7D_EVENTS_H

#include <poll.h>
#include <stdint.h>
#include <time.h>

#include "display.h"
#include "fallback_screen.h"
#include "frame.h"
#include "http.h"
#include "input.h"
#include "json.h"
#include "mqtt.h"
#include "server.h"
#include "ws.h"

#define EVENTS_PATH "/api/v1/events"
#define EVENTS_NOT_MINE 1
#define EVENTS_SNDBUF 32768 /* kernel send buffer per WebSocket client, on top of its WS_QUEUE_BYTES queue */

struct events {
    struct input input;
    struct ws_hub hub;
    const struct display *disp;
    const struct frame_store *frames;
    const struct fallback_screen *screen; /* NULL = no fallback clock */
    struct mqtt_app *mqtt;
    const char *token;
    const char *device_id;
    int n_input_fds; /* the first n_input_fds of our poll entries are input devices, the rest WebSocket clients */
    int have_last_touch, have_last_button;
    struct timespec last_touch, last_button; /* wall clock */
};

/* Open the input devices (input_dir holds the eventN nodes; sysfs_root
 * names them) and get ready for WebSocket clients. Events carry the frame_id
 * of what is on screen: `screen`'s clock face when it shows, else the frame
 * store's frame. Never fails: missing devices are looked for again every
 * few seconds. */
void events_init(struct events *e, const char *input_dir, const char *sysfs_root, const struct display *disp,
                 const struct frame_store *frames, const struct fallback_screen *screen, struct mqtt_app *mqtt,
                 const char *token, const char *device_id);

/* Head-time checks for GET /api/v1/events. Returns EVENTS_NOT_MINE for any
 * other path, 0 to go on (to events_take_over), or -1 with resp filled:
 * 405, 401 (no or wrong token: "Authorization: Bearer" or ?token=), 426 (not
 * a version 13 WebSocket upgrade), 400 (bad Sec-WebSocket-Key), 503 (all
 * WS_MAX_CLIENTS slots taken). */
int events_check_head(struct events *e, const struct http_request *req, struct response *resp);

/* server_handlers.take_over: 1 if the socket became a WebSocket client. */
int events_take_over(struct events *e, int fd, const struct http_request *req);

/* The poll loop hooks. */
int events_prepare(struct events *e, struct pollfd *pfd, int max, int64_t *wait_ms);
void events_service(struct events *e, const struct pollfd *pfd, int n);

/* The "input" member of /info capabilities and of /state (key included, no comma). */
void events_info_member(const struct events *e, struct sbuf *sb);
void events_state_member(const struct events *e, struct sbuf *sb);

#endif
