/* ABOUTME: poll()-driven HTTP/1.1 server for tt7d: non-blocking sockets, per-connection deadlines,
 * ABOUTME: Content-Length bodies, Expect: 100-continue, and "Connection: close" after every reply. */
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNECTIONS_CAP 64
#define READ_CHUNK 65536

enum conn_state { READING, WRITING };

struct conn {
    int fd;
    enum conn_state state;
    char peer[INET_ADDRSTRLEN];
    int64_t started_ms;
    int64_t deadline_ms;
    char *in; /* the head (plus any body bytes that came with it); never moves once
                 allocated, because req points into it */
    size_t in_len;
    int head_done;
    struct http_request req;
    uint8_t *body; /* its own buffer, body_len bytes */
    size_t body_len, body_got;
    size_t discard_left;         /* body bytes still to drop before replying */
    struct response early;       /* reply decided at head time (discard mode) */
    int have_early;
    struct sbuf out;
    size_t out_off;
    char log_line[160];
    int status; /* of the reply; 0 until one is decided */
};

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void resp_error_begin(struct response *resp, int status, const char *code, const char *message) {
    resp->status = status;
    resp->content_type = NULL;
    resp->error = code;
    resp->body.len = 0;
    if (resp->body.buf) resp->body.buf[0] = 0;
    sb_puts(&resp->body, "{\"error\":");
    sb_json_str(&resp->body, code);
    sb_puts(&resp->body, ",\"message\":");
    sb_json_str(&resp->body, message);
}

void resp_error_end(struct response *resp) { sb_puts(&resp->body, "}"); }

void resp_error(struct response *resp, int status, const char *code, const char *message) {
    resp_error_begin(resp, status, code, message);
    resp_error_end(resp);
}

/* Errors the server itself produces, before any handler runs. */
static void protocol_error(struct response *resp, int status) {
    switch (status) {
    case 400: resp_error(resp, 400, "bad_request", "malformed HTTP request"); break;
    case 408: resp_error(resp, 408, "request_timeout", "the request did not arrive in time"); break;
    case 411:
        resp_error(resp, 411, "length_required", "send the body with Content-Length; chunked transfer is not supported");
        break;
    case 413: resp_error(resp, 413, "payload_too_large", "the body is larger than this device accepts"); break;
    case 431: resp_error(resp, 431, "headers_too_large", "the request head is too large or has too many headers"); break;
    case 505: resp_error(resp, 505, "http_version_not_supported", "only HTTP/1.0 and HTTP/1.1 are supported"); break;
    default: resp_error(resp, 500, "internal_error", "unexpected server error");
    }
}

static void conn_free(struct conn *c) {
    if (c->fd >= 0) close(c->fd);
    free(c->in);
    free(c->body);
    sb_free(&c->out);
    sb_free(&c->early.body);
    free(c);
}

/* Serialise resp into c->out and switch to writing. */
static void conn_reply(struct conn *c, struct response *resp, const struct server_config *cfg,
                       const struct server_handlers *h) {
    if (resp->body.oom) {
        sb_free(&resp->body);
        resp_error(resp, 500, "internal_error", "out of memory");
    }
    sb_printf(&c->out,
              "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
              "Connection: close\r\n%s\r\n",
              resp->status, http_reason(resp->status),
              resp->content_type ? resp->content_type : "application/json; charset=utf-8", resp->body.len,
              resp->extra_headers);
    sb_add(&c->out, resp->body.buf ? resp->body.buf : "", resp->body.len);
    if (c->out.oom) { /* nothing sensible to send */
        c->out.len = 0;
    }
    if (c->head_done && h->on_reply) h->on_reply(h->ctx, &c->req, resp);
    c->status = resp->status;
    snprintf(c->log_line, sizeof c->log_line, "%s %s -> %d%s%s", c->head_done ? c->req.method : "-",
             c->head_done ? c->req.path : "-", resp->status, resp->error ? " " : "", resp->error ? resp->error : "");
    free(c->body); /* the request is done with; a big body need not wait for a slow reader */
    c->body = NULL;
    c->state = WRITING;
    c->out_off = 0;
    c->deadline_ms = now_ms() + cfg->timeout_ms;
}

static int expects_continue(const struct http_request *req) {
    const char *e = http_header(req, "Expect");
    return e && strcasecmp(e, "100-continue") == 0;
}

