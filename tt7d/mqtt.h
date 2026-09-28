/* ABOUTME: tt7d's MQTT integration: availability, state and sensor telemetry, events, commands,
 * ABOUTME: Home Assistant discovery, and GET/PUT /api/v1/config/mqtt. Plugs into the HTTP server's poll loop. */
#ifndef TT7D_MQTT_H
#define TT7D_MQTT_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "frame.h"
#include "http.h"
#include "json.h"
#include "mqtt_client.h"
#include "mqtt_config.h"
#include "server.h"

/* Display operations that MQTT commands map to. Each is optional: a NULL
 * one makes its command answer "unsupported_command" and keeps its Home
 * Assistant entity out of discovery. They return 0 on success. The control
 * panel milestone (M4) owns the backlight and reboot code and wires these
 * up in main.c; MQTT never touches sysfs itself. */
struct mqtt_actions {
    void *ctx;
    int (*set_brightness)(void *ctx, int percent); /* 0-100 */
    int (*wake)(void *ctx);
    int (*blank)(void *ctx);
    int (*reboot)(void *ctx);
};

struct mqtt_app {
    struct mqtt_config cfg;      /* in effect: mqtt.conf, then the flags on top */
    struct mqtt_config file_cfg; /* what mqtt.conf holds (PUT edits this) */
    unsigned flag_fields;        /* bitmask of enum mqtt_field set by --mqtt-* flags */
    const char *const *flag_kv;  /* those flags as key, value pairs (argv) */
    int nflags;
    char conf_error[240]; /* why mqtt.conf was not used, or "" */
    char data_dir[256];
    const char *sysfs_root;
    char device_id[32];
    const char *sw_version;
    const struct frame_store *frames;
    struct mqtt_actions actions;
    struct mqtt_client client;
    char base[160]; /* "<prefix>/<device id>" */
    unsigned long config_revision;
    int boot_sent;
    int64_t next_check_ms, next_state_ms;
    char last_signature[512];
    int have_last_publish;
    struct timespec last_publish;
};

/* Load <data_dir>/mqtt.conf, apply the --mqtt-* flags (pairs of key and
 * value in flag_kv), read the password file, and start connecting if a
 * broker is configured. A bad flag is fatal (returns -1 with err); a bad
 * mqtt.conf only leaves MQTT off, with the reason in /api/v1/state. */
int mqtt_app_init(struct mqtt_app *m, const char *data_dir, const char *sysfs_root, const char *device_id,
                  const char *sw_version, const struct frame_store *frames, const struct mqtt_actions *actions,
                  const char *const *flag_kv, int nflags, char *err, size_t errlen);

/* The poll loop hooks (see server_handlers). */
void mqtt_app_prepare(struct mqtt_app *m, struct pollfd *pfd, int64_t *wait_ms);
void mqtt_app_service(struct mqtt_app *m, short revents);

/* A frame was accepted (PUT /api/v1/frame answered 200): event/frame. */
void mqtt_app_frame_accepted(struct mqtt_app *m);

/* Publish tt7/<id>/event/<type> (not retained) with a JSON object payload.
 * For physical button events (M3): mqtt_app_event(m, "button", "{...}").
 * Touch events do not go over MQTT (the owner chose a WebSocket). Dropped when
 * not connected. */
void mqtt_app_event(struct mqtt_app *m, const char *type, const char *json);

/* The "mqtt" member of /api/v1/state (key included, no comma). */
void mqtt_app_state_member(struct mqtt_app *m, struct sbuf *sb);

/* GET and PUT /api/v1/config/mqtt. check_head does the token and body checks. */
int mqtt_http_check_head(const char *token, const struct http_request *req, struct response *resp);
void mqtt_http_handle(struct mqtt_app *m, const struct http_request *req, const uint8_t *body, size_t len,
                      struct response *resp);

/* ---- pure helpers, unit tested ---- */

/* "<prefix>/<device_id>/<leaf>" into out. Returns 0, or -1 if it does not fit. */
int mqtt_topic(char *out, size_t n, const char *prefix, const char *device_id, const char *leaf);

/* "homeassistant/<component>/<device_id>/<object>/config". */
int mqtt_discovery_topic(char *out, size_t n, const char *component, const char *device_id, const char *object);

enum mqtt_command { CMD_NONE, CMD_BRIGHTNESS, CMD_WAKE, CMD_BLANK, CMD_REBOOT };

/* Which command a topic is, given the "<prefix>/<device_id>" base. */
enum mqtt_command mqtt_command_of(const char *topic, const char *base);

/* A cmd/brightness payload: "NN%" is a percentage (0-100), a plain "NN" a
 * raw backlight level (0 to backlight_max, 255 on the TT7). Surrounding
 * spaces are fine. Returns 0 with *percent set, or -1. */
int mqtt_parse_brightness(const uint8_t *payload, size_t len, int backlight_max, int *percent);

#endif
