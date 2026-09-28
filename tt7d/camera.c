/* ABOUTME: The camera in tt7d's poll loop: starts, watches and kills the worker process, caches its JPEGs in
 * ABOUTME: memory, answers the snapshot/config routes, and fans presence out to WebSocket, MQTT and the display. */
#include "camera.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "camera_proto.h"
#include "camera_worker.h"
#include "ident.h"
#include "sysinfo.h"

#define MAX_CONFIG_BODY 4096
#define SEND_TIMEOUT_MS 30000 /* to send a snapshot reply once it is ready */
#define STUCK_LOG_MS 5000     /* after SIGKILL, say once that the worker has not exited */

static int64_t mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void set_error(struct camera *c, const char *msg) {
    snprintf(c->last_error, sizeof c->last_error, "%s", msg);
    fprintf(stderr, "tt7d: camera: %s\n", msg);
}

/* The device node the worker would open (or, in the tests, the fake source) exists. */
static int camera_available(const struct camera *c) {
    struct stat st;
    if (c->fake_source) return stat(c->fake_source, &st) == 0;
    return stat(c->device, &st) == 0 && S_ISCHR(st.st_mode);
}

static int snapshot_fresh(const struct camera *c, int64_t now) {
    return c->jpeg && now - c->jpeg_ms < (int64_t)c->cfg.snapshot_max_age_s * 1000;
}

static void drop_snapshot(struct camera *c) {
    free(c->jpeg);
    c->jpeg = NULL;
    c->jpeg_len = 0;
}

/* ---- presence ---- */

static int display_blank(const struct camera *c) {
    int on, pct;
    sysinfo_backlight(c->sysfs_root, &on, &pct);
    return on == 0;
}

static void apply_presence_action(struct camera *c, enum presence_action a) {
    if (a == PRESENCE_WAKE && panel_wake(c->panel) != PANEL_OK) fprintf(stderr, "tt7d: camera: presence wake failed\n");
    if (a == PRESENCE_BLANK && panel_blank(c->panel) != PANEL_OK)
        fprintf(stderr, "tt7d: camera: presence idle blank failed\n");
    if (a != PRESENCE_NOTHING) mqtt_app_state_changed(c->mqtt);
}

static struct presence_policy policy(const struct camera *c) {
    struct presence_policy p = {.wake = c->cfg.presence_wake, .idle_blank_s = c->cfg.presence_idle_blank_s};
    return p;
}

static void publish_presence(struct camera *c) {
    if (!mqtt_app_connected(c->mqtt)) return;
    /* Retained, so Home Assistant sees the state after a restart. An empty
     * retained payload clears it while presence is off. */
    const char *v = c->cfg.enabled && c->cfg.presence ? (c->present ? "ON" : "OFF") : "";
    mqtt_app_publish(c->mqtt, "presence", v, strlen(v), 1);
}

/* Presence changed: WebSocket event, MQTT, and the display. */
static void presence_changed(struct camera *c, int present) {
    c->present = present;
    clock_gettime(CLOCK_REALTIME, &c->present_changed);
    c->have_present_changed = 1;
    struct sbuf sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"type\":\"presence\",\"present\":%s,\"score\":%.2f,\"timestamp\":", present ? "true" : "false",
              (double)c->score);
    sb_json_time(&sb, &c->present_changed);
    sb_printf(&sb, ",\"monotonic_ms\":%lld}", (long long)mono_ms());
    if (!sb.oom) ws_hub_broadcast(&c->events->hub, sb.buf, sb.len, mono_ms());
    sb_free(&sb);
    publish_presence(c);
    struct presence_policy p = policy(c);
    apply_presence_action(c, presence_update(&c->pctl, &p, present, display_blank(c), mono_ms()));
}

/* ---- snapshot replies ---- */

static void snapshot_reply(const struct camera *c, struct response *resp) {
    resp->status = 200;
    resp->content_type = "image/jpeg";
    sb_add(&resp->body, c->jpeg, c->jpeg_len);
    struct sbuf t;
    sb_init(&t);
    sb_json_time(&t, &c->jpeg_wall); /* "2026-...Z" with quotes: drop them */
    if (t.buf && t.len > 2)
        snprintf(resp->extra_headers, sizeof resp->extra_headers, "X-Captured-At: %.*s\r\n", (int)t.len - 2, t.buf + 1);
    sb_free(&t);
}

