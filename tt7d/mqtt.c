/* ABOUTME: tt7d's MQTT integration (SPEC 22-28): availability with a retained will, state and sensor
 * ABOUTME: topics, events, commands, Home Assistant discovery, and the /api/v1/config/mqtt endpoint. */
#include "mqtt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ident.h"
#include "sysinfo.h"

#define STATE_CHECK_MS 1000 /* how often state is compared for changes */
#define MAX_CONFIG_BODY 4096
#define DISCOVERY_PREFIX "homeassistant"
#define MODEL "C4-TT7"
#define MANUFACTURER "Control4 (repurposed)"

/* ---- pure helpers ---------------------------------------------------------- */

int mqtt_topic(char *out, size_t n, const char *prefix, const char *device_id, const char *leaf) {
    int w = snprintf(out, n, "%s/%s/%s", prefix, device_id, leaf);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

int mqtt_discovery_topic(char *out, size_t n, const char *component, const char *device_id, const char *object) {
    int w = snprintf(out, n, DISCOVERY_PREFIX "/%s/%s/%s/config", component, device_id, object);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

enum mqtt_command mqtt_command_of(const char *topic, const char *base) {
    size_t n = strlen(base);
    if (strncmp(topic, base, n) != 0 || strncmp(topic + n, "/cmd/", 5) != 0) return CMD_NONE;
    const char *name = topic + n + 5;
    if (!strcmp(name, "brightness")) return CMD_BRIGHTNESS;
    if (!strcmp(name, "wake")) return CMD_WAKE;
    if (!strcmp(name, "blank")) return CMD_BLANK;
    if (!strcmp(name, "reboot")) return CMD_REBOOT;
    return CMD_NONE;
}

int mqtt_parse_brightness(const uint8_t *payload, size_t len, int backlight_max, long *value, int *percent) {
    char buf[16];
    size_t a = 0, b = len;
    while (a < b && strchr(" \t\r\n", payload[a]) && payload[a]) a++;
    while (b > a && strchr(" \t\r\n", payload[b - 1]) && payload[b - 1]) b--;
    if (b - a == 0 || b - a >= sizeof buf) return -1;
    memcpy(buf, payload + a, b - a);
    buf[b - a] = 0;
    size_t n = b - a;
    int is_percent = buf[n - 1] == '%';
    if (is_percent) buf[--n] = 0;
    if (n == 0 || n > 5 || strspn(buf, "0123456789") != n) return -1;
    long v = strtol(buf, NULL, 10);
    if (is_percent ? v > 100 : backlight_max <= 0 || v > backlight_max) return -1;
    *value = v;
    *percent = is_percent;
    return 0;
}

/* ---- files ----------------------------------------------------------------- */

static void data_path(const struct mqtt_app *m, const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/%s", m->data_dir, name);
}

/* A small file's text, or NULL (errno set). The caller frees it. */
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

/* The broker password: the file's first line. Returns 1 if there is one. */
static int read_password(const char *path, char *out, size_t n) {
    char *text = read_text(path, 300);
    out[0] = 0;
    if (!text) return 0;
    text[strcspn(text, "\r\n")] = 0;
    int ok = text[0] && mqtt_password_valid(text);
    if (ok) snprintf(out, n, "%s", text);
    memset(text, 0, strlen(text));
    free(text);
    return ok;
}

static unsigned long load_revision(const struct mqtt_app *m) {
    char path[512];
    data_path(m, "config-revision", path, sizeof path);
    char *text = read_text(path, 32);
    unsigned long v = text ? strtoul(text, NULL, 10) : 0;
    free(text);
    return v;
}

static int save_revision(const struct mqtt_app *m, unsigned long v) {
    char path[512], line[32];
    data_path(m, "config-revision", path, sizeof path);
    snprintf(line, sizeof line, "%lu\n", v);
    return write_file_atomic(path, line, strlen(line), 0644);
}

/* ---- publishing ------------------------------------------------------------ */

static void pub(struct mqtt_app *m, const char *topic, const char *payload, int retain) {
    if (mqtt_client_publish(&m->client, topic, payload, strlen(payload), retain) == 0) {
        clock_gettime(CLOCK_REALTIME, &m->last_publish);
        m->have_last_publish = 1;
    }
}

static void pub_leaf(struct mqtt_app *m, const char *leaf, const char *payload, int retain) {
    char topic[256];
    if (snprintf(topic, sizeof topic, "%s/%s", m->base, leaf) < (int)sizeof topic) pub(m, topic, payload, retain);
}

static const char *tri(int v) { return v < 0 ? "null" : v ? "true" : "false"; }

static void json_int_or_null(struct sbuf *sb, const char *key, int v) {
    if (v >= 0) sb_printf(sb, ",\"%s\":%d", key, v);
    else sb_printf(sb, ",\"%s\":null", key);
}

/* The retained state document (SPEC 24) into doc, and into sig the part
 * that counts as a change: everything except the clocks. */
static void build_state(struct mqtt_app *m, const struct sysinfo_values *v, struct sbuf *doc, struct sbuf *sig) {
    const struct frame_store *fs = m->frames;
    struct timespec now, boot;
    clock_gettime(CLOCK_REALTIME, &now);
    clock_gettime(CLOCK_BOOTTIME, &boot);

    sb_puts(sig, "{");
    json_int_or_null(sig, "battery_percent", v->has_battery ? v->battery_percent : -1);
    sb_printf(sig, ",\"battery_estimate\":true,\"charging\":%s,\"external_power\":%s", tri(v->charging),
              tri(v->external_power));
    json_int_or_null(sig, "brightness", v->brightness_percent);
    sb_printf(sig, ",\"display_on\":%s,\"wifi_ip\":", tri(v->display_on));
    sb_json_str(sig, v->wifi_ip[0] ? v->wifi_ip : NULL);
    sb_puts(sig, ",\"ethernet_ip\":");
    sb_json_str(sig, v->ethernet_ip[0] ? v->ethernet_ip : NULL);
    const char *id; /* what the screen shows: a frame, or the fallback clock */
    double age;
    fallback_screen_current(m->screen, fs, &id, &age);
    sb_puts(sig, ",\"frame_id\":");
    sb_json_str(sig, id);

    /* The document: the clocks, the same members, then frame age. */
    sb_puts(doc, "{\"time\":");
    sb_json_time(doc, &now);
    sb_printf(doc, ",\"uptime_s\":%lld", (long long)boot.tv_sec);
    if (sig->buf) sb_puts(doc, sig->buf + 1);
    if (age >= 0) sb_printf(doc, ",\"frame_age_s\":%.1f", age);
    else sb_puts(doc, ",\"frame_age_s\":null");
    if (m->screen) { /* the fallback clock: in the document with its start time, in sig without */
        struct timespec mono;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        int64_t now_ms = (int64_t)mono.tv_sec * 1000 + mono.tv_nsec / 1000000;
        sb_puts(doc, ",");
        fallback_json(&m->screen->state, now_ms, &now, 1, doc);
        sb_puts(sig, ",");
        fallback_json(&m->screen->state, now_ms, &now, 0, sig);
    }
    sb_puts(doc, ",\"last_touch\":"); /* not in sig: a touch alone does not publish */
    if (m->last_touch) sb_json_time(doc, m->last_touch);
    else sb_puts(doc, "null");
    sb_puts(doc, "}");
    sb_puts(sig, "}");
}

static void publish_sensors(struct mqtt_app *m, const struct sysinfo_values *v) {
    char num[32];
    struct timespec boot;
    clock_gettime(CLOCK_BOOTTIME, &boot);
    snprintf(num, sizeof num, "%lld", (long long)boot.tv_sec);
    pub_leaf(m, "sensor/uptime_s", num, 1);
    if (v->has_battery && v->battery_percent >= 0) {
        snprintf(num, sizeof num, "%d", v->battery_percent);
        pub_leaf(m, "sensor/battery_percent", num, 1);
    }
    if (v->charging >= 0) pub_leaf(m, "sensor/charging", v->charging ? "true" : "false", 1);
    if (v->brightness_percent >= 0) {
        snprintf(num, sizeof num, "%d", v->brightness_percent);
        pub_leaf(m, "sensor/brightness", num, 1);
    }
    const char *id;
    double age;
    fallback_screen_current(m->screen, m->frames, &id, &age);
    if (age >= 0) {
        snprintf(num, sizeof num, "%.1f", age);
        pub_leaf(m, "sensor/frame_age_s", num, 1);
    }
    if (v->wifi_ip[0]) pub_leaf(m, "sensor/wifi_ip", v->wifi_ip, 1);
    if (v->ethernet_ip[0]) pub_leaf(m, "sensor/ethernet_ip", v->ethernet_ip, 1);
}

/* Publish state (and the sensor topics) now if forced, due, or changed. */
static void maybe_publish_state(struct mqtt_app *m, int force) {
    if (m->client.state != MQTT_UP) return;
    int64_t now = mqtt_now_ms();
    struct sysinfo_values v;
    sysinfo_read(m->sysfs_root, &v);
    if (m->actions.display) m->actions.display(m->actions.ctx, &v.display_on, &v.brightness_percent);
    struct sbuf doc, sig;
    sb_init(&doc);
    sb_init(&sig);
    build_state(m, &v, &doc, &sig);
    int changed = sig.buf && strcmp(sig.buf, m->last_signature) != 0;
    if (!doc.oom && !sig.oom && (force || changed || now >= m->next_state_ms)) {
        pub_leaf(m, "state", doc.buf, 1);
        publish_sensors(m, &v);
        snprintf(m->last_signature, sizeof m->last_signature, "%s", sig.buf);
        m->next_state_ms = now + (int64_t)m->cfg.telemetry_interval * 1000;
    }
    sb_free(&doc);
    sb_free(&sig);
}

/* ---- Home Assistant discovery (M6) -----------------------------------------
 *
 * One retained config per entity at homeassistant/<component>/<device id>/
 * <object>/config ("single component discovery"), with every entity reading
 * the retained state JSON through a value_template. Field names follow
 * https://www.home-assistant.io/integrations/mqtt/ and the sensor,
 * binary_sensor, number and button MQTT pages (read 2026-09-27). */

enum need { ALWAYS, BATTERY, BACKLIGHT_READONLY, BACKLIGHT_SET, WIFI, ETHERNET, WAKE, BLANK, REBOOT };

static const struct entity {
    const char *component, *object, *name;
    enum need need;
    const char *device_class, *unit, *state_class, *category, *value_template;
} entities[] = {
    {"sensor", "battery", "Battery (estimate)", BATTERY, "battery", "%", "measurement", NULL,
     "{{ value_json.battery_percent }}"},
    {"binary_sensor", "charging", "Charging", BATTERY, "battery_charging", NULL, NULL, NULL,
     "{% if value_json.charging is none %}{% elif value_json.charging %}ON{% else %}OFF{% endif %}"},
    {"sensor", "uptime", "Uptime", ALWAYS, "duration", "s", NULL, "diagnostic", "{{ value_json.uptime_s }}"},
    {"sensor", "frame_age", "Frame age", ALWAYS, "duration", "s", NULL, "diagnostic",
     "{{ value_json.frame_age_s }}"},
    {"sensor", "wifi_ip", "Wi-Fi IP", WIFI, NULL, NULL, NULL, "diagnostic", "{{ value_json.wifi_ip }}"},
    {"sensor", "ethernet_ip", "Ethernet IP", ETHERNET, NULL, NULL, NULL, "diagnostic",
     "{{ value_json.ethernet_ip }}"},
    {"sensor", "brightness", "Brightness", BACKLIGHT_READONLY, NULL, "%", "measurement", NULL,
     "{{ value_json.brightness }}"},
    {"number", "brightness", "Brightness", BACKLIGHT_SET, NULL, "%", NULL, NULL, "{{ value_json.brightness }}"},
    {"button", "wake", "Wake display", WAKE, NULL, NULL, NULL, NULL, NULL},
    {"button", "blank", "Blank display", BLANK, NULL, NULL, NULL, NULL, NULL},
    {"button", "reboot", "Reboot", REBOOT, "restart", NULL, NULL, "config", NULL},
};

static int entity_available(const struct mqtt_app *m, const struct entity *e, const struct sysinfo_values *v) {
    switch (e->need) {
    case ALWAYS: return 1;
    case BATTERY: return v->has_battery;
    case BACKLIGHT_READONLY: return v->has_backlight && !m->actions.set_brightness;
    case BACKLIGHT_SET: return v->has_backlight && m->actions.set_brightness != NULL;
    case WIFI: return v->has_wifi;
    case ETHERNET: return v->has_ethernet;
    case WAKE: return m->actions.wake != NULL;
    case BLANK: return m->actions.blank != NULL;
    case REBOOT: return m->actions.reboot != NULL && m->cfg.allow_reboot_cmd;
    }
    return 0;
}

static void kv(struct sbuf *sb, const char *key, const char *value) {
    if (!value) return;
    sb_printf(sb, ",\"%s\":", key);
    sb_json_str(sb, value);
}

void mqtt_app_discovery_common(const struct mqtt_app *m, const char *object, struct sbuf *sb) {
    char topic[256];
    sb_printf(sb, ",\"unique_id\":\"%s_%s\"", m->device_id, object);
    snprintf(topic, sizeof topic, "%s/availability", m->base);
    kv(sb, "availability_topic", topic);
    sb_puts(sb, ",\"device\":{\"identifiers\":[");
    sb_json_str(sb, m->device_id);
    sb_puts(sb, "],\"name\":");
    char name[64];
    snprintf(name, sizeof name, "TT7 %s", m->device_id);
    sb_json_str(sb, name);
    sb_puts(sb, ",\"model\":\"" MODEL "\",\"manufacturer\":\"" MANUFACTURER "\",\"sw_version\":");
    sb_json_str(sb, m->sw_version);
    sb_puts(sb, "},\"origin\":{\"name\":\"tt7d\",\"sw_version\":");
    sb_json_str(sb, m->sw_version);
    sb_puts(sb, "}");
}

static void entity_config(const struct mqtt_app *m, const struct entity *e, struct sbuf *sb) {
    char topic[256];
    sb_puts(sb, "{\"name\":");
    sb_json_str(sb, e->name);
    if (!strcmp(e->component, "button")) {
        snprintf(topic, sizeof topic, "%s/cmd/%s", m->base, e->object);
        kv(sb, "command_topic", topic);
        kv(sb, "payload_press", "PRESS");
    } else {
        snprintf(topic, sizeof topic, "%s/state", m->base);
        kv(sb, "state_topic", topic);
        kv(sb, "value_template", e->value_template);
    }
    if (!strcmp(e->component, "number")) {
        snprintf(topic, sizeof topic, "%s/cmd/brightness", m->base);
        kv(sb, "command_topic", topic);
        kv(sb, "command_template", "{{ value | int }}%");
        sb_puts(sb, ",\"min\":0,\"max\":100,\"step\":1,\"mode\":\"slider\"");
    }
    kv(sb, "device_class", e->device_class);
    kv(sb, "unit_of_measurement", e->unit);
    kv(sb, "state_class", e->state_class);
    kv(sb, "entity_category", e->category);
    mqtt_app_discovery_common(m, e->object, sb);
    sb_puts(sb, "}");
}

/* Publish every entity's config for device_id: the real config when it is
 * available and `announce` is set, else an empty retained payload, which
 * removes the entity from Home Assistant (and clears the broker's copy). */
static void publish_discovery(struct mqtt_app *m, const char *device_id, int announce) {
    struct sysinfo_values v;
    sysinfo_read(m->sysfs_root, &v);
    for (size_t i = 0; i < sizeof entities / sizeof entities[0]; i++) {
        const struct entity *e = &entities[i];
        char topic[256];
        if (mqtt_discovery_topic(topic, sizeof topic, e->component, device_id, e->object) != 0) continue;
        struct sbuf sb;
        sb_init(&sb);
        if (announce && entity_available(m, e, &v)) entity_config(m, e, &sb);
        pub(m, topic, sb.buf && !sb.oom ? sb.buf : "", 1);
        sb_free(&sb);
    }
    if (m->ext.discovery) m->ext.discovery(m->ext.ctx, m, device_id, announce);
}

void mqtt_app_publish_discovery(struct mqtt_app *m, const char *component, const char *device_id, const char *object,
                                const char *config) {
    char topic[256];
    if (mqtt_discovery_topic(topic, sizeof topic, component, device_id, object) == 0) pub(m, topic, config, 1);
}

/* Announce (or remove) this device's entities, and remove those of a
 * device id announced earlier from this data dir (device.json replaced). */
static void sync_discovery(struct mqtt_app *m) {
    char path[512];
    data_path(m, "mqtt-ha-device", path, sizeof path);
    char *prev = read_text(path, 64);
    if (prev) prev[strcspn(prev, "\r\n")] = 0;
    if (prev && prev[0] && strcmp(prev, m->device_id) != 0) publish_discovery(m, prev, 0);
    publish_discovery(m, m->device_id, m->cfg.ha_discovery);
    if (m->cfg.ha_discovery && (!prev || strcmp(prev, m->device_id) != 0)) {
        char line[40];
        snprintf(line, sizeof line, "%s\n", m->device_id);
        write_file_atomic(path, line, strlen(line), 0644);
    } else if (!m->cfg.ha_discovery && prev) {
        unlink(path);
    }
    free(prev);
}

void mqtt_app_resync_discovery(struct mqtt_app *m) {
    if (m->client.state == MQTT_UP) sync_discovery(m);
}

/* ---- connection callbacks -------------------------------------------------- */

static void on_connected(void *ctx) {
    struct mqtt_app *m = ctx;
    char cmd[200];
    snprintf(cmd, sizeof cmd, "%s/cmd/+", m->base);
    const char *filters[] = {cmd, DISCOVERY_PREFIX "/status"};
    mqtt_client_subscribe(&m->client, filters, m->cfg.ha_discovery ? 2 : 1);
    pub_leaf(m, "availability", "online", 1);
    sync_discovery(m);
    m->last_signature[0] = 0;
    maybe_publish_state(m, 1);
    if (m->ext.connected) m->ext.connected(m->ext.ctx, m);
    if (!m->boot_sent) {
        struct timespec now, boot;
        clock_gettime(CLOCK_REALTIME, &now);
        clock_gettime(CLOCK_BOOTTIME, &boot);
        struct sbuf sb;
        sb_init(&sb);
        sb_puts(&sb, "{\"type\":\"boot\",\"firmware_version\":");
        sb_json_str(&sb, m->sw_version);
        sb_printf(&sb, ",\"uptime_s\":%lld,\"timestamp\":", (long long)boot.tv_sec);
        sb_json_time(&sb, &now);
        sb_puts(&sb, "}");
        if (!sb.oom) pub_leaf(m, "event/boot", sb.buf, 0);
        sb_free(&sb);
        m->boot_sent = 1;
    }
}

static void command_error(struct mqtt_app *m, const char *command, const char *code, const char *message) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct sbuf sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"type\":\"error\",\"error\":");
    sb_json_str(&sb, code);
    sb_puts(&sb, ",\"command\":");
    sb_json_str(&sb, command);
    sb_puts(&sb, ",\"message\":");
    sb_json_str(&sb, message);
    sb_puts(&sb, ",\"timestamp\":");
    sb_json_time(&sb, &now);
    sb_puts(&sb, "}");
    if (!sb.oom) pub_leaf(m, "event/error", sb.buf, 0);
    sb_free(&sb);
}

