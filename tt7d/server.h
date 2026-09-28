/* ABOUTME: Single-threaded poll() HTTP/1.1 server: many connections, one request each, strict timeouts.
 * ABOUTME: Knows nothing about frames; main.c plugs in the routes through server_handlers. */
#ifndef TT7D_SERVER_H
#define TT7D_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include "http.h"
#include "json.h"

struct response {
    int status;
    const char *content_type; /* NULL -> application/json */
    struct sbuf body;
    char extra_headers[256]; /* complete "Name: value\r\n" lines, or "" */
    const char *error;        /* the error code, for the request log */
};

struct server_handlers {
    void *ctx;
    /* Called as soon as the head is parsed, before any body is read. Return 0
     * to read the body and call handle(), or fill `resp` (an error) to answer
     * without the body. */
    int (*check_head)(void *ctx, const struct http_request *req, struct response *resp);
    void (*handle)(void *ctx, const struct http_request *req, const uint8_t *body, size_t len, struct response *resp);
    /* Called with every reply to a request whose head parsed, including the
     * server's own errors (408, 411, 413): one place to count outcomes. */
    void (*on_reply)(void *ctx, const struct http_request *req, const struct response *resp);
};

struct server_config {
    const char *listen;  /* "IPv4:PORT" */
    size_t max_head;     /* bytes */
    size_t max_body;     /* bytes */
    int timeout_ms;      /* to receive a whole request, and again to send the reply */
    int max_connections;
};

/* Fill resp with {"error": code, "message": message}; the caller may append
 * more members before resp_error_end(). */
void resp_error_begin(struct response *resp, int status, const char *code, const char *message);
void resp_error_end(struct response *resp);
void resp_error(struct response *resp, int status, const char *code, const char *message);

/* Check "Authorization: Bearer <token>" with a constant-time compare.
 * Returns 0 if it matches, or -1 with resp filled (401 unauthorized plus
 * WWW-Authenticate). */
int resp_require_bearer(const char *token, const struct http_request *req, struct response *resp);

/* Bind, then serve forever. Returns only if the listen address is unusable
 * (with a message in err). */
int server_run(const struct server_config *cfg, const struct server_handlers *h, char *err, size_t errlen);

#endif
