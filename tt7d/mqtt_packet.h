/* ABOUTME: MQTT 3.1.1 packet encoding and decoding for tt7d: CONNECT, PUBLISH, SUBSCRIBE, PING, DISCONNECT.
 * ABOUTME: Pure functions over byte buffers (no sockets), so the unit tests check exact bytes. */
#ifndef TT7D_MQTT_PACKET_H
#define TT7D_MQTT_PACKET_H

#include <stddef.h>
#include <stdint.h>

#include "json.h"

/* Control packet types (MQTT 3.1.1 section 2.2.1). */
enum {
    MQTT_CONNECT = 1,
    MQTT_CONNACK = 2,
    MQTT_PUBLISH = 3,
    MQTT_PUBACK = 4,
    MQTT_SUBSCRIBE = 8,
    MQTT_SUBACK = 9,
    MQTT_PINGREQ = 12,
    MQTT_PINGRESP = 13,
    MQTT_DISCONNECT = 14,
};

/* The largest Remaining Length the 4-byte encoding can carry (section 2.2.3). */
#define MQTT_MAX_REMAINING 268435455u

/* Encode n into out (1 to 4 bytes). Returns the byte count, or -1 if n is
 * larger than MQTT_MAX_REMAINING. */
int mqtt_encode_remaining(size_t n, uint8_t out[4]);

/* Decode the Remaining Length that starts at buf. Returns 1 and sets *value
 * and *used, 0 if more bytes are needed, -1 if malformed (a 5th byte). */
int mqtt_decode_remaining(const uint8_t *buf, size_t avail, size_t *value, size_t *used);

struct mqtt_connect_opts {
    const char *client_id;
    uint16_t keepalive_s;
    const char *username; /* NULL or "" for none */
    const char *password; /* sent only together with a username (section 3.1.2.9) */
    const char *will_topic; /* NULL for no Last Will */
    const char *will_payload;
    int will_retain;
};

/* Each encoder appends one whole packet to out. Returns 0, or -1 if a field
 * is too long for MQTT (strings over 65535 bytes, packets over the limit). */
int mqtt_encode_connect(struct sbuf *out, const struct mqtt_connect_opts *o);
int mqtt_encode_publish(struct sbuf *out, const char *topic, const void *payload, size_t len, int retain);
int mqtt_encode_subscribe(struct sbuf *out, uint16_t packet_id, const char *const *filters, int n);
int mqtt_encode_puback(struct sbuf *out, uint16_t packet_id);
int mqtt_encode_pingreq(struct sbuf *out);
int mqtt_encode_disconnect(struct sbuf *out);

/* The fixed header of the packet at the start of buf. Returns 1 when the
 * header is complete (the body may not be), 0 if more bytes are needed, -1
 * if malformed. */
struct mqtt_header {
    int type;           /* high nibble of byte 0 */
    int flags;          /* low nibble */
    size_t header_len;  /* 1 + the Remaining Length bytes */
    size_t body_len;    /* the Remaining Length */
};
int mqtt_parse_header(const uint8_t *buf, size_t avail, struct mqtt_header *h);

/* A PUBLISH the broker sent us. topic is NOT NUL-terminated. */
struct mqtt_publish {
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
    int qos;
    int retain;
    uint16_t packet_id; /* 0 for QoS 0 */
};
/* Parse a PUBLISH body (after the fixed header). Returns 0 or -1 if malformed. */
int mqtt_parse_publish(int flags, const uint8_t *body, size_t len, struct mqtt_publish *p);

/* CONNACK body: returns the return code (0 = accepted, 1-5 refused), or -1 if malformed. */
int mqtt_parse_connack(const uint8_t *body, size_t len);

/* A readable reason for a CONNACK return code. */
const char *mqtt_connack_reason(int rc);

#endif
