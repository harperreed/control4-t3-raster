/* ABOUTME: tt7d's MQTT connection: non-blocking TCP connect, CONNECT/CONNACK, PINGREQ keepalive, and
 * ABOUTME: exponential reconnect backoff, all driven from the daemon's poll() loop without ever blocking. */
#include "mqtt_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "mqtt_packet.h"

#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 60000
#define CONNECT_TIMEOUT_MS 10000 /* TCP connect, and again for CONNACK */
#define CLOSE_TIMEOUT_MS 2000    /* to flush the queue before a clean close */

struct mqtt_outmsg {
    struct mqtt_outmsg *next;
    int droppable; /* a PUBLISH; control packets are never dropped */
    size_t len;
    uint8_t data[];
};

int64_t mqtt_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void mqtt_client_init(struct mqtt_client *c, void *ctx, void (*on_connected)(void *),
                      void (*on_message)(void *, const char *, const uint8_t *, size_t, int)) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->state = MQTT_OFF;
    c->backoff_ms = BACKOFF_MIN_MS;
    c->ctx = ctx;
    c->on_connected = on_connected;
    c->on_message = on_message;
}

/* Control packets are wiped before they are freed: CONNECT holds the password. */
static void free_msg(struct mqtt_outmsg *m) {
    if (!m->droppable) memset(m->data, 0, m->len);
    free(m);
}

static void queue_clear(struct mqtt_client *c) {
    while (c->head) {
        struct mqtt_outmsg *m = c->head;
        c->head = m->next;
        free_msg(m);
    }
    c->tail = NULL;
    c->out_bytes = 0;
    c->head_off = 0;
}

static void close_socket(struct mqtt_client *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    queue_clear(c);
    c->in_len = 0;
    c->skip_left = 0;
}

static void finish_close(struct mqtt_client *c);

/* The connection failed or dropped: close it and wait out the backoff. Only
 * the first failure of a run is logged, so a long broker outage does not
 * write the flash log once a minute forever. */
static void fail(struct mqtt_client *c, const char *fmt, ...) {
    if (c->state == MQTT_CLOSING) { /* going away anyway */
        finish_close(c);
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->last_error, sizeof c->last_error, fmt, ap);
    va_end(ap);
    if (!c->error_logged) {
        fprintf(stderr, "tt7d: mqtt: %s; retrying with backoff up to %d s\n", c->last_error, BACKOFF_MAX_MS / 1000);
        c->error_logged = 1;
    }
    close_socket(c);
    c->state = MQTT_WAITING;
    c->next_attempt_ms = mqtt_now_ms() + c->backoff_ms;
    c->backoff_ms = c->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : c->backoff_ms * 2;
}

/* Send what the socket takes right now. */
static void flush(struct mqtt_client *c) {
    while (c->fd >= 0 && c->head && c->state != MQTT_CONNECTING) {
        struct mqtt_outmsg *m = c->head;
        ssize_t n = send(c->fd, m->data + c->head_off, m->len - c->head_off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            fail(c, "send to the broker failed: %s", n < 0 ? strerror(errno) : "connection closed");
            return;
        }
        c->last_tx_ms = mqtt_now_ms();
        c->head_off += (size_t)n;
        if (c->head_off < m->len) return;
        c->head = m->next;
        if (!c->head) c->tail = NULL;
        c->out_bytes -= m->len;
        c->head_off = 0;
        free_msg(m);
    }
}

/* Drop the oldest droppable message that has not started going out. */
static int drop_oldest(struct mqtt_client *c) {
    struct mqtt_outmsg **pp = &c->head;
    if (c->head && c->head_off > 0) pp = &c->head->next; /* half-sent: must finish */
    for (; *pp; pp = &(*pp)->next) {
        struct mqtt_outmsg *m = *pp;
        if (!m->droppable) continue;
        *pp = m->next;
        if (c->tail == m) {
            c->tail = NULL;
            for (struct mqtt_outmsg *t = c->head; t; t = t->next) c->tail = t;
        }
        c->out_bytes -= m->len;
        c->dropped++;
        free(m);
        return 0;
    }
    return -1;
}

