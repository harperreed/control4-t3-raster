/* ABOUTME: RFC 6455 frames and handshake, plus the hub that fans event messages out to WebSocket clients.
 * ABOUTME: Non-blocking throughout: each client has a fixed-size send queue and is dropped when it overflows. */
#include "ws.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sha1.h"

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"; /* RFC 6455 section 1.3 */

int ws_key_valid(const char *key) {
    if (!key || strlen(key) != 24 || strcmp(key + 22, "==") != 0) return 0;
    for (int i = 0; i < 22; i++)
        if (!strchr(B64, key[i]) || !key[i]) return 0;
    return 1;
}

void ws_accept_key(const char *key, char out[WS_ACCEPT_LEN + 1]) {
    uint8_t buf[128], d[21];
    size_t klen = strlen(key);
    if (klen > sizeof buf - (sizeof GUID - 1)) klen = sizeof buf - (sizeof GUID - 1);
    memcpy(buf, key, klen);
    memcpy(buf + klen, GUID, sizeof GUID - 1);
    sha1(buf, klen + sizeof GUID - 1, d);
    d[20] = 0;
    /* 20 bytes = six 3-byte groups and a 2-byte tail with one '='. */
    char *o = out;
    for (int i = 0; i < 21; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)(i + 1 < 21 ? d[i + 1] : 0) << 8 | (i + 2 < 21 ? d[i + 2] : 0);
        *o++ = B64[(v >> 18) & 63];
        *o++ = B64[(v >> 12) & 63];
        *o++ = B64[(v >> 6) & 63];
        *o++ = i + 2 < 20 ? B64[v & 63] : '=';
    }
    out[WS_ACCEPT_LEN] = 0;
}

size_t ws_encode_header(uint8_t out[WS_MAX_HEADER], int fin, int opcode, uint64_t len, const uint8_t *mask) {
    size_t n = 0;
    out[n++] = (uint8_t)((fin ? 0x80 : 0) | (opcode & 0x0f));
    uint8_t m = mask ? 0x80 : 0;
    if (len < 126) {
        out[n++] = m | (uint8_t)len;
    } else if (len < 65536) {
        out[n++] = m | 126;
        out[n++] = (uint8_t)(len >> 8);
        out[n++] = (uint8_t)len;
    } else {
        out[n++] = m | 127;
        for (int i = 7; i >= 0; i--) out[n++] = (uint8_t)(len >> (8 * i));
    }
    if (mask) {
        memcpy(out + n, mask, 4);
        n += 4;
    }
    return n;
}

void ws_mask(uint8_t *p, size_t n, const uint8_t mask[4]) {
    for (size_t i = 0; i < n; i++) p[i] ^= mask[i & 3];
}

long ws_parse_frame(const uint8_t *buf, size_t len, struct ws_frame *f) {
    if (len < 2) return 0;
    if (buf[0] & 0x70) return -1; /* RSV1-3: no extension was negotiated */
    f->fin = buf[0] >> 7;
    f->opcode = buf[0] & 0x0f;
    f->masked = buf[1] >> 7;
    int op = f->opcode;
    if (op != WS_OP_CONT && op != WS_OP_TEXT && op != WS_OP_BINARY && op != WS_OP_CLOSE && op != WS_OP_PING &&
        op != WS_OP_PONG)
        return -1;
    uint64_t plen = buf[1] & 0x7f;
    size_t h = 2;
    if (plen == 126) {
        if (len < 4) return 0;
        plen = (uint64_t)buf[2] << 8 | buf[3];
        h = 4;
    } else if (plen == 127) {
        if (len < 10) return 0;
        if (buf[2] & 0x80) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = plen << 8 | buf[2 + i];
        h = 10;
    }
    if (op >= WS_OP_CLOSE && (plen > 125 || !f->fin)) return -1;
    if (f->masked) {
        if (len < h + 4) return 0;
        memcpy(f->mask, buf + h, 4);
        h += 4;
    }
    if (plen > (uint64_t)(len - h)) return 0;
    f->len = plen;
    f->payload = buf + h;
    return (long)(h + plen);
}

size_t ws_encode_close(uint8_t *out, size_t cap, int code, const char *reason) {
    size_t rlen = reason ? strlen(reason) : 0;
    if (rlen > 123) rlen = 123;
    if (cap < 4 + rlen) return 0;
    size_t n = ws_encode_header(out, 1, WS_OP_CLOSE, 2 + rlen, NULL);
    out[n++] = (uint8_t)(code >> 8);
    out[n++] = (uint8_t)code;
    memcpy(out + n, reason, rlen);
    return n + rlen;
}

int ws_close_code(const struct ws_frame *f) {
    if (f->len < 2) return 1005;
    return f->payload[0] << 8 | f->payload[1];
}