static void on_message(void *ctx, const char *topic, const uint8_t *payload, size_t len, int retain) {
    struct mqtt_app *m = ctx;
    if (!strcmp(topic, DISCOVERY_PREFIX "/status")) {
        /* Home Assistant's birth message: it (re)started and wants configs. */
        if (len == 6 && !memcmp(payload, "online", 6) && m->cfg.ha_discovery) {
            sync_discovery(m);
            maybe_publish_state(m, 1);
        }
        return;
    }
    enum mqtt_command cmd = mqtt_command_of(topic, m->base);
    /* A retained command would replay on every reconnect (a reboot loop for
     * cmd/reboot): commands are only taken live. */
    if (cmd == CMD_NONE) {
        size_t n = strlen(m->base);
        if (!retain && m->ext.command && !strncmp(topic, m->base, n) && !strncmp(topic + n, "/cmd/", 5))
            m->ext.command(m->ext.ctx, m, topic + n + 5, payload, len);
        return;
    }
    if (retain) return;
    const char *name = strrchr(topic, '/') + 1;
    int (*op)(void *) = NULL;
    int rc;
    switch (cmd) {
    case CMD_BRIGHTNESS: {
        long value;
        int pct;
        struct sysinfo_values v;
        sysinfo_read(m->sysfs_root, &v);
        if (mqtt_parse_brightness(payload, len, v.backlight_max, &value, &pct) != 0) {
            command_error(m, name, "invalid_payload", "brightness wants NN% (0-100) or a raw level 0-max_brightness");
            return;
        }
        if (!m->actions.set_brightness) {
            command_error(m, name, "unsupported_command", "this tt7d build cannot set the brightness");
            return;
        }
        rc = m->actions.set_brightness(m->actions.ctx, value, pct);
        break;
    }
    case CMD_REBOOT:
        if (!m->cfg.allow_reboot_cmd) {
            command_error(m, name, "command_disabled", "cmd/reboot is off; set allow_reboot_cmd=true to allow it");
            return;
        }
        op = m->actions.reboot;
        goto simple;
    case CMD_WAKE: op = m->actions.wake; goto simple;
    case CMD_BLANK: op = m->actions.blank; goto simple;
    default: return;
    }
    goto done;
simple:
    if (!op) {
        command_error(m, name, "unsupported_command", "this tt7d build has no such operation yet");
        return;
    }
    rc = op(m->actions.ctx);
done:
    if (rc != 0) command_error(m, name, "command_failed", "the operation failed; see the tt7d log");
    m->next_check_ms = 0; /* report the new state soon */
}

