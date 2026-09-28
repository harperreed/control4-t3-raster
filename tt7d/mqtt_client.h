/* ABOUTME: A non-blocking MQTT 3.1.1 client connection for tt7d's single poll() loop: connect, keepalive,
 * ABOUTME: reconnect with 1-60 s backoff, and a bounded send queue that drops old publishes rather than block. */
#ifndef TT7D_MQTT_CLIENT_H
#define TT7D_MQTT_CLIENT_H

#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>

/* Queued bytes allowed at once. A publish that does not fit pushes out the
 * oldest queued publishes; nothing ever waits for the socket. Sized for one
 * camera snapshot (a 1280x720 JPEG at quality 80, typically 50-250 KiB)
 * plus the usual telemetry. */
#define MQTT_OUT_CAP (512 * 1024)
/* Incoming packets bigger than this are skipped, not buffered. */
#define MQTT_IN_CAP (16 * 1024)

enum mqtt_state {
    MQTT_OFF,        /* disabled or not configured */
    MQTT_WAITING,    /* backing off until next_attempt_ms */
    MQTT_CONNECTING, /* TCP connect in progress */
    MQTT_HANDSHAKE,  /* CONNECT sent, waiting for CONNACK */
    MQTT_UP,
    MQTT_CLOSING,    /* DISCONNECT queued; closing once the queue drains */
};

struct mqtt_client_params {
    struct sockaddr_in broker;
    char client_id[65];
    int keepalive_s;
    char username[128];
    char password[256];
    int have_password;
    char will_topic[200];
    char will_payload[16];
};

struct mqtt_outmsg;

struct mqtt_client {
    struct mqtt_client_params params;
    enum mqtt_state state;
    int fd;
    int64_t next_attempt_ms, backoff_ms, deadline_ms;
    int64_t last_tx_ms, last_rx_ms, ping_sent_ms;

    struct mqtt_outmsg *head, *tail;
    size_t out_bytes, head_off;

    uint8_t in[MQTT_IN_CAP];
    size_t in_len;
    size_t skip_left; /* bytes of an oversized packet still to discard */

    /* Called once CONNACK accepts the connection: publish and subscribe here. */
    void (*on_connected)(void *ctx);
    /* A PUBLISH from the broker; topic is NUL-terminated. */
    void (*on_message)(void *ctx, const char *topic, const uint8_t *payload, size_t len, int retain);
    void *ctx;

    /* Diagnostics. */
    unsigned long connects;  /* accepted connections since start */
    unsigned long dropped;   /* publishes dropped: not connected, or the queue was full */
    char last_error[256];    /* "" if none since the last successful connect */
    int error_logged;        /* the current run of failures was logged once */

    /* A restart waiting for the clean close to finish. */
    struct mqtt_client_params pending;
    int restart_pending;
    int shut_wr;
    int disconnect_sent;
    char barrier_topic[200]; /* see mqtt_client_close_barrier */
    char barrier_payload[16];
};

/* Set up an idle client (state OFF). */
void mqtt_client_init(struct mqtt_client *c, void *ctx, void (*on_connected)(void *),
                      void (*on_message)(void *, const char *, const uint8_t *, size_t, int));

/* Start (or restart) with new parameters: an open connection is closed
 * cleanly first (DISCONNECT, so the broker drops the will), then a new one
 * starts at once. */
void mqtt_client_start(struct mqtt_client *c, const struct mqtt_client_params *p);

/* Make the next clean close wait (up to 2 s) until the broker sends
 * `payload` on `topic` before it sends DISCONNECT. Subscribe to the topic and
 * publish to it last: its arrival shows the broker has handled everything
 * sent before it. Some brokers (amqtt 0.12) drop queued messages when a
 * DISCONNECT arrives before they are handled. Cleared by the close. */
void mqtt_client_close_barrier(struct mqtt_client *c, const char *topic, const char *payload);

/* Close cleanly and stay OFF. */
void mqtt_client_stop(struct mqtt_client *c);

/* Queue a QoS 0 PUBLISH. Returns 0, or -1 when not connected (the message is
 * dropped and counted). */
int mqtt_client_publish(struct mqtt_client *c, const char *topic, const void *payload, size_t len, int retain);

/* Queue a SUBSCRIBE (QoS 0) for the filters. */
int mqtt_client_subscribe(struct mqtt_client *c, const char *const *filters, int n);

/* For the poll loop: fill *pfd (fd -1 if none) and lower *wait_ms to the
 * next timer. Then, after poll(), hand back the revents. */
void mqtt_client_prepare(struct mqtt_client *c, struct pollfd *pfd, int64_t *wait_ms);
void mqtt_client_service(struct mqtt_client *c, short revents);

/* Monotonic milliseconds, the clock every deadline here uses. */
int64_t mqtt_now_ms(void);

#endif
