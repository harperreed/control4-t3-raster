/* ABOUTME: Minimal HTTP/1.1 request-head parser and body-framing rules for tt7d.
 * ABOUTME: Pure functions over a caller-owned buffer, so the unit tests feed it raw bytes. */
#ifndef TT7D_HTTP_H
#define TT7D_HTTP_H

#include <stddef.h>

#define HTTP_MAX_HEADERS 32

struct http_header {
    const char *name;
    const char *value; /* leading and trailing spaces/tabs removed */
};

struct http_request {
    const char *method;
    const char *path;  /* the target up to '?' */
    const char *query; /* after '?', or "" */
    int minor_version; /* 0 or 1 */
    struct http_header headers[HTTP_MAX_HEADERS];
    int nheaders;
    size_t head_len; /* bytes up to and including the blank line */
};

/* Results of http_parse_head other than an HTTP error status. */
#define HTTP_INCOMPLETE 0
#define HTTP_OK 1

/* Parse the request head at the start of buf[0..len). Returns HTTP_INCOMPLETE
 * until the blank line arrives, HTTP_OK once parsed, or an error status:
 * 400 malformed, 431 head longer than max_head or too many headers, 505 not
 * HTTP/1.0 or 1.1. On HTTP_OK the head is rewritten in place (NULs) and req
 * points into buf. */
int http_parse_head(char *buf, size_t len, size_t max_head, struct http_request *req);

/* Case-insensitive header lookup; NULL if absent. */
const char *http_header(const struct http_request *req, const char *name);

/* Decide the body length. Content-Length only: returns 0 and sets *len, or an
 * error status: 400 bad or conflicting Content-Length, 411 Transfer-Encoding
 * (chunked is not supported) or a PUT/POST with no Content-Length, 413 longer
 * than max_body. A request with neither header has an empty body. */
int http_body_length(const struct http_request *req, size_t max_body, size_t *len);

/* The standard reason phrase for the statuses tt7d sends. */
const char *http_reason(int status);

#endif