/* ---- configuration --------------------------------------------------------- */

/* The connection parameters for the config in effect. Returns 0, or -1 with
 * the reason MQTT stays off. */
static int client_params(struct mqtt_app *m, struct mqtt_client_params *p, char *why, size_t n) {
    const struct mqtt_config *c = &m->cfg;
    memset(p, 0, sizeof *p);
    if (!c->enabled) {
        snprintf(why, n, "disabled");
        return -1;
    }
    if (!c->host[0]) {
        snprintf(why, n, "no broker configured");
        return -1;
    }
    if (!m->device_id[0]) {
        snprintf(why, n, "no device id (device.json is unusable), so no topics");
        return -1;
    }
    p->broker.sin_family = AF_INET;
    p->broker.sin_port = htons((uint16_t)c->port);
    inet_pton(AF_INET, c->host, &p->broker.sin_addr);
    snprintf(p->client_id, sizeof p->client_id, "%s", c->client_id[0] ? c->client_id : m->device_id);
    p->keepalive_s = c->keepalive;
    snprintf(p->username, sizeof p->username, "%s", c->username);
    p->have_password = read_password(c->password_file, p->password, sizeof p->password);
    snprintf(p->will_topic, sizeof p->will_topic, "%s/availability", m->base);
    snprintf(p->will_payload, sizeof p->will_payload, "offline");
    return 0;
}

