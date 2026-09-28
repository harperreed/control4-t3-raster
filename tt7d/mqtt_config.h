/* ABOUTME: tt7d's MQTT settings: defaults, mqtt.conf (KEY=VALUE lines), --mqtt-* flags and JSON bodies.
 * ABOUTME: One validator for every source; the password lives in its own file and never in mqtt.conf. */
#ifndef TT7D_MQTT_CONFIG_H
#define TT7D_MQTT_CONFIG_H

#include <stddef.h>

#include "json.h"

/* Fields, as bits for "which ones came from a flag". The key names are the
 * mqtt.conf keys, the JSON members, and (with '_' as '-') the --mqtt-* flags. */
enum mqtt_field {
    MQF_ENABLED,
    MQF_HOST,
    MQF_PORT,
    MQF_USERNAME,
    MQF_PASSWORD_FILE,
    MQF_PREFIX,
    MQF_CLIENT_ID,
    MQF_KEEPALIVE,
    MQF_TELEMETRY_INTERVAL,
    MQF_HA_DISCOVERY,
    MQF_ALLOW_REBOOT_CMD,
    MQF_COUNT
};

struct mqtt_config {
    int enabled;
    char host[64]; /* IPv4 literal; "" = no broker configured */
    int port;
    char username[128];
    char password_file[256];
    char prefix[64];
    char client_id[65]; /* "" = the device id */
    int keepalive;      /* seconds */
    int telemetry_interval; /* seconds */
    int ha_discovery;
    int allow_reboot_cmd;
};

/* The defaults: enabled, no host, port 1883, prefix tt7, keepalive 60 s,
 * telemetry every 10 s, HA discovery on, reboot command off, password file
 * <data_dir>/mqtt-password. */
void mqtt_config_defaults(struct mqtt_config *c, const char *data_dir);

/* The key name of a field, and the field of a key name (-1 if unknown). */
const char *mqtt_field_name(int field);
int mqtt_field_by_name(const char *key);

/* Set one field from its text form, validating it. Returns the field, or -1
 * with a message in err (c unchanged). */
int mqtt_config_set(struct mqtt_config *c, const char *key, const char *value, char *err, size_t errlen);

/* Apply mqtt.conf text: KEY=VALUE lines, '#' comments, blank lines. Returns
 * 0, or -1 with "line N: ..." in err (c keeps the lines before it). */
int mqtt_config_parse(struct mqtt_config *c, const char *text, char *err, size_t errlen);

/* mqtt.conf text for c, every key, with a header comment. */
void mqtt_config_format(const struct mqtt_config *c, struct sbuf *out);

/* Apply a JSON object of fields ({"host": "10.0.0.2", "port": 1883, ...}).
 * Booleans must be JSON booleans, numbers JSON numbers, strings strings.
 * "password" (a string, or null to remove it) goes to *password with
 * *password_change = 1; it is never a config field. "password_file" cannot be
 * set this way. Keys in `locked` (a field bitmask) are refused. Returns 0,
 * or -1 with the offending member in bad_field[32] ("" for a syntax error)
 * and a message in err; c is then unchanged. Returns -2 when the member is
 * locked by a flag. */
int mqtt_config_apply_json(struct mqtt_config *c, const char *body, size_t len, unsigned locked, char *password,
                           size_t password_size, int *password_change, char *bad_field, char *err,
                           size_t errlen);

/* 1 if the password text is acceptable: at most 255 bytes, no NUL, CR or LF. */
int mqtt_password_valid(const char *p);

#endif