int ws_header_has_token(const char *value, const char *token) {
    size_t tlen = strlen(token);
    for (const char *p = value ? value : ""; *p;) {
        p += strspn(p, ", \t");
        size_t len = strcspn(p, ",");
        size_t trimmed = len;
        while (trimmed && (p[trimmed - 1] == ' ' || p[trimmed - 1] == '\t')) trimmed--;
        if (trimmed == tlen && strncasecmp(p, token, tlen) == 0) return 1;
        p += len;
    }
    return 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ws_query_param(const char *query, const char *name, char *out, size_t n) {
    size_t nlen = strlen(name);
    for (const char *p = query ? query : ""; *p;) {
        size_t len = strcspn(p, "&");
        if (len > nlen && strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            size_t o = 0;
            for (size_t i = nlen + 1; i < len; i++) {
                int c = (unsigned char)p[i];
                if (c == '%') {
                    int hi = i + 2 < len ? hexval(p[i + 1]) : -1, lo = i + 2 < len ? hexval(p[i + 2]) : -1;
                    if (hi < 0 || lo < 0) return -1;
                    c = hi << 4 | lo;
                    i += 2;
                }
                if (o + 1 >= n) return -1;
                out[o++] = (char)c;
            }
            out[o] = 0;
            return 0;
        }
        p += len + (p[len] == '&');
    }
    return -1;
}

/* ---- the hub ---------------------------------------------------------------- */

void ws_hub_init(struct ws_hub *hub, size_t queue_cap) {
    memset(hub, 0, sizeof *hub);
    hub->queue_cap = queue_cap;
    hub->log = stderr;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) hub->c[i].fd = -1;
}

static void client_close(struct ws_client *c) {
    if (c->fd >= 0) close(c->fd);
    free(c->out);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

void ws_hub_free(struct ws_hub *hub) {
    for (int i = 0; i < WS_MAX_CLIENTS; i++) client_close(&hub->c[i]);
}

int ws_hub_count(const struct ws_hub *hub) {
    int n = 0;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) n += hub->c[i].fd >= 0;
    return n;
}

/* Append to the send queue. Returns -1 if it does not fit. */
static int enqueue(const struct ws_hub *hub, struct ws_client *c, const void *a, size_t alen, const void *b,
                   size_t blen) {
    if (c->out_off) { /* move the unsent bytes to the front */
        memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off = 0;
    }
    if (alen + blen > hub->queue_cap - c->out_len) return -1;
    memcpy(c->out + c->out_len, a, alen);
    if (blen) memcpy(c->out + c->out_len + alen, b, blen);
    c->out_len += alen + blen;
    return 0;
}

static int enqueue_frame(const struct ws_hub *hub, struct ws_client *c, int opcode, const void *payload, size_t len) {
    uint8_t h[WS_MAX_HEADER];
    size_t hl = ws_encode_header(h, 1, opcode, len, NULL);
    return enqueue(hub, c, h, hl, payload, len);
}

/* Send what the socket takes now. Returns -1 if the connection failed. */
static int flush(struct ws_client *c) {
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        c->out_off += (size_t)n;
    }
    c->out_off = c->out_len = 0;
    return 0;
}

/* Drop a client at once: one non-blocking try at a close frame, then close. */
static void drop(struct ws_hub *hub, struct ws_client *c, int code, const char *reason) {
    if (hub->log) fprintf(hub->log, "tt7d: events: dropped client %s: %s\n", c->peer, reason);
    uint8_t fr[WS_MAX_HEADER + 125];
    size_t n = ws_encode_close(fr, sizeof fr, code, reason);
    if (send(c->fd, fr, n, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) { /* it was going away anyway */
    }
    client_close(c);
}

/* Queue our close frame; the socket closes once it is sent (or after WS_CLOSE_WAIT_MS). */
static void start_close(struct ws_hub *hub, struct ws_client *c, int code, const char *reason, int64_t now) {
    if (code != 1000 && hub->log) fprintf(hub->log, "tt7d: events: closing client %s: %d %s\n", c->peer, code, reason);
    uint8_t fr[WS_MAX_HEADER + 125];
    size_t n = ws_encode_close(fr, sizeof fr, code, reason);
    c->closing = 1;
    c->close_deadline_ms = now + WS_CLOSE_WAIT_MS;
    if (enqueue(hub, c, fr, n, NULL, 0) != 0 || flush(c) != 0 || c->out_len == 0) client_close(c);
}

int ws_hub_add(struct ws_hub *hub, int fd, const char *key, const char *hello, int64_t now_ms) {
    struct ws_client *c = NULL;
    for (int i = 0; i < WS_MAX_CLIENTS && !c; i++)
        if (hub->c[i].fd < 0) c = &hub->c[i];
    if (!c) return -1;
    memset(c, 0, sizeof *c);
    if (!(c->out = malloc(hub->queue_cap))) return -1;
    c->fd = fd;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    c->last_rx_ms = now_ms;
    c->next_ping_ms = now_ms + WS_PING_INTERVAL_MS;
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    if (getpeername(fd, (struct sockaddr *)&sa, &sl) != 0 || sa.sin_family != AF_INET ||
        !inet_ntop(AF_INET, &sa.sin_addr, c->peer, sizeof c->peer))
        snprintf(c->peer, sizeof c->peer, "(local)");

    char accept[WS_ACCEPT_LEN + 1], head[160];
    ws_accept_key(key, accept);
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n",
                      accept);
    enqueue(hub, c, head, (size_t)hl, NULL, 0);
    if (hello) enqueue_frame(hub, c, WS_OP_TEXT, hello, strlen(hello));
    hub->connected++;
    if (flush(c) != 0) client_close(c); /* the socket is ours now either way */
    return 0;
}