/* Apply m->cfg: recompute topics and (re)connect or stop. */
static void apply_config(struct mqtt_app *m) {
    snprintf(m->base, sizeof m->base, "%s/%s", m->cfg.prefix, m->device_id);
    struct mqtt_client_params p;
    char why[160];
    if (client_params(m, &p, why, sizeof why) == 0) {
        mqtt_client_start(&m->client, &p);
    } else {
        mqtt_client_stop(&m->client);
        snprintf(m->client.last_error, sizeof m->client.last_error, "%s", why);
    }
    memset(p.password, 0, sizeof p.password);
    m->next_check_ms = 0;
}

/* Effective config = mqtt.conf + flags. Flags were validated at startup. */
static void effective(struct mqtt_app *m) {
    char err[160];
    m->cfg = m->file_cfg;
    for (int i = 0; i + 1 < 2 * m->nflags; i += 2) mqtt_config_set(&m->cfg, m->flag_kv[i], m->flag_kv[i + 1], err, sizeof err);
}

int mqtt_app_init(struct mqtt_app *m, const char *data_dir, const char *sysfs_root, const char *device_id,
                  const char *sw_version, const struct frame_store *frames, const struct mqtt_actions *actions,
                  const char *const *flag_kv, int nflags, char *err, size_t errlen) {
    memset(m, 0, sizeof *m);
    snprintf(m->data_dir, sizeof m->data_dir, "%s", data_dir);
    m->sysfs_root = sysfs_root;
    snprintf(m->device_id, sizeof m->device_id, "%s", device_id);
    m->sw_version = sw_version;
    m->frames = frames;
    if (actions) m->actions = *actions;
    m->flag_kv = flag_kv;
    m->nflags = nflags;
    mqtt_client_init(&m->client, m, on_connected, on_message);

    /* Flags first: a typo on the command line is fatal, as for every flag. */
    struct mqtt_config probe;
    mqtt_config_defaults(&probe, data_dir);
    for (int i = 0; i < nflags; i++) {
        char msg[160];
        int f = mqtt_config_set(&probe, flag_kv[2 * i], flag_kv[2 * i + 1], msg, sizeof msg);
        if (f < 0) {
            snprintf(err, errlen, "--mqtt-%s: %s", flag_kv[2 * i], msg);
            return -1;
        }
        m->flag_fields |= 1u << f;
    }

    mqtt_config_defaults(&m->file_cfg, data_dir);
    char path[512];
    data_path(m, "mqtt.conf", path, sizeof path);
    char *text = read_text(path, 16384);
    if (text) {
        struct mqtt_config parsed = m->file_cfg;
        char msg[160];
        if (mqtt_config_parse(&parsed, text, msg, sizeof msg) == 0) {
            m->file_cfg = parsed;
        } else {
            snprintf(m->conf_error, sizeof m->conf_error, "mqtt.conf: %s; MQTT stays off until it is fixed", msg);
            fprintf(stderr, "tt7d: mqtt: %s\n", m->conf_error);
        }
        free(text);
    } else if (errno != ENOENT) {
        snprintf(m->conf_error, sizeof m->conf_error, "mqtt.conf: %s", strerror(errno));
        fprintf(stderr, "tt7d: mqtt: %s\n", m->conf_error);
    }
    m->config_revision = load_revision(m);
    effective(m);
    if (m->conf_error[0]) {
        snprintf(m->base, sizeof m->base, "%s/%s", m->cfg.prefix, m->device_id);
        snprintf(m->client.last_error, sizeof m->client.last_error, "%s", m->conf_error);
        return 0;
    }
    apply_config(m);
    if (m->client.state == MQTT_OFF) fprintf(stderr, "tt7d: mqtt: off (%s)\n", m->client.last_error);
    return 0;
}