/* Answer every waiting request: with the JPEG (code NULL) or an error. */
static void answer_waiters(struct camera *c, int status, const char *code, const char *message) {
    int64_t now = mono_ms();
    for (int i = 0; i < CAMERA_MAX_WAITING; i++) {
        struct camera_waiter *w = &c->waiters[i];
        if (w->fd < 0 || w->answered) continue;
        struct response resp = {0};
        if (!code) snapshot_reply(c, &resp);
        else resp_error(&resp, status, code, message);
        server_format_reply(&resp, &w->out);
        if (code) fprintf(stderr, "tt7d: GET " CAMERA_SNAPSHOT_PATH " -> %d %s\n", status, code);
        sb_free(&resp.body);
        w->answered = 1;
        w->off = 0;
        w->deadline_ms = now + SEND_TIMEOUT_MS;
    }
}

static void close_waiter(struct camera_waiter *w) {
    shutdown(w->fd, SHUT_WR);
    close(w->fd);
    sb_free(&w->out);
    memset(w, 0, sizeof *w);
    w->fd = -1;
}

static void mqtt_error_event(struct camera *c, const char *code, const char *message) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct sbuf sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"type\":\"error\",\"error\":");
    sb_json_str(&sb, code);
    sb_puts(&sb, ",\"command\":\"snapshot\",\"message\":");
    sb_json_str(&sb, message);
    sb_puts(&sb, ",\"timestamp\":");
    sb_json_time(&sb, &now);
    sb_puts(&sb, "}");
    if (!sb.oom) mqtt_app_event(c->mqtt, "error", sb.buf);
    sb_free(&sb);
}

static void publish_image(struct camera *c) {
    c->mqtt_wants = 0;
    if (mqtt_app_publish(c->mqtt, "camera/image", c->jpeg, c->jpeg_len, 0) != 0)
        fprintf(stderr, "tt7d: camera: snapshot (%zu bytes) not published: MQTT not connected or queue full\n",
                c->jpeg_len);
}

/* Everyone waiting for the capture that just failed. */
static void snapshot_failed(struct camera *c, const char *code, const char *message) {
    c->requested = 0;
    answer_waiters(c, 503, code, message);
    if (c->mqtt_wants) mqtt_error_event(c, code, message);
    c->mqtt_wants = 0;
}

/* ---- the worker ---- */

static void close_other_fds(int keep) {
    /* /proc/self/fd, not a loop to OPEN_MAX: that can be very large. */
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        for (int fd = 3; fd < 1024; fd++)
            if (fd != keep) close(fd);
        return;
    }
    int dfd = dirfd(d);
    struct dirent *e;
    while ((e = readdir(d))) {
        int fd = atoi(e->d_name);
        if (e->d_name[0] != '.' && fd > 2 && fd != keep && fd != dfd) close(fd);
    }
    closedir(d);
}

static void start_worker(struct camera *c) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        set_error(c, "socketpair for the worker failed");
        return;
    }
    struct camera_worker_config w = {.device = c->device, .fake_source = c->fake_source,
                                     .presence = c->cfg.presence, .interval_ms = c->cfg.presence_interval_ms,
                                     .threshold = c->cfg.presence_threshold, .settle_frames = c->cfg.settle_frames};
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        set_error(c, "fork for the worker failed");
        c->wstate = CW_RESTARTING;
        c->restart_at_ms = mono_ms() + 1000;
        return;
    }
    if (pid == 0) {
        /* A process of its own, so a camera driver hang or crash cannot take
         * the display or the HTTP server down with it. Nothing of tt7d's
         * (listening socket, clients, broker connection) stays open here. */
        close_other_fds(sv[1]);
        camera_worker_run(sv[1], &w);
    }
    close(sv[1]);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    c->pid = pid;
    c->sock = sv[0];
    c->in_len = 0;
    c->running = c->cfg;
    c->wstate = CW_RUNNING;
    c->requested = 0;
    c->ticks = 0;
    c->alive_deadline_ms = c->cfg.presence ? mono_ms() + (int64_t)c->cfg.worker_timeout_s * 1000 : 0;
    fprintf(stderr, "tt7d: camera: worker %d started (%s, %s%s)\n", (int)pid,
            c->cfg.presence ? "presence" : "snapshots on request", c->fake_source ? "TEST fake source " : "",
            c->fake_source ? c->fake_source : c->device);
}

static void close_sock(struct camera *c) {
    if (c->sock >= 0) close(c->sock);
    c->sock = -1;
}

/* Presence is unknown without a presence worker: report it gone. */
static void presence_stopped(struct camera *c) {
    if (c->present) presence_changed(c, 0);
    c->ticks = 0;
}