/* The head just parsed: decide the body length and whether to go on. */
static void on_head(struct conn *c, const struct server_config *cfg, const struct server_handlers *h) {
    struct response resp = {0};
    int st = http_body_length(&c->req, cfg->max_body, &c->body_len);
    if (st) {
        protocol_error(&resp, st);
        conn_reply(c, &resp, cfg, h);
        sb_free(&resp.body);
        return;
    }
    size_t have_body = c->in_len - c->req.head_len;
    if (h->check_head(h->ctx, &c->req, &resp) != 0) {
        /* Rejected on the head alone. Without Expect: 100-continue the client
         * is already sending the body: drain it first so closing the socket
         * cannot reset the connection before the reply is read. */
        if (expects_continue(&c->req) || c->body_len <= have_body) {
            conn_reply(c, &resp, cfg, h);
            sb_free(&resp.body);
        } else {
            c->early = resp;
            c->have_early = 1;
            c->discard_left = c->body_len - have_body;
        }
        return;
    }
    if (c->body_len && !(c->body = malloc(c->body_len))) {
        resp_error(&resp, 500, "internal_error", "out of memory for the request body");
        conn_reply(c, &resp, cfg, h);
        sb_free(&resp.body);
        return;
    }
    c->body_got = have_body < c->body_len ? have_body : c->body_len;
    if (c->body_got) memcpy(c->body, c->in + c->req.head_len, c->body_got);
    if (c->body_len > have_body && expects_continue(&c->req)) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (send(c->fd, cont, sizeof cont - 1, MSG_NOSIGNAL) != (ssize_t)sizeof cont - 1)
            c->deadline_ms = 0; /* cannot even say "continue": time the client out */
    }
}

static void maybe_dispatch(struct conn *c, const struct server_config *cfg, const struct server_handlers *h) {
    if (c->state != READING || !c->head_done) return;
    struct response resp = {0};
    if (c->have_early) {
        if (c->discard_left) return;
        conn_reply(c, &c->early, cfg, h);
        return;
    }
    if (c->body_got < c->body_len) return;
    h->handle(h->ctx, &c->req, c->body, c->body_len, &resp);
    conn_reply(c, &resp, cfg, h);
    sb_free(&resp.body);
}