void mqtt_app_prepare(struct mqtt_app *m, struct pollfd *pfd, int64_t *wait_ms) {
    mqtt_client_prepare(&m->client, pfd, wait_ms);
    if (m->client.state == MQTT_UP) {
        int64_t left = m->next_check_ms - mqtt_now_ms();
        if (left < 0) left = 0;
        if (*wait_ms < 0 || left < *wait_ms) *wait_ms = left;
    }
}

void mqtt_app_service(struct mqtt_app *m, short revents) {
    mqtt_client_service(&m->client, revents);
    int64_t now = mqtt_now_ms();
    if (m->client.state == MQTT_UP && now >= m->next_check_ms) {
        maybe_publish_state(m, 0);
        m->next_check_ms = now + STATE_CHECK_MS;
    }
}

void mqtt_app_event(struct mqtt_app *m, const char *type, const char *json) {
    char leaf[64];
    snprintf(leaf, sizeof leaf, "event/%s", type);
    if (m->client.state == MQTT_UP) pub_leaf(m, leaf, json, 0);
    else m->client.dropped++;
}

void mqtt_app_state_changed(struct mqtt_app *m) { m->next_check_ms = 0; }

void mqtt_app_set_extension(struct mqtt_app *m, const struct mqtt_extension *x) { m->ext = *x; }