static void stop_worker(struct camera *c, enum camera_stop_reason why) {
    if (c->wstate != CW_RUNNING) return;
    c->stop_reason = why;
    kill(c->pid, SIGTERM); /* the worker closes the camera (STREAMOFF, ion free) and exits */
    close_sock(c);         /* and the socket's EOF tells it too */
    c->wstate = CW_STOPPING;
    c->sigkill_at_ms = mono_ms() + CAMERA_KILL_GRACE_MS;
    c->stuck_logged_ms = 0;
    c->alive_deadline_ms = 0;
    presence_stopped(c);
    if (c->requested) snapshot_failed(c, why == STOP_SETTINGS ? "camera_busy" : "camera_timeout",
                                      why == STOP_SETTINGS ? "the camera settings changed; try again"
                                                           : "the camera worker stopped responding");
}

static void schedule_restart(struct camera *c) {
    c->backoff_ms = c->backoff_ms ? c->backoff_ms * 2 : 1000;
    if (c->backoff_ms > CAMERA_BACKOFF_MAX_MS) c->backoff_ms = CAMERA_BACKOFF_MAX_MS;
    c->wstate = CW_RESTARTING;
    c->restart_at_ms = mono_ms() + c->backoff_ms;
    c->worker_restarts++;
}

/* waitpid without blocking; handle a worker that exited. */
static void reap(struct camera *c) {
    if (c->wstate != CW_RUNNING && c->wstate != CW_STOPPING) return;
    int status;
    pid_t r = waitpid(c->pid, &status, WNOHANG);
    if (r != c->pid) return;
    int clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!clean || c->wstate == CW_RUNNING || c->stop_reason != STOP_SETTINGS) {
        char how[64];
        if (WIFSIGNALED(status)) snprintf(how, sizeof how, "killed by signal %d", WTERMSIG(status));
        else snprintf(how, sizeof how, "exit status %d", WEXITSTATUS(status));
        fprintf(stderr, "tt7d: camera: worker %d reaped (%s)\n", (int)c->pid, how);
    }
    int expected = c->wstate == CW_STOPPING && c->stop_reason == STOP_SETTINGS;
    c->pid = 0;
    close_sock(c);
    presence_stopped(c);
    if (c->requested) snapshot_failed(c, "camera_failed", c->last_error[0] ? c->last_error : "the camera worker exited");
    if (expected) {
        c->wstate = CW_OFF; /* camera_apply_settings starts the next one, if any */
    } else {
        if (c->wstate == CW_RUNNING && !c->last_error[0]) set_error(c, "the camera worker exited unexpectedly");
        schedule_restart(c);
    }
}

/* Start, stop or restart the worker to match c->cfg. */
static void sync_worker(struct camera *c) {
    int want = c->cfg.enabled && camera_available(c);
    const struct camera_config *r = &c->running, *n = &c->cfg;
    int changed = r->presence != n->presence || r->presence_interval_ms != n->presence_interval_ms ||
                  r->presence_threshold != n->presence_threshold || r->settle_frames != n->settle_frames;
    if (c->wstate == CW_RUNNING && (!want || changed)) stop_worker(c, STOP_SETTINGS);
    if (!want && c->wstate == CW_RESTARTING) c->wstate = CW_OFF;
    if (want && c->wstate == CW_OFF) start_worker(c);
}

static int request_snapshot(struct camera *c) {
    if (c->wstate != CW_RUNNING || c->sock < 0) return -1;
    if (c->requested) return 0;
    uint8_t h[CAMERA_MSG_HEADER];
    camera_msg_header(h, CMSG_SNAPSHOT, 0);
    if (send(c->sock, h, sizeof h, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)sizeof h) return -1;
    c->requested = 1;
    if (!c->running.presence) c->alive_deadline_ms = mono_ms() + (int64_t)c->cfg.worker_timeout_s * 1000;
    return 0;
}

static void on_jpeg(struct camera *c, const uint8_t *data, uint32_t len) {
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) {
        snapshot_failed(c, "camera_failed", "out of memory for the snapshot");
        return;
    }
    memcpy(copy, data, len);
    drop_snapshot(c);
    c->jpeg = copy;
    c->jpeg_len = len;
    c->jpeg_ms = mono_ms();
    clock_gettime(CLOCK_REALTIME, &c->jpeg_wall);
    c->requested = 0;
    c->backoff_ms = 0; /* the worker works */
    c->last_error[0] = 0;
    answer_waiters(c, 200, NULL, NULL);
    if (c->mqtt_wants) publish_image(c);
}