/* Read what is available. Returns -1 when the connection should close now. */
static int conn_read(struct conn *c, const struct server_config *cfg, const struct server_handlers *h) {
    for (;;) {
        if (c->state != READING) return 0;
        char drop[4096];
        char *dst;
        size_t room;
        if (c->have_early) { /* draining a rejected body */
            dst = drop;
            room = c->discard_left < sizeof drop ? c->discard_left : sizeof drop;
        } else if (!c->head_done) {
            /* One byte past max_head lets the parser see "too long". */
            size_t cap = cfg->max_head + 1;
            if (!c->in && !(c->in = malloc(cap))) return -1;
            dst = c->in + c->in_len;
            room = cap - c->in_len;
        } else {
            dst = (char *)c->body + c->body_got;
            room = c->body_len - c->body_got;
        }
        if (room == 0) return 0; /* pipelined extra bytes are left unread */
        ssize_t n = recv(c->fd, dst, room > READ_CHUNK ? READ_CHUNK : room, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1; /* the client went away mid-request */
        if (c->have_early) {
            c->discard_left -= (size_t)n;
        } else if (c->head_done) {
            c->body_got += (size_t)n;
        } else {
            c->in_len += (size_t)n;
            int st = http_parse_head(c->in, c->in_len, cfg->max_head, &c->req);
            if (st == HTTP_INCOMPLETE) continue;
            if (st != HTTP_OK) {
                struct response resp = {0};
                protocol_error(&resp, st);
                conn_reply(c, &resp, cfg, h);
                sb_free(&resp.body);
                return 0;
            }
            c->head_done = 1;
            on_head(c, cfg, h);
        }
        maybe_dispatch(c, cfg, h);
    }
}

/* Send what the socket takes. Returns -1 once the reply is out (or failed). */
static int conn_write(struct conn *c) {
    while (c->out_off < c->out.len) {
        ssize_t n = send(c->fd, c->out.buf + c->out_off, c->out.len - c->out_off, MSG_NOSIGNAL);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        c->out_off += (size_t)n;
    }
    return -1;
}

/* Only failures are logged: the log lives on flash (/data/tt7/app.log), and
 * frames pushed every few seconds must not turn into a stream of writes.
 * /api/v1/state counts the successes. */
static void log_done(struct conn *c) {
    if (c->log_line[0] && c->status >= 400)
        fprintf(stderr, "tt7d: %s %s (%lld ms)\n", c->peer, c->log_line, (long long)(now_ms() - c->started_ms));
}

static int open_listener(const char *listen_addr, char *err, size_t errlen) {
    char host[64];
    const char *colon = strrchr(listen_addr, ':');
    char *end;
    long port = colon ? strtol(colon + 1, &end, 10) : -1;
    if (!colon || (size_t)(colon - listen_addr) >= sizeof host || *end || port < 0 || port > 65535) {
        snprintf(err, errlen, "--listen wants IPv4:PORT, got '%s'", listen_addr);
        return -1;
    }
    memcpy(host, listen_addr, (size_t)(colon - listen_addr));
    host[colon - listen_addr] = 0;
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        snprintf(err, errlen, "--listen: '%s' is not an IPv4 address", host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
        bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 16) != 0) {
        snprintf(err, errlen, "listen on %s: %s", listen_addr, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

static void accept_one(int lfd, struct conn **conns, int *n, const struct server_config *cfg) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    int fd = accept4(lfd, (struct sockaddr *)&peer, &plen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;
    struct conn *c = calloc(1, sizeof *c);
    if (!c) {
        close(fd);
        return;
    }
    c->fd = fd;
    c->state = READING;
    inet_ntop(AF_INET, &peer.sin_addr, c->peer, sizeof c->peer);
    c->started_ms = now_ms();
    c->deadline_ms = c->started_ms + cfg->timeout_ms;
    conns[(*n)++] = c;
}

int server_run(const struct server_config *cfg, const struct server_handlers *h, char *err, size_t errlen) {
    int lfd = open_listener(cfg->listen, err, errlen);
    if (lfd < 0) return -1;
    fprintf(stderr, "tt7d: listening on %s\n", cfg->listen);

    int max = cfg->max_connections;
    if (max < 1) max = 1;
    if (max > MAX_CONNECTIONS_CAP) max = MAX_CONNECTIONS_CAP;
    struct conn *conns[MAX_CONNECTIONS_CAP];
    struct pollfd pfd[MAX_CONNECTIONS_CAP + 1];
    int n = 0;

    for (;;) {
        int64_t now = now_ms(), wait = -1;
        int np = 0;
        /* When every slot is taken, stop accepting: the kernel backlog holds
         * new clients until a slot frees up (at the latest after a timeout). */
        if (n < max) pfd[np++] = (struct pollfd){.fd = lfd, .events = POLLIN};
        for (int i = 0; i < n; i++) {
            pfd[np++] = (struct pollfd){.fd = conns[i]->fd, .events = conns[i]->state == READING ? POLLIN : POLLOUT};
            int64_t left = conns[i]->deadline_ms - now;
            if (left < 0) left = 0;
            if (wait < 0 || left < wait) wait = left;
        }
        if (poll(pfd, (nfds_t)np, wait > 0x7fffffff ? 0x7fffffff : (int)wait) < 0 && errno != EINTR) {
            fprintf(stderr, "tt7d: poll: %s\n", strerror(errno));
            sleep(1);
            continue;
        }
        int base = n < max ? 1 : 0;
        int nconn = n; /* accept after servicing, so pfd indexes stay valid */
        for (int i = 0; i < nconn; i++) {
            struct conn *c = conns[i];
            short rev = pfd[base + i].revents;
            int done = 0;
            if (c->state == READING && (rev & (POLLIN | POLLHUP | POLLERR))) done = conn_read(c, cfg, h) < 0;
            if (!done && c->state == WRITING && (rev & (POLLOUT | POLLERR | POLLHUP) || c->out_off == 0))
                done = conn_write(c) < 0;
            if (!done && now_ms() >= c->deadline_ms) {
                if (c->state == READING) { /* too slow to send its request: say so once, then close */
                    struct response resp = {0};
                    protocol_error(&resp, 408);
                    conn_reply(c, &resp, cfg, h);
                    sb_free(&resp.body);
                    conn_write(c); /* one non-blocking try; the reply is small */
                    done = 1;
                } else {
                    done = 1; /* too slow to read its reply */
                }
            }
            if (done) {
                log_done(c);
                shutdown(c->fd, SHUT_WR);
                conn_free(c);
                conns[i] = NULL;
            }
        }
        int k = 0;
        for (int i = 0; i < n; i++)
            if (conns[i]) conns[k++] = conns[i];
        n = k;
        if (base && (pfd[0].revents & POLLIN))
            while (n < max) {
                int before = n;
                accept_one(lfd, conns, &n, cfg);
                if (n == before) break;
            }
    }
}