int mqtt_app_connected(const struct mqtt_app *m) { return m->client.state == MQTT_UP; }

int mqtt_app_publish(struct mqtt_app *m, const char *leaf, const void *payload, size_t len, int retain) {
    char topic[256];
    if (snprintf(topic, sizeof topic, "%s/%s", m->base, leaf) >= (int)sizeof topic) return -1;
    if (mqtt_client_publish(&m->client, topic, payload, len, retain) != 0) return -1;
    clock_gettime(CLOCK_REALTIME, &m->last_publish);
    m->have_last_publish = 1;
    return 0;
}

unsigned long mqtt_app_bump_revision(struct mqtt_app *m) {
    m->config_revision++;
    if (save_revision(m, m->config_revision) != 0)
        fprintf(stderr, "tt7d: could not save config-revision: %s\n", strerror(errno));
    return m->config_revision;
}

void mqtt_app_frame_accepted(struct mqtt_app *m) {
    const struct frame_store *fs = m->frames;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct sbuf sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"type\":\"frame\",\"frame_id\":");
    sb_json_str(&sb, fs->id[0] ? fs->id : NULL);
    sb_puts(&sb, ",\"sha256\":");
    sb_json_str(&sb, fs->sha256);
    sb_printf(&sb, ",\"deduplicated\":%s,\"timestamp\":", fs->deduplicated ? "true" : "false");
    sb_json_time(&sb, &now);
    sb_puts(&sb, "}");
    if (!sb.oom) mqtt_app_event(m, "frame", sb.buf);
    sb_free(&sb);
    m->next_check_ms = 0; /* frame_id changed: state goes out on the next loop */
}