static void on_message(struct camera *c, uint32_t type, const uint8_t *payload, uint32_t len) {
    if (c->running.presence) c->alive_deadline_ms = mono_ms() + (int64_t)c->cfg.worker_timeout_s * 1000;
    else if (type == CMSG_JPEG || type == CMSG_ERROR) c->alive_deadline_ms = 0;
    if (type == CMSG_TICK) {
        struct camera_tick t;
        memcpy(&t, payload, sizeof t);
        c->ticks++;
        c->score = t.score;
        c->backoff_ms = 0;
        if (!!t.present != c->present) presence_changed(c, !!t.present);
    } else if (type == CMSG_JPEG) {
        on_jpeg(c, payload, len);
    } else if (type == CMSG_ERROR) {
        char msg[200];
        snprintf(msg, sizeof msg, "worker: %.*s", (int)(len < 180 ? len : 180), (const char *)payload);
        set_error(c, msg);
        if (c->requested) snapshot_failed(c, "camera_failed", c->last_error);
    }
}

static void read_worker(struct camera *c) {
    for (;;) {
        if (c->in_cap - c->in_len < 65536) {
            size_t cap = c->in_cap ? c->in_cap * 2 : 262144;
            if (cap > CAMERA_MSG_HEADER + CAMERA_MSG_MAX_PAYLOAD + 65536) cap = CAMERA_MSG_HEADER + CAMERA_MSG_MAX_PAYLOAD + 65536;
            uint8_t *grown = cap > c->in_cap ? realloc(c->in, cap) : c->in;
            if (!grown) break;
            c->in = grown;
            c->in_cap = cap;
        }
        ssize_t n = recv(c->sock, c->in + c->in_len, c->in_cap - c->in_len, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) { /* the worker is exiting; reap() takes it from here */
            close_sock(c);
            return;
        }
        c->in_len += (size_t)n;
        size_t off = 0;
        for (;;) {
            uint32_t type, plen;
            const uint8_t *payload;
            long used = camera_msg_parse(c->in + off, c->in_len - off, &type, &payload, &plen);
            if (used == 0) break;
            if (used < 0) {
                set_error(c, "the camera worker sent garbage; restarting it");
                stop_worker(c, STOP_BROKEN);
                return;
            }
            on_message(c, type, payload, plen);
            if (c->sock < 0) return; /* a handler stopped the worker */
            off += (size_t)used;
        }
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
}

/* Deadlines: a silent worker, SIGKILL after the grace time, restarts. */
static void supervise(struct camera *c, int64_t now) {
    reap(c);
    if (c->wstate == CW_RUNNING && c->alive_deadline_ms && now >= c->alive_deadline_ms) {
        char msg[160];
        snprintf(msg, sizeof msg, "worker %d gave no sign of life for %d s; stopping it (SIGTERM)", (int)c->pid,
                 c->cfg.worker_timeout_s);
        set_error(c, msg);
        stop_worker(c, STOP_TIMEOUT);
    }
    if (c->wstate == CW_STOPPING && c->sigkill_at_ms && now >= c->sigkill_at_ms) {
        fprintf(stderr, "tt7d: camera: worker %d still running %d ms after SIGTERM; SIGKILL\n", (int)c->pid,
                CAMERA_KILL_GRACE_MS);
        kill(c->pid, SIGKILL);
        c->sigkill_at_ms = 0;
        c->stuck_logged_ms = now;
    }
    if (c->wstate == CW_STOPPING && !c->sigkill_at_ms && c->stuck_logged_ms > 0 &&
        now - c->stuck_logged_ms >= STUCK_LOG_MS) {
        char msg[200];
        snprintf(msg, sizeof msg, "worker %d has not exited after SIGKILL (stuck in the camera driver?); "
                 "no new worker until it does", (int)c->pid);
        set_error(c, msg);
        c->stuck_logged_ms = -1; /* once */
    }
    reap(c);
    if (c->wstate == CW_RESTARTING && now >= c->restart_at_ms) {
        c->wstate = CW_OFF;
        sync_worker(c);
    }
    if (c->wstate == CW_OFF) sync_worker(c); /* a device that appeared, or a stop that finished */
}

/* ---- settings ---- */

static void conf_path(const struct camera *c, char *out, size_t n) { snprintf(out, n, "%s/camera.conf", c->data_dir); }

static void effective(struct camera *c) {
    c->cfg = c->file_cfg;
    if (c->flag_enabled >= 0) c->cfg.enabled = c->flag_enabled;
}

/* Act on a settings change (or the first load). */
static void apply_settings(struct camera *c, const struct camera_config *old) {
    if (!c->cfg.enabled) {
        drop_snapshot(c); /* off means no picture is kept, even in memory */
        c->next_periodic_ms = 0;
    }
    sync_worker(c);
    if (!c->cfg.enabled || !c->cfg.presence) presence_stopped(c);
    if (!old || old->enabled != c->cfg.enabled || old->presence != c->cfg.presence) {
        mqtt_app_resync_discovery(c->mqtt);
        publish_presence(c);
    }
    if (!old || old->snapshot_interval_s != c->cfg.snapshot_interval_s) c->next_periodic_ms = 0;
}

/* ---- MQTT hooks ---- */

static void ext_discovery(void *ctx, struct mqtt_app *m, const char *device_id, int announce) {
    struct camera *c = ctx;
    char topic[256];
    struct sbuf sb;
    /* https://www.home-assistant.io/integrations/camera.mqtt/ (read 2026-09-28): `topic`
     * carries the image as raw bytes unless image_encoding is "b64". */
    sb_init(&sb);
    if (announce && c->cfg.enabled) {
        snprintf(topic, sizeof topic, "%s/camera/image", m->base);
        sb_puts(&sb, "{\"name\":\"Camera\",\"topic\":");
        sb_json_str(&sb, topic);
        mqtt_app_discovery_common(m, "camera", &sb);
        sb_puts(&sb, "}");
    }
    mqtt_app_publish_discovery(m, "camera", device_id, "camera", sb.buf && !sb.oom ? sb.buf : "");
    sb_free(&sb);
    /* binary_sensor.mqtt: state_topic with payload_on/off (defaults ON/OFF);
     * device class occupancy: "on means occupied (detected)". */
    sb_init(&sb);
    if (announce && c->cfg.enabled && c->cfg.presence) {
        snprintf(topic, sizeof topic, "%s/presence", m->base);
        sb_puts(&sb, "{\"name\":\"Presence\",\"state_topic\":");
        sb_json_str(&sb, topic);
        sb_puts(&sb, ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"device_class\":\"occupancy\"");
        mqtt_app_discovery_common(m, "presence", &sb);
        sb_puts(&sb, "}");
    }
    mqtt_app_publish_discovery(m, "binary_sensor", device_id, "presence", sb.buf && !sb.oom ? sb.buf : "");
    sb_free(&sb);
}

static void ext_connected(void *ctx, struct mqtt_app *m) {
    (void)m;
    struct camera *c = ctx;
    publish_presence(c);
    c->next_periodic_ms = 0;
}

/* A snapshot for MQTT: the cached one if fresh, else the next capture. */
static void mqtt_snapshot(struct camera *c) {
    if (!c->cfg.enabled) {
        mqtt_error_event(c, "camera_disabled", "the camera is off; PUT /api/v1/config/camera {\"enabled\": true}");
        return;
    }
    if (snapshot_fresh(c, mono_ms())) {
        publish_image(c);
        return;
    }
    c->mqtt_wants = 1;
    if (request_snapshot(c) != 0) {
        c->mqtt_wants = 0;
        mqtt_error_event(c, "camera_busy", "the camera worker is not running; try again");
    }
}

static int ext_command(void *ctx, struct mqtt_app *m, const char *name, const uint8_t *payload, size_t len) {
    (void)m;
    (void)payload;
    (void)len;
    if (strcmp(name, "snapshot") != 0) return -1;
    mqtt_snapshot(ctx);
    return 0;
}

/* ---- setup ---- */

static char *read_text(const char *path, size_t max) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    char *buf = malloc(max + 1);
    size_t got = 0;
    ssize_t r = 0;
    while (buf && got < max && (r = read(fd, buf + got, max - got)) > 0) got += (size_t)r;
    close(fd);
    if (!buf || r < 0) {
        free(buf);
        return NULL;
    }
    buf[got] = 0;
    return buf;
}