void ws_hub_broadcast(struct ws_hub *hub, const char *text, size_t len, int64_t now_ms) {
    (void)now_ms;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        struct ws_client *c = &hub->c[i];
        if (c->fd < 0 || c->closing) continue;
        if (enqueue_frame(hub, c, WS_OP_TEXT, text, len) != 0) {
            hub->dropped_slow++;
            drop(hub, c, 1008, "send queue full: the client reads too slowly");
            continue;
        }
        if (flush(c) != 0) client_close(c);
    }
}

int ws_hub_prepare(struct ws_hub *hub, struct pollfd *pfd, int max, int64_t *wait_ms, int64_t now_ms) {
    int n = 0;
    for (int i = 0; i < WS_MAX_CLIENTS && n < max; i++) {
        struct ws_client *c = &hub->c[i];
        if (c->fd < 0) continue;
        short ev = c->closing ? 0 : POLLIN;
        if (c->out_len > c->out_off) ev |= POLLOUT;
        pfd[n] = (struct pollfd){.fd = c->fd, .events = ev};
        hub->pidx[n++] = i;
        int64_t due = c->closing ? c->close_deadline_ms : c->next_ping_ms;
        int64_t idle = c->last_rx_ms + WS_IDLE_TIMEOUT_MS;
        if (!c->closing && idle < due) due = idle;
        int64_t left = due > now_ms ? due - now_ms : 0;
        if (*wait_ms < 0 || left < *wait_ms) *wait_ms = left;
    }
    return n;
}

/* Handle every complete frame in c->in. */
static void read_frames(struct ws_hub *hub, struct ws_client *c, int64_t now) {
    while (c->fd >= 0 && !c->closing) {
        struct ws_frame f;
        long used = ws_parse_frame(c->in, c->in_len, &f);
        if (used < 0) {
            start_close(hub, c, 1002, "protocol error", now);
            return;
        }
        if (used == 0) {
            if (c->in_len == sizeof c->in) start_close(hub, c, 1009, "message too big", now);
            return;
        }
        if (!f.masked) {
            start_close(hub, c, 1002, "client frames must be masked", now);
            return;
        }
        c->last_rx_ms = now;
        uint8_t *payload = c->in + (f.payload - c->in);
        ws_mask(payload, (size_t)f.len, f.mask);
        if (f.opcode == WS_OP_CONT || ((f.opcode == WS_OP_TEXT || f.opcode == WS_OP_BINARY) && !f.fin)) {
            start_close(hub, c, 1003, "fragmented messages are not supported", now);
            return;
        }
        if (f.opcode == WS_OP_PING) {
            if (enqueue_frame(hub, c, WS_OP_PONG, payload, (size_t)f.len) != 0) {
                hub->dropped_slow++;
                drop(hub, c, 1008, "send queue full: the client reads too slowly");
                return;
            }
        } else if (f.opcode == WS_OP_CLOSE) {
            int code = ws_close_code(&f);
            start_close(hub, c, code == 1005 || code < 1000 || code > 4999 ? 1000 : code, "", now);
            return;
        } /* text, binary and pong frames carry nothing for us */
        memmove(c->in, c->in + used, c->in_len - (size_t)used);
        c->in_len -= (size_t)used;
    }
}

void ws_hub_service(struct ws_hub *hub, const struct pollfd *pfd, int n, int64_t now_ms) {
    for (int k = 0; k < n; k++) {
        struct ws_client *c = &hub->c[hub->pidx[k]];
        if (c->fd != pfd[k].fd || c->fd < 0) continue; /* dropped since prepare */
        short rev = pfd[k].revents;
        if ((rev & POLLIN) && !c->closing) {
            ssize_t got = recv(c->fd, c->in + c->in_len, sizeof c->in - c->in_len, MSG_DONTWAIT);
            if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                client_close(c); /* the client went away */
                continue;
            }
            if (got > 0) {
                c->in_len += (size_t)got;
                read_frames(hub, c, now_ms);
            }
        } else if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
            client_close(c);
            continue;
        }
        if (c->fd >= 0 && flush(c) != 0) client_close(c);
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        struct ws_client *c = &hub->c[i];
        if (c->fd < 0) continue;
        if (c->closing) {
            if (c->out_len == 0 || now_ms >= c->close_deadline_ms) client_close(c);
            continue;
        }
        if (now_ms - c->last_rx_ms >= WS_IDLE_TIMEOUT_MS) {
            drop(hub, c, 1001, "no reply to pings");
            continue;
        }
        if (now_ms >= c->next_ping_ms) {
            c->next_ping_ms = now_ms + WS_PING_INTERVAL_MS;
            if (enqueue_frame(hub, c, WS_OP_PING, NULL, 0) != 0) {
                hub->dropped_slow++;
                drop(hub, c, 1008, "send queue full: the client reads too slowly");
                continue;
            }
            if (flush(c) != 0) client_close(c);
        }
    }
}