void mqtt_app_state_member(struct mqtt_app *m, struct sbuf *sb) {
    const struct mqtt_client *c = &m->client;
    sb_printf(sb, "\"mqtt\":{\"enabled\":%s,\"connected\":%s,\"broker\":", m->cfg.enabled ? "true" : "false",
              c->state == MQTT_UP ? "true" : "false");
    if (m->cfg.host[0]) sb_printf(sb, "\"%s:%d\"", m->cfg.host, m->cfg.port);
    else sb_puts(sb, "null");
    sb_puts(sb, ",\"client_id\":");
    sb_json_str(sb, m->cfg.client_id[0] ? m->cfg.client_id : m->device_id[0] ? m->device_id : NULL);
    sb_puts(sb, ",\"topic_base\":");
    sb_json_str(sb, m->base);
    sb_puts(sb, ",\"last_publish\":");
    if (m->have_last_publish) sb_json_time(sb, &m->last_publish);
    else sb_puts(sb, "null");
    sb_puts(sb, ",\"last_error\":");
    sb_json_str(sb, c->last_error[0] ? c->last_error : NULL);
    sb_printf(sb, ",\"reconnects\":%lu,\"dropped\":%lu}", c->connects > 0 ? c->connects - 1 : 0, c->dropped);
}

/* ---- HTTP: /api/v1/config/mqtt --------------------------------------------- */