void camera_init(struct camera *c, const struct camera_init_args *a) {
    memset(c, 0, sizeof *c);
    snprintf(c->data_dir, sizeof c->data_dir, "%s", a->data_dir);
    c->device = a->device;
    c->fake_source = a->fake_source;
    c->sysfs_root = a->sysfs_root;
    c->token = a->token;
    c->panel = a->panel;
    c->mqtt = a->mqtt;
    c->events = a->events;
    c->flag_enabled = a->flag_enabled;
    if (a->flag_enabled >= 0) c->flag_fields |= 1u << CAMF_ENABLED;
    c->sock = -1;
    for (int i = 0; i < CAMERA_MAX_WAITING; i++) c->waiters[i].fd = -1;
    presence_ctl_init(&c->pctl);
    if (c->fake_source) fprintf(stderr, "tt7d: camera: TEST MODE: frames come from %s, not the camera\n", c->fake_source);

    camera_config_defaults(&c->file_cfg);
    char path[512];
    conf_path(c, path, sizeof path);
    char *text = read_text(path, 16384);
    if (text) {
        struct camera_config parsed = c->file_cfg;
        char msg[160];
        if (camera_config_parse(&parsed, text, msg, sizeof msg) == 0) c->file_cfg = parsed;
        else snprintf(c->conf_error, sizeof c->conf_error, "camera.conf: %s; the camera stays off until it is fixed", msg);
        free(text);
    } else if (errno != ENOENT) {
        snprintf(c->conf_error, sizeof c->conf_error, "camera.conf: %s; the camera stays off", strerror(errno));
    }
    if (c->conf_error[0]) {
        fprintf(stderr, "tt7d: camera: %s\n", c->conf_error);
        snprintf(c->last_error, sizeof c->last_error, "%s", c->conf_error);
        camera_config_defaults(&c->file_cfg); /* off, whatever the flag says */
        c->flag_enabled = -1;
    }
    effective(c);
    c->running = c->cfg;
    struct mqtt_extension x = {.ctx = c, .discovery = ext_discovery, .connected = ext_connected,
                               .command = ext_command};
    mqtt_app_set_extension(c->mqtt, &x);
    if (c->cfg.enabled && !camera_available(c))
        fprintf(stderr, "tt7d: camera: enabled, but %s does not exist\n", c->fake_source ? c->fake_source : c->device);
    apply_settings(c, NULL);
}