/* Queue one encoded packet and try to send it. */
static int enqueue(struct mqtt_client *c, const struct sbuf *pkt, int droppable) {
    if (pkt->oom || pkt->len > MQTT_OUT_CAP) {
        if (droppable) c->dropped++;
        return -1;
    }
    while (c->out_bytes + pkt->len > MQTT_OUT_CAP) {
        if (drop_oldest(c) == 0) continue;
        if (droppable) { /* only control packets are queued: drop the newcomer */
            c->dropped++;
            return -1;
        }
        fail(c, "the broker stopped reading (send queue full)");
        return -1;
    }
    struct mqtt_outmsg *m = malloc(sizeof *m + pkt->len);
    if (!m) {
        if (droppable) c->dropped++;
        return -1;
    }
    m->next = NULL;
    m->droppable = droppable;
    m->len = pkt->len;
    memcpy(m->data, pkt->buf, pkt->len);
    if (c->tail) c->tail->next = m;
    else c->head = m;
    c->tail = m;
    c->out_bytes += pkt->len;
    flush(c);
    return 0;
}

static void send_control(struct mqtt_client *c, int (*encode)(struct sbuf *)) {
    struct sbuf pkt;
    sb_init(&pkt);
    encode(&pkt);
    enqueue(c, &pkt, 0);
    sb_free(&pkt);
}

static void send_connect(struct mqtt_client *c) {
    const struct mqtt_client_params *p = &c->params;
    struct mqtt_connect_opts o = {.client_id = p->client_id,
                                  .keepalive_s = (uint16_t)p->keepalive_s,
                                  .username = p->username,
                                  .password = p->have_password ? p->password : NULL,
                                  .will_topic = p->will_topic,
                                  .will_payload = p->will_payload,
                                  .will_retain = 1};
    struct sbuf pkt;
    sb_init(&pkt);
    mqtt_encode_connect(&pkt, &o);
    c->state = MQTT_HANDSHAKE;
    c->deadline_ms = mqtt_now_ms() + CONNECT_TIMEOUT_MS;
    enqueue(c, &pkt, 0);
    /* The packet holds the password: wipe it before freeing. */
    if (pkt.buf) memset(pkt.buf, 0, pkt.cap);
    sb_free(&pkt);
}

static void start_connect(struct mqtt_client *c) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fail(c, "socket: %s", strerror(errno));
        return;
    }
    c->fd = fd;
    c->in_len = 0;
    c->skip_left = 0;
    int rc = connect(fd, (struct sockaddr *)&c->params.broker, sizeof c->params.broker);
    if (rc == 0) {
        send_connect(c);
    } else if (errno == EINPROGRESS) {
        c->state = MQTT_CONNECTING;
        c->deadline_ms = mqtt_now_ms() + CONNECT_TIMEOUT_MS;
    } else {
        fail(c, "connect to %s:%u: %s", inet_ntoa(c->params.broker.sin_addr), ntohs(c->params.broker.sin_port),
             strerror(errno));
    }
}

/* After a clean close: start again with the pending parameters, or stay off. */
static void finish_close(struct mqtt_client *c) {
    close_socket(c);
    c->barrier_topic[0] = 0;
    if (!c->restart_pending) {
        c->state = MQTT_OFF;
        return;
    }
    c->restart_pending = 0;
    c->params = c->pending;
    memset(&c->pending, 0, sizeof c->pending);
    c->state = MQTT_WAITING;
    c->next_attempt_ms = mqtt_now_ms();
    c->backoff_ms = BACKOFF_MIN_MS;
    c->last_error[0] = 0;
    c->error_logged = 0;
}

/* Close cleanly when connected: DISCONNECT (so the broker drops the will),
 * flush, half-close, and wait briefly for the broker to hang up. Anything
 * else closes at once. */
static void send_disconnect(struct mqtt_client *c) {
    c->disconnect_sent = 1;
    c->shut_wr = 0;
    c->deadline_ms = mqtt_now_ms() + CLOSE_TIMEOUT_MS;
    send_control(c, mqtt_encode_disconnect);
}

static void begin_close(struct mqtt_client *c) {
    if (c->state != MQTT_UP) {
        if (c->state != MQTT_CLOSING) finish_close(c);
        return;
    }
    c->state = MQTT_CLOSING;
    c->disconnect_sent = 0;
    c->deadline_ms = mqtt_now_ms() + CLOSE_TIMEOUT_MS;
    if (!c->barrier_topic[0]) send_disconnect(c);
}

void mqtt_client_close_barrier(struct mqtt_client *c, const char *topic, const char *payload) {
    snprintf(c->barrier_topic, sizeof c->barrier_topic, "%s", topic);
    snprintf(c->barrier_payload, sizeof c->barrier_payload, "%s", payload);
}

void mqtt_client_start(struct mqtt_client *c, const struct mqtt_client_params *p) {
    c->pending = *p;
    c->restart_pending = 1;
    begin_close(c);
}