int mqtt_http_check_head(const char *token, const struct http_request *req, struct response *resp) {
    if (resp_require_bearer(token, req, resp) != 0) return -1;
    if (strcmp(req->method, "PUT") != 0) return 0;
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

static void config_json(const struct mqtt_app *m, struct sbuf *sb) {
    const struct mqtt_config *c = &m->cfg;
    char pw[256];
    int have_pw = read_password(c->password_file, pw, sizeof pw);
    memset(pw, 0, sizeof pw);
    sb_printf(sb, "{\"config_revision\":%lu,\"enabled\":%s,\"host\":", m->config_revision, c->enabled ? "true" : "false");
    sb_json_str(sb, c->host[0] ? c->host : NULL);
    sb_printf(sb, ",\"port\":%d,\"username\":", c->port);
    sb_json_str(sb, c->username[0] ? c->username : NULL);
    sb_printf(sb, ",\"password_set\":%s,\"password_file\":", have_pw ? "true" : "false");
    sb_json_str(sb, c->password_file);
    sb_puts(sb, ",\"prefix\":");
    sb_json_str(sb, c->prefix);
    sb_puts(sb, ",\"client_id\":");
    sb_json_str(sb, c->client_id[0] ? c->client_id : NULL);
    sb_puts(sb, ",\"effective_client_id\":");
    sb_json_str(sb, c->client_id[0] ? c->client_id : m->device_id[0] ? m->device_id : NULL);
    sb_printf(sb, ",\"keepalive\":%d,\"telemetry_interval\":%d,\"ha_discovery\":%s,\"allow_reboot_cmd\":%s", c->keepalive,
              c->telemetry_interval, c->ha_discovery ? "true" : "false", c->allow_reboot_cmd ? "true" : "false");
    sb_puts(sb, ",\"set_by_flags\":[");
    int first = 1;
    for (int f = 0; f < MQF_COUNT; f++) {
        if (!(m->flag_fields & (1u << f))) continue;
        sb_printf(sb, "%s\"%s\"", first ? "" : ",", mqtt_field_name(f));
        first = 0;
    }
    sb_puts(sb, "],\"config_error\":");
    sb_json_str(sb, m->conf_error[0] ? m->conf_error : NULL);
    sb_puts(sb, "}");
}

/* Before leaving the current connection: tell the broker we are going, and
 * remove our Home Assistant entities if they will not come back there. */
static void farewell(struct mqtt_app *m, const struct mqtt_config *old) {
    if (m->client.state != MQTT_UP) return;
    int same_broker = !strcmp(old->host, m->cfg.host) && old->port == m->cfg.port && m->cfg.enabled;
    char avail[200];
    snprintf(avail, sizeof avail, "%s/availability", m->base); /* m->base is still the old prefix */
    const char *filters[] = {avail};
    mqtt_client_subscribe(&m->client, filters, 1);
    if (old->ha_discovery && !(same_broker && m->cfg.ha_discovery)) publish_discovery(m, m->device_id, 0);
    pub(m, avail, "offline", 1);
    mqtt_client_close_barrier(&m->client, avail, "offline");
}

void mqtt_http_handle(struct mqtt_app *m, const struct http_request *req, const uint8_t *body, size_t len,
                      struct response *resp) {
    if (!strcmp(req->method, "GET")) {
        config_json(m, &resp->body);
        return;
    }
    struct mqtt_config next = m->file_cfg;
    char pw[256], field[32], err[200];
    int pw_change;
    int rc = mqtt_config_apply_json(&next, (const char *)body, len, m->flag_fields, pw, sizeof pw, &pw_change, field,
                                    err, sizeof err);
    if (rc != 0) {
        resp_error_begin(resp, rc == -2 ? 409 : 400, rc == -2 ? "set_by_flag" : "invalid_config", err);
        sb_puts(&resp->body, ",\"field\":");
        sb_json_str(&resp->body, field[0] ? field : NULL);
        resp_error_end(resp);
        return;
    }

    /* Persist: the password file first, then mqtt.conf, then the revision. */
    char path[512];
    if (pw_change) {
        int written = 1;
        if (pw[0]) {
            char line[260];
            snprintf(line, sizeof line, "%s\n", pw);
            written = write_file_atomic(m->cfg.password_file, line, strlen(line), 0600) == 0;
            memset(line, 0, sizeof line);
        } else if (unlink(m->cfg.password_file) != 0 && errno != ENOENT) {
            written = 0;
        }
        memset(pw, 0, sizeof pw);
        if (!written) {
            resp_error(resp, 500, "write_failed", "could not write the password file; nothing was changed");
            return;
        }
    }
    struct sbuf text;
    sb_init(&text);
    mqtt_config_format(&next, &text);
    data_path(m, "mqtt.conf", path, sizeof path);
    int ok = !text.oom && write_file_atomic(path, text.buf, text.len, 0644) == 0;
    sb_free(&text);
    if (!ok) {
        resp_error(resp, 500, "write_failed", "could not write mqtt.conf; the running settings are unchanged");
        return;
    }
    mqtt_app_bump_revision(m);

    struct mqtt_config old = m->cfg;
    m->file_cfg = next;
    m->conf_error[0] = 0;
    effective(m);
    farewell(m, &old);
    apply_config(m);
    fprintf(stderr, "tt7d: mqtt: settings changed (revision %lu)\n", m->config_revision);
    config_json(m, &resp->body);
}