/* ---- HTTP ---- */

int camera_check_head(struct camera *c, const struct http_request *req, struct response *resp) {
    int snap = !strcmp(req->path, CAMERA_SNAPSHOT_PATH), conf = !strcmp(req->path, CAMERA_CONFIG_PATH);
    if (!snap && !conf) return CAMERA_NOT_MINE;
    const char *allow = snap ? "GET" : "GET, PUT";
    if (strcmp(req->method, "GET") != 0 && !(conf && !strcmp(req->method, "PUT"))) {
        resp_error(resp, 405, "method_not_allowed", "this method is not supported on this path");
        snprintf(resp->extra_headers, sizeof resp->extra_headers, "Allow: %s\r\n", allow);
        return -1;
    }
    /* A camera in someone's home: every camera route needs the token. */
    if (resp_require_bearer(c->token, req, resp) != 0) return -1;
    if (!conf || strcmp(req->method, "PUT") != 0) return 0;
    const char *ct = http_header(req, "Content-Type");
    size_t n = ct ? strcspn(ct, "; \t") : 0;
    if (!ct || n != 16 || strncasecmp(ct, "application/json", 16) != 0) {
        resp_error_begin(resp, 415, "unsupported_media_type", "send the settings as Content-Type: application/json");
        sb_puts(&resp->body, ",\"supported\":[\"application/json\"]");
        resp_error_end(resp);
        return -1;
    }
    const char *cl = http_header(req, "Content-Length");
    if (cl && strtoul(cl, NULL, 10) > MAX_CONFIG_BODY) {
        resp_error(resp, 413, "payload_too_large", "the settings body is limited to 4096 bytes");
        return -1;
    }
    return 0;
}

static int free_waiter(const struct camera *c) {
    for (int i = 0; i < CAMERA_MAX_WAITING; i++)
        if (c->waiters[i].fd < 0) return i;
    return -1;
}

/* Why a snapshot cannot be had right now, or NULL if it can (from the
 * cache, or by waiting for a capture). */
static const char *snapshot_refusal(const struct camera *c, const char **message) {
    if (!c->cfg.enabled) {
        *message = "the camera is off; PUT /api/v1/config/camera {\"enabled\": true} to turn it on";
        return "camera_disabled";
    }
    if (snapshot_fresh(c, mono_ms())) return NULL;
    if (!camera_available(c)) {
        *message = "the camera device does not exist";
        return "camera_unavailable";
    }
    if (c->wstate != CW_RUNNING || c->sock < 0) {
        *message = c->last_error[0] ? c->last_error : "the camera worker is restarting; try again shortly";
        return "camera_busy";
    }
    if (free_waiter(c) < 0) {
        *message = "too many snapshot requests are waiting; try again shortly";
        return "camera_busy";
    }
    return NULL;
}

int camera_take_over(struct camera *c, int fd, const struct http_request *req) {
    if (strcmp(req->path, CAMERA_SNAPSHOT_PATH) != 0) return 0;
    const char *msg;
    if (snapshot_refusal(c, &msg) || snapshot_fresh(c, mono_ms())) return 0; /* camera_handle answers now */
    if (request_snapshot(c) != 0) return 0;
    struct camera_waiter *w = &c->waiters[free_waiter(c)];
    memset(w, 0, sizeof *w);
    w->fd = fd;
    return 1;
}

