/* ABOUTME: MQTT 3.1.1 packet encoders and decoders (OASIS MQTT 3.1.1, sections 2 and 3).
 * ABOUTME: QoS 0 publishing, clean-session CONNECT with an optional will and credentials. */
#include "mqtt_packet.h"

#include <string.h>

int mqtt_encode_remaining(size_t n, uint8_t out[4]) {
    if (n > MQTT_MAX_REMAINING) return -1;
    int i = 0;
    do {
        uint8_t b = n % 128;
        n /= 128;
        if (n > 0) b |= 0x80;
        out[i++] = b;
    } while (n > 0);
    return i;
}

int mqtt_decode_remaining(const uint8_t *buf, size_t avail, size_t *value, size_t *used) {
    size_t v = 0, mult = 1;
    for (size_t i = 0; i < 4; i++) {
        if (i >= avail) return 0;
        v += (size_t)(buf[i] & 0x7f) * mult;
        if (!(buf[i] & 0x80)) {
            *value = v;
            *used = i + 1;
            return 1;
        }
        mult *= 128;
    }
    return -1;
}

static void put_u16(struct sbuf *sb, unsigned v) {
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    sb_add(sb, b, 2);
}

/* A UTF-8 string field: two length bytes, then the bytes. */
static void put_str(struct sbuf *sb, const char *s, size_t n) {
    put_u16(sb, (unsigned)n);
    sb_add(sb, s, n);
}

/* Append the fixed header for a body of body_len bytes. */
static int put_header(struct sbuf *sb, uint8_t first, size_t body_len) {
    uint8_t len[4];
    int n = mqtt_encode_remaining(body_len, len);
    if (n < 0) return -1;
    sb_add(sb, &first, 1);
    sb_add(sb, len, (size_t)n);
    return 0;
}

static int str_ok(const char *s) { return strlen(s) <= 65535; }

int mqtt_encode_connect(struct sbuf *out, const struct mqtt_connect_opts *o) {
    int user = o->username && o->username[0];
    int pass = user && o->password;
    int will = o->will_topic != NULL;
    const char *will_payload = o->will_payload ? o->will_payload : "";
    if (!str_ok(o->client_id) || (user && !str_ok(o->username)) || (pass && !str_ok(o->password)) ||
        (will && (!str_ok(o->will_topic) || !str_ok(will_payload))))
        return -1;

    size_t body = 10 + 2 + strlen(o->client_id);
    if (will) body += 2 + strlen(o->will_topic) + 2 + strlen(will_payload);
    if (user) body += 2 + strlen(o->username);
    if (pass) body += 2 + strlen(o->password);

    uint8_t flags = 0x02; /* clean session */
    if (will) flags |= 0x04 | (o->will_retain ? 0x20 : 0); /* will QoS 0 */
    if (pass) flags |= 0x40;
    if (user) flags |= 0x80;

    if (put_header(out, MQTT_CONNECT << 4, body) != 0) return -1;
    put_str(out, "MQTT", 4);
    uint8_t level = 4; /* 3.1.1 */
    sb_add(out, &level, 1);
    sb_add(out, &flags, 1);
    put_u16(out, o->keepalive_s);
    put_str(out, o->client_id, strlen(o->client_id));
    if (will) {
        put_str(out, o->will_topic, strlen(o->will_topic));
        put_str(out, will_payload, strlen(will_payload));
    }
    if (user) put_str(out, o->username, strlen(o->username));
    if (pass) put_str(out, o->password, strlen(o->password));
    return 0;
}

int mqtt_encode_publish(struct sbuf *out, const char *topic, const void *payload, size_t len, int retain) {
    size_t tlen = strlen(topic);
    if (tlen > 65535) return -1;
    if (put_header(out, (uint8_t)(MQTT_PUBLISH << 4 | (retain ? 1 : 0)), 2 + tlen + len) != 0) return -1;
    put_str(out, topic, tlen);
    sb_add(out, payload, len);
    return 0;
}

int mqtt_encode_subscribe(struct sbuf *out, uint16_t packet_id, const char *const *filters, int n) {
    size_t body = 2;
    for (int i = 0; i < n; i++) {
        if (!str_ok(filters[i])) return -1;
        body += 2 + strlen(filters[i]) + 1;
    }
    if (put_header(out, MQTT_SUBSCRIBE << 4 | 0x02, body) != 0) return -1; /* flags 0010 are required */
    put_u16(out, packet_id);
    for (int i = 0; i < n; i++) {
        uint8_t qos = 0;
        put_str(out, filters[i], strlen(filters[i]));
        sb_add(out, &qos, 1);
    }
    return 0;
}

int mqtt_encode_puback(struct sbuf *out, uint16_t packet_id) {
    put_header(out, MQTT_PUBACK << 4, 2);
    put_u16(out, packet_id);
    return 0;
}

int mqtt_encode_pingreq(struct sbuf *out) { return put_header(out, MQTT_PINGREQ << 4, 0); }

int mqtt_encode_disconnect(struct sbuf *out) { return put_header(out, MQTT_DISCONNECT << 4, 0); }

int mqtt_parse_header(const uint8_t *buf, size_t avail, struct mqtt_header *h) {
    if (avail < 2) return 0;
    size_t used;
    int rc = mqtt_decode_remaining(buf + 1, avail - 1, &h->body_len, &used);
    if (rc <= 0) return rc;
    h->type = buf[0] >> 4;
    h->flags = buf[0] & 0x0f;
    h->header_len = 1 + used;
    return 1;
}

int mqtt_parse_publish(int flags, const uint8_t *body, size_t len, struct mqtt_publish *p) {
    memset(p, 0, sizeof *p);
    p->qos = (flags >> 1) & 3;
    p->retain = flags & 1;
    if (p->qos == 3 || len < 2) return -1;
    size_t tlen = (size_t)body[0] << 8 | body[1];
    size_t pos = 2 + tlen;
    if (pos > len) return -1;
    p->topic = (const char *)body + 2;
    p->topic_len = tlen;
    if (p->qos > 0) {
        if (pos + 2 > len) return -1;
        p->packet_id = (uint16_t)(body[pos] << 8 | body[pos + 1]);
        pos += 2;
    }
    p->payload = body + pos;
    p->payload_len = len - pos;
    return 0;
}

int mqtt_parse_connack(const uint8_t *body, size_t len) {
    if (len != 2) return -1;
    return body[1];
}

const char *mqtt_connack_reason(int rc) {
    switch (rc) {
    case 0: return "accepted";
    case 1: return "unacceptable protocol version";
    case 2: return "client identifier rejected";
    case 3: return "server unavailable";
    case 4: return "bad user name or password";
    case 5: return "not authorized";
    default: return "unknown CONNACK return code";
    }
}