void mqtt_client_stop(struct mqtt_client *c) {
    c->restart_pending = 0;
    memset(&c->pending, 0, sizeof c->pending);
    memset(c->params.password, 0, sizeof c->params.password);
    begin_close(c);
}

int mqtt_client_publish(struct mqtt_client *c, const char *topic, const void *payload, size_t len, int retain) {
    if (c->state != MQTT_UP) {
        c->dropped++;
        return -1;
    }
    struct sbuf pkt;
    sb_init(&pkt);
    int rc = mqtt_encode_publish(&pkt, topic, payload, len, retain) == 0 ? enqueue(c, &pkt, 1) : -1;
    sb_free(&pkt);
    return rc;
}

int mqtt_client_subscribe(struct mqtt_client *c, const char *const *filters, int n) {
    if (c->state != MQTT_UP) return -1;
    struct sbuf pkt;
    sb_init(&pkt);
    int rc = mqtt_encode_subscribe(&pkt, 1, filters, n) == 0 ? enqueue(c, &pkt, 0) : -1;
    sb_free(&pkt);
    return rc;
}

/* One whole packet from the broker. */
static void handle_packet(struct mqtt_client *c, const struct mqtt_header *h, const uint8_t *body) {
    if (c->state == MQTT_HANDSHAKE) {
        int rc = h->type == MQTT_CONNACK ? mqtt_parse_connack(body, h->body_len) : -1;
        if (rc != 0) {
            fail(c, rc < 0 ? "broker sent something other than CONNACK" : "broker refused the connection: %s (%d)",
                 mqtt_connack_reason(rc), rc);
            return;
        }
        c->state = MQTT_UP;
        c->backoff_ms = BACKOFF_MIN_MS;
        c->connects++;
        c->last_error[0] = 0;
        c->ping_sent_ms = 0;
        fprintf(stderr, "tt7d: mqtt: connected to %s:%u as %s\n", inet_ntoa(c->params.broker.sin_addr),
                ntohs(c->params.broker.sin_port), c->params.client_id);
        c->error_logged = 0;
        if (c->on_connected) c->on_connected(c->ctx);
        return;
    }
    if (c->state == MQTT_CLOSING) { /* only the barrier message matters now */
        struct mqtt_publish p;
        size_t n = strlen(c->barrier_topic);
        if (h->type == MQTT_PUBLISH && !c->disconnect_sent && n &&
            mqtt_parse_publish(h->flags, body, h->body_len, &p) == 0 && p.topic_len == n &&
            !memcmp(p.topic, c->barrier_topic, n) && p.payload_len == strlen(c->barrier_payload) &&
            !memcmp(p.payload, c->barrier_payload, p.payload_len))
            send_disconnect(c);
        return;
    }
    if (c->state != MQTT_UP) return;
    if (h->type == MQTT_PINGRESP) {
        c->ping_sent_ms = 0;
    } else if (h->type == MQTT_PUBLISH) {
        struct mqtt_publish p;
        if (mqtt_parse_publish(h->flags, body, h->body_len, &p) != 0) {
            fail(c, "malformed PUBLISH from the broker");
            return;
        }
        if (p.qos == 1) {
            struct sbuf pkt;
            sb_init(&pkt);
            mqtt_encode_puback(&pkt, p.packet_id);
            enqueue(c, &pkt, 0);
            sb_free(&pkt);
        }
        char topic[256];
        if (p.topic_len < sizeof topic && c->on_message) {
            memcpy(topic, p.topic, p.topic_len);
            topic[p.topic_len] = 0;
            c->on_message(c->ctx, topic, p.payload, p.payload_len, p.retain);
        }
    }
    /* SUBACK, PUBACK and anything else need no action. */
}

static void read_socket(struct mqtt_client *c) {
    for (;;) {
        if (c->fd < 0) return;
        uint8_t drop[2048];
        uint8_t *dst = c->skip_left ? drop : c->in + c->in_len;
        size_t room = c->skip_left ? (c->skip_left < sizeof drop ? c->skip_left : sizeof drop)
                                   : sizeof c->in - c->in_len;
        ssize_t n = recv(c->fd, dst, room, MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 && c->state == MQTT_CLOSING) { /* the broker hung up, as asked */
            finish_close(c);
            return;
        }
        if (n <= 0) {
            fail(c, "the broker closed the connection%s%s", n < 0 ? ": " : "", n < 0 ? strerror(errno) : "");
            return;
        }
        c->last_rx_ms = mqtt_now_ms();
        if (c->skip_left) {
            c->skip_left -= (size_t)n;
            continue;
        }
        c->in_len += (size_t)n;
        /* Take every whole packet in the buffer. */
        size_t off = 0;
        while (c->fd >= 0) {
            struct mqtt_header h;
            int rc = mqtt_parse_header(c->in + off, c->in_len - off, &h);
            if (rc < 0) {
                fail(c, "malformed packet from the broker");
                return;
            }
            if (rc == 0) break;
            size_t total = h.header_len + h.body_len;
            if (total > sizeof c->in) { /* too big to hold: skip it */
                c->skip_left = total - (c->in_len - off);
                off = c->in_len;
                break;
            }
            if (c->in_len - off < total) break;
            handle_packet(c, &h, c->in + off + h.header_len);
            off += total;
        }
        if (c->fd < 0) return;
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
}

static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }

/* When the keepalive needs attention next. */
static int64_t keepalive_due(const struct mqtt_client *c) {
    int64_t ka = (int64_t)c->params.keepalive_s * 1000;
    if (c->ping_sent_ms) return c->ping_sent_ms + ka / 2; /* PINGRESP deadline */
    return min64(c->last_tx_ms, c->last_rx_ms) + ka;
}

void mqtt_client_prepare(struct mqtt_client *c, struct pollfd *pfd, int64_t *wait_ms) {
    int64_t now = mqtt_now_ms(), due = -1;
    *pfd = (struct pollfd){.fd = -1};
    switch (c->state) {
    case MQTT_OFF: break;
    case MQTT_WAITING: due = c->next_attempt_ms; break;
    case MQTT_CONNECTING:
        *pfd = (struct pollfd){.fd = c->fd, .events = POLLOUT};
        due = c->deadline_ms;
        break;
    case MQTT_HANDSHAKE:
    case MQTT_CLOSING:
        *pfd = (struct pollfd){.fd = c->fd, .events = (short)(POLLIN | (c->head ? POLLOUT : 0))};
        due = c->deadline_ms;
        break;
    case MQTT_UP:
        *pfd = (struct pollfd){.fd = c->fd, .events = (short)(POLLIN | (c->head ? POLLOUT : 0))};
        due = keepalive_due(c);
        break;
    }
    if (due < 0) return;
    int64_t left = due > now ? due - now : 0;
    if (*wait_ms < 0 || left < *wait_ms) *wait_ms = left;
}

void mqtt_client_service(struct mqtt_client *c, short revents) {
    int64_t now = mqtt_now_ms();
    switch (c->state) {
    case MQTT_OFF: return;
    case MQTT_WAITING:
        if (now >= c->next_attempt_ms) start_connect(c);
        return;
    case MQTT_CONNECTING:
        if (revents & (POLLOUT | POLLERR | POLLHUP)) {
            int err = 0;
            socklen_t len = sizeof err;
            if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) err = errno;
            if (err) {
                fail(c, "connect to %s:%u: %s", inet_ntoa(c->params.broker.sin_addr),
                     ntohs(c->params.broker.sin_port), strerror(err));
                return;
            }
            c->last_rx_ms = now;
            send_connect(c);
        } else if (now >= c->deadline_ms) {
            fail(c, "connect to %s:%u timed out", inet_ntoa(c->params.broker.sin_addr),
                 ntohs(c->params.broker.sin_port));
        }
        return;
    default: break;
    }
    if (revents & (POLLIN | POLLHUP | POLLERR)) read_socket(c);
    if (c->fd >= 0 && (revents & POLLOUT)) flush(c);
    if (c->fd < 0 && c->state == MQTT_CLOSING) finish_close(c);
    if (c->fd < 0) return;
    now = mqtt_now_ms();
    if (c->state == MQTT_CLOSING && !c->disconnect_sent) {
        if (now >= c->deadline_ms) send_disconnect(c); /* no barrier message: go anyway */
    } else if (c->state == MQTT_CLOSING) {
        if (!c->head && !c->shut_wr) {
            shutdown(c->fd, SHUT_WR);
            c->shut_wr = 1;
        }
        if (now >= c->deadline_ms) finish_close(c);
    } else if (c->state == MQTT_HANDSHAKE && now >= c->deadline_ms) {
        fail(c, "no CONNACK from the broker within %d s", CONNECT_TIMEOUT_MS / 1000);
    } else if (c->state == MQTT_UP && now >= keepalive_due(c)) {
        if (c->ping_sent_ms) {
            fail(c, "no PINGRESP from the broker within %d s", c->params.keepalive_s / 2);
        } else {
            c->ping_sent_ms = now;
            send_control(c, mqtt_encode_pingreq);
        }
    }
}