static void config_json(const struct camera *c, struct sbuf *sb) {
    sb_printf(sb, "{\"config_revision\":%lu,", c->mqtt->config_revision);
    camera_config_json_members(&c->cfg, sb);
    sb_puts(sb, ",\"set_by_flags\":[");
    int first = 1;
    for (int f = 0; f < CAMF_COUNT; f++) {
        if (!(c->flag_fields & (1u << f))) continue;
        sb_printf(sb, "%s\"%s\"", first ? "" : ",", camera_field_json_key(f));
        first = 0;
    }
    sb_puts(sb, "],\"config_error\":");
    sb_json_str(sb, c->conf_error[0] ? c->conf_error : NULL);
    sb_printf(sb, ",\"available\":%s}", camera_available(c) ? "true" : "false");
}

static void config_put(struct camera *c, const uint8_t *body, size_t len, struct response *resp) {
    struct camera_config next = c->file_cfg;
    char field[32], err[200];
    int rc = camera_config_apply_json(&next, (const char *)body, len, c->flag_fields, field, err, sizeof err);
    if (rc != 0) {
        resp_error_begin(resp, rc == -2 ? 409 : 400, rc == -2 ? "set_by_flag" : "invalid_config", err);
        sb_puts(&resp->body, ",\"field\":");
        sb_json_str(&resp->body, field[0] ? field : NULL);
        resp_error_end(resp);
        return;
    }
    struct sbuf text;
    sb_init(&text);
    camera_config_format(&next, &text);
    char path[512];
    conf_path(c, path, sizeof path);
    int ok = !text.oom && write_file_atomic(path, text.buf, text.len, 0644) == 0;
    sb_free(&text);
    if (!ok) {
        resp_error(resp, 500, "write_failed", "could not write camera.conf; the running settings are unchanged");
        return;
    }
    mqtt_app_bump_revision(c->mqtt);
    struct camera_config old = c->cfg;
    c->file_cfg = next;
    if (c->conf_error[0]) { /* a good PUT replaces a bad camera.conf */
        c->conf_error[0] = 0;
        c->last_error[0] = 0;
    }
    effective(c);
    apply_settings(c, &old);
    fprintf(stderr, "tt7d: camera: settings changed (revision %lu): camera %s, presence %s\n",
            c->mqtt->config_revision, c->cfg.enabled ? "on" : "off", c->cfg.presence ? "on" : "off");
    config_json(c, &resp->body);
}

void camera_handle(struct camera *c, const struct http_request *req, const uint8_t *body, size_t len,
                   struct response *resp) {
    resp->status = 200;
    if (!strcmp(req->path, CAMERA_CONFIG_PATH)) {
        if (!strcmp(req->method, "PUT")) config_put(c, body, len, resp);
        else config_json(c, &resp->body);
        return;
    }
    const char *msg = NULL, *code = snapshot_refusal(c, &msg);
    if (code) resp_error(resp, 503, code, msg);
    else if (snapshot_fresh(c, mono_ms())) snapshot_reply(c, resp);
    else resp_error(resp, 503, "camera_busy", "the camera could not take the request; try again");
}

/* ---- the poll loop ---- */

#define REAP_POLL_MS 100
#define DEVICE_POLL_MS 5000 /* enabled but no device: look again this often */

static void lower_wait(int64_t *wait_ms, int64_t d) {
    if (d < 0) d = 0;
    if (*wait_ms < 0 || d < *wait_ms) *wait_ms = d;
}

int camera_prepare(struct camera *c, struct pollfd *pfd, int max, int64_t *wait_ms) {
    int64_t now = mono_ms();
    supervise(c, now);
    int n = 0;
    if (max < 1) return 0;
    pfd[n++] = (struct pollfd){.fd = c->sock, .events = POLLIN}; /* fd -1 when there is none: poll() skips it */
    c->npoll_waiters = 0;
    for (int i = 0; i < CAMERA_MAX_WAITING && n < max; i++) {
        if (c->waiters[i].fd < 0 || !c->waiters[i].answered) continue;
        pfd[n++] = (struct pollfd){.fd = c->waiters[i].fd, .events = POLLOUT};
        c->poll_waiter[c->npoll_waiters++] = i;
    }
    if (c->alive_deadline_ms) lower_wait(wait_ms, c->alive_deadline_ms - now);
    /* An exiting worker is reaped by polling: its EOF can come before waitpid() sees it. */
    if (c->wstate == CW_STOPPING || (c->wstate == CW_RUNNING && c->sock < 0)) lower_wait(wait_ms, REAP_POLL_MS);
    if (c->wstate == CW_RESTARTING) lower_wait(wait_ms, c->restart_at_ms - now);
    if (c->wstate == CW_OFF && c->cfg.enabled) lower_wait(wait_ms, DEVICE_POLL_MS);
    struct presence_policy p = policy(c);
    int64_t blank_in = presence_next_ms(&c->pctl, &p, now);
    if (blank_in >= 0) lower_wait(wait_ms, blank_in);
    if (c->cfg.enabled && c->cfg.snapshot_interval_s > 0 && mqtt_app_connected(c->mqtt))
        lower_wait(wait_ms, c->next_periodic_ms - now);
    for (int i = 0; i < CAMERA_MAX_WAITING; i++)
        if (c->waiters[i].fd >= 0 && c->waiters[i].answered) lower_wait(wait_ms, c->waiters[i].deadline_ms - now);
    return n;
}

