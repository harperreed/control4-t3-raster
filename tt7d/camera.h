/* ABOUTME: tt7d's camera (owner-approved extension to SPEC 51): snapshots over HTTP and MQTT, presence that
 * ABOUTME: wakes the display, and supervision of the camera worker process. Off by default; plugs into main.c. */
#ifndef TT7D_CAMERA_H
#define TT7D_CAMERA_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "camera_config.h"
#include "events.h"
#include "http.h"
#include "json.h"
#include "mqtt.h"
#include "panel.h"
#include "presence.h"
#include "server.h"

#define CAMERA_SNAPSHOT_PATH "/api/v1/camera/snapshot"
#define CAMERA_CONFIG_PATH "/api/v1/config/camera"
#define CAMERA_NOT_MINE 1
#define CAMERA_MAX_WAITING 4       /* snapshot requests waiting for one capture */
#define CAMERA_KILL_GRACE_MS 1000  /* SIGTERM, then this long before SIGKILL */
#define CAMERA_BACKOFF_MAX_MS 60000

/* A snapshot request whose socket the camera took over (server.h take_over):
 * answered when the worker's JPEG arrives, or when it fails. */
struct camera_waiter {
    int fd; /* -1: free */
    int answered;
    struct sbuf out;
    size_t off;
    int64_t deadline_ms; /* to finish sending once answered */
};

enum camera_worker_state {
    CW_OFF,        /* no worker: camera disabled or no device */
    CW_RUNNING,
    CW_STOPPING,   /* SIGTERM sent (SIGKILL after the grace time); waiting to reap it */
    CW_RESTARTING, /* the last one failed: a new one starts at restart_at_ms */
};

enum camera_stop_reason { STOP_SETTINGS, STOP_TIMEOUT, STOP_BROKEN };

struct camera_init_args {
    const char *data_dir;
    const char *device;      /* --camera-dev, default /dev/video0 */
    const char *fake_source; /* --camera-fake-source: TEST ONLY, NULL on the panel */
    int flag_enabled;        /* --camera on|off: 1 or 0; -1 when not given */
    const char *sysfs_root;
    const char *token;
    struct panel *panel;   /* wake/blank for presence */
    struct mqtt_app *mqtt; /* image, presence, discovery, cmd/snapshot */
    struct events *events; /* the WebSocket presence events */
};

struct camera {
    char data_dir[256];
    const char *device, *fake_source, *sysfs_root, *token;
    struct panel *panel;
    struct mqtt_app *mqtt;
    struct events *events;
    struct camera_config cfg;      /* in effect: camera.conf, then the flag */
    struct camera_config file_cfg; /* what camera.conf holds (PUT edits this) */
    unsigned flag_fields;          /* enum camera_field bits set by --camera */
    int flag_enabled;
    char conf_error[240];

    /* The worker process. */
    enum camera_worker_state wstate;
    pid_t pid;
    int sock; /* our end of the socketpair, -1 when closed */
    struct camera_config running; /* the settings it was started with */
    enum camera_stop_reason stop_reason;
    int64_t sigkill_at_ms; /* while stopping; 0 once SIGKILL went out */
    int64_t stuck_logged_ms;
    int64_t restart_at_ms, backoff_ms;
    int64_t alive_deadline_ms; /* it must say something by then (0: nothing expected) */
    uint8_t *in;
    size_t in_len, in_cap;
    unsigned long worker_restarts;
    char last_error[256];

    /* Snapshots: the latest JPEG, in memory only. */
    uint8_t *jpeg;
    size_t jpeg_len;
    int64_t jpeg_ms;
    struct timespec jpeg_wall;
    int requested;  /* a CMSG_SNAPSHOT is out */
    int mqtt_wants; /* publish the next JPEG to camera/image */
    int64_t next_periodic_ms;
    struct camera_waiter waiters[CAMERA_MAX_WAITING];
    int poll_waiter[CAMERA_MAX_WAITING]; /* waiter index of each poll entry after the socket's */
    int npoll_waiters;

    /* Presence. */
    int present;
    float score;
    unsigned long ticks;
    struct timespec present_changed;
    int have_present_changed;
    struct presence_ctl pctl;
};

/* Load <data_dir>/camera.conf, apply the --camera flag, and start the
 * worker if the camera is enabled and its device exists. A bad camera.conf
 * leaves the camera off, with the reason in /state. */
void camera_init(struct camera *c, const struct camera_init_args *a);

/* Head-time routing for GET /api/v1/camera/snapshot and GET/PUT
 * /api/v1/config/camera (all need the bearer token). Returns
 * CAMERA_NOT_MINE for other paths, 0 to go on, or -1 with resp filled. */
int camera_check_head(struct camera *c, const struct http_request *req, struct response *resp);

/* server_handlers.take_over: 1 if the snapshot request now waits for a
 * capture (the camera answers it later); 0 to let camera_handle answer now
 * (from the cache, or with an error). */
int camera_take_over(struct camera *c, int fd, const struct http_request *req);

void camera_handle(struct camera *c, const struct http_request *req, const uint8_t *body, size_t len,
                   struct response *resp);

/* The poll loop hooks (see server_handlers). */
int camera_prepare(struct camera *c, struct pollfd *pfd, int max, int64_t *wait_ms);
void camera_service(struct camera *c, const struct pollfd *pfd, int n);

/* Before tt7d exits (an update restart): stop the worker the normal way
 * (SIGTERM, so it closes the camera: STREAMOFF, ION free), SIGKILL after
 * CAMERA_KILL_GRACE_MS, and reap it, so it never outlives tt7d holding the
 * camera the next tt7d wants. Blocks for at most about the grace time. */
void camera_shutdown(struct camera *c);

/* The "camera" member of /info capabilities and of /state (key included, no comma). */
void camera_info_member(const struct camera *c, struct sbuf *sb);
void camera_state_member(const struct camera *c, struct sbuf *sb);

#endif
