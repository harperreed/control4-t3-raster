/* ABOUTME: WebSocket (RFC 6455) server side for tt7d's event stream: handshake key, frame encode/decode,
 * ABOUTME: and a hub of up to 8 clients with bounded send queues, driven by the daemon's single poll() loop. */
#ifndef TT7D_WS_H
#define TT7D_WS_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define WS_MAX_CLIENTS 8
#define WS_MAX_HEADER 14           /* 2 + 8 length bytes + 4 mask bytes */
#define WS_ACCEPT_LEN 28           /* base64 of a SHA-1 digest */
#define WS_QUEUE_BYTES 65536       /* per client: events waiting for a slow reader */
#define WS_MAX_CLIENT_PAYLOAD 1024 /* larger client frames get close 1009; we expect only control frames */
#define WS_PING_INTERVAL_MS 30000
#define WS_IDLE_TIMEOUT_MS 75000 /* nothing at all from the client for this long: gone */
#define WS_CLOSE_WAIT_MS 2000    /* after our close frame, how long its bytes may take to leave */

enum { WS_OP_CONT = 0, WS_OP_TEXT = 1, WS_OP_BINARY = 2, WS_OP_CLOSE = 8, WS_OP_PING = 9, WS_OP_PONG = 10 };

struct ws_frame {
    int fin;
    int opcode;
    int masked;
    uint8_t mask[4];
    uint64_t len;
    const uint8_t *payload; /* inside the parsed buffer, still masked */
};

/* 1 if key is a Sec-WebSocket-Key: base64 of 16 bytes (24 characters ending "=="). */
int ws_key_valid(const char *key);

/* Sec-WebSocket-Accept for a client key: base64(SHA-1(key + RFC 6455 GUID)), NUL-terminated. */
void ws_accept_key(const char *key, char out[WS_ACCEPT_LEN + 1]);

/* Write a frame header for a `len`-byte payload; mask NULL = unmasked (the
 * server's frames). Returns its length (2 to 14). The caller appends the
 * payload, masked with ws_mask() when a mask was given. */
size_t ws_encode_header(uint8_t out[WS_MAX_HEADER], int fin, int opcode, uint64_t len, const uint8_t *mask);

/* XOR n bytes with the 4-byte mask (masking and unmasking are the same). */
void ws_mask(uint8_t *p, size_t n, const uint8_t mask[4]);

/* Parse one frame at the start of buf. Returns the bytes it takes (header +
 * payload), 0 if more bytes are needed, or -1 for a protocol error: RSV bits,
 * an unknown opcode, a control frame over 125 bytes or fragmented, or a
 * 64-bit length with its top bit set. */
long ws_parse_frame(const uint8_t *buf, size_t len, struct ws_frame *f);

/* An unmasked close frame with a status code and reason. Returns its length. */
size_t ws_encode_close(uint8_t *out, size_t cap, int code, const char *reason);

/* A close frame's status code, or 1005 ("no status") if it has none. */
int ws_close_code(const struct ws_frame *f);

/* 1 if a comma-separated header value (Connection, Upgrade) contains `token`,
 * compared without case. */
int ws_header_has_token(const char *value, const char *token);

/* The value of `name` in a URL query ("a=1&token=abc"), percent-decoded
 * ('+' stays '+'). Returns 0, or -1 if absent, malformed or too long for out. */
int ws_query_param(const char *query, const char *name, char *out, size_t n);

/* ---- the client hub ---- */

struct ws_client {
    int fd;      /* -1: a free slot */
    int closing; /* our close frame is queued: no more reads or events; the socket closes once it is out */
    int64_t close_deadline_ms, last_rx_ms, next_ping_ms;
    uint8_t *out; /* the send queue, queue_cap bytes */
    size_t out_len, out_off;
    uint8_t in[WS_MAX_HEADER + WS_MAX_CLIENT_PAYLOAD];
    size_t in_len;
    char peer[48];
};

struct ws_hub {
    struct ws_client c[WS_MAX_CLIENTS];
    int pidx[WS_MAX_CLIENTS]; /* which client each descriptor from ws_hub_prepare belongs to */
    size_t queue_cap;
    unsigned long connected;    /* clients accepted since start */
    unsigned long dropped_slow; /* clients dropped because their send queue overflowed */
    FILE *log;                  /* abnormal drops are logged here; NULL = quiet */
};

void ws_hub_init(struct ws_hub *hub, size_t queue_cap);
void ws_hub_free(struct ws_hub *hub);

/* Take over a socket whose upgrade request was already checked: queue the
 * 101 reply (with the Accept for `key`) and, if not NULL, a first text
 * message. Returns 0, or -1 (no free slot, or out of memory) without taking
 * the socket. */
int ws_hub_add(struct ws_hub *hub, int fd, const char *key, const char *hello, int64_t now_ms);

int ws_hub_count(const struct ws_hub *hub);

/* Send a text message to every open client. A client whose queue cannot take
 * it is dropped (close 1008), so a slow reader never holds anything up. */
void ws_hub_broadcast(struct ws_hub *hub, const char *text, size_t len, int64_t now_ms);

/* The poll loop hooks: fill up to max descriptors (returns how many) and
 * lower *wait_ms to the next ping or close deadline; then service them. */
int ws_hub_prepare(struct ws_hub *hub, struct pollfd *pfd, int max, int64_t *wait_ms, int64_t now_ms);
void ws_hub_service(struct ws_hub *hub, const struct pollfd *pfd, int n, int64_t now_ms);

#endif