void camera_service(struct camera *c, const struct pollfd *pfd, int n) {
    int64_t now = mono_ms();
    if (n > 0 && c->sock >= 0 && pfd[0].fd == c->sock && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
        read_worker(c);
    for (int k = 0; k < c->npoll_waiters && k + 1 < n; k++) {
        struct camera_waiter *w = &c->waiters[c->poll_waiter[k]];
        if (w->fd < 0 || pfd[k + 1].fd != w->fd) continue;
        int done = 0;
        if (pfd[k + 1].revents & (POLLOUT | POLLERR | POLLHUP)) {
            while (w->off < w->out.len) {
                ssize_t s = send(w->fd, w->out.buf + w->off, w->out.len - w->off, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (s < 0 && errno == EINTR) continue;
                if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (s <= 0) {
                    done = 1;
                    break;
                }
                w->off += (size_t)s;
            }
            if (w->off >= w->out.len) done = 1;
        }
        if (!done && now >= w->deadline_ms) done = 1;
        if (done) close_waiter(w);
    }
    supervise(c, now);

    struct presence_policy p = policy(c);
    if (presence_next_ms(&c->pctl, &p, now) == 0)
        apply_presence_action(c, presence_update(&c->pctl, &p, c->present, display_blank(c), now));

    if (c->cfg.enabled && c->cfg.snapshot_interval_s > 0 && mqtt_app_connected(c->mqtt) &&
        now >= c->next_periodic_ms) {
        c->next_periodic_ms = now + (int64_t)c->cfg.snapshot_interval_s * 1000;
        mqtt_snapshot(c);
    }
}

/* ---- /info and /state ---- */

void camera_info_member(const struct camera *c, struct sbuf *sb) {
    sb_printf(sb, "\"camera\":{\"available\":%s,\"enabled\":%s,\"presence\":%s,\"device\":",
              camera_available(c) ? "true" : "false", c->cfg.enabled ? "true" : "false",
              c->cfg.enabled && c->cfg.presence ? "true" : "false");
    sb_json_str(sb, c->fake_source ? c->fake_source : c->device);
    sb_printf(sb, ",\"test_source\":%s,\"video4linux_devices\":[", c->fake_source ? "true" : "false");
    struct names v4l;
    list_dir(c->sysfs_root, "class/video4linux", &v4l);
    for (int i = 0; i < v4l.n; i++) {
        if (i) sb_puts(sb, ",");
        sb_json_str(sb, v4l.v[i]);
    }
    sb_printf(sb, "],\"snapshot\":{\"path\":\"" CAMERA_SNAPSHOT_PATH "\",\"format\":\"image/jpeg\",\"width\":%d,"
                  "\"height\":%d,\"quality\":%d},\"config\":\"" CAMERA_CONFIG_PATH "\"}",
              CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_JPEG_QUALITY);
}

void camera_state_member(const struct camera *c, struct sbuf *sb) {
    static const char *const states[] = {"off", "running", "stopping", "restarting"};
    int presence_on = c->cfg.enabled && c->cfg.presence;
    sb_printf(sb, "\"camera\":{\"enabled\":%s,\"presence_enabled\":%s,\"present\":", c->cfg.enabled ? "true" : "false",
              presence_on ? "true" : "false");
    /* null until the detector has scored a frame */
    if (presence_on && c->ticks) sb_puts(sb, c->present ? "true" : "false");
    else sb_puts(sb, "null");
    sb_puts(sb, ",\"presence_changed_at\":");
    if (c->have_present_changed) sb_json_time(sb, &c->present_changed);
    else sb_puts(sb, "null");
    sb_puts(sb, ",\"last_snapshot_at\":");
    if (c->jpeg) sb_json_time(sb, &c->jpeg_wall);
    else sb_puts(sb, "null");
    sb_printf(sb, ",\"worker\":\"%s\",\"worker_pid\":", states[c->wstate]);
    if (c->pid > 0) sb_printf(sb, "%d", (int)c->pid);
    else sb_puts(sb, "null");
    sb_printf(sb, ",\"worker_restarts\":%lu,\"frames_scored\":%lu,\"last_error\":", c->worker_restarts, c->ticks);
    sb_json_str(sb, c->last_error[0] ? c->last_error : NULL);
    sb_puts(sb, "}");
}
