/* ABOUTME: Host unit tests for http.c: request heads (good, malformed, oversized) and body framing.
 * ABOUTME: Feeds raw bytes, exactly as they would arrive from a socket. */
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "test_common.h"

#define MAX_HEAD 8192

/* Parse a copy of `raw` (the parser writes NULs into its buffer). */
static int parse(const char *raw, struct http_request *req, char **keep) {
    size_t n = strlen(raw);
    char *buf = malloc(n + 1);
    memcpy(buf, raw, n + 1);
    int rc = http_parse_head(buf, n, MAX_HEAD, req);
    *keep = buf;
    return rc;
}

static int status_of(const char *raw) {
    struct http_request req;
    char *buf;
    int rc = parse(raw, &req, &buf);
    free(buf);
    return rc;
}

static void test_good_put(void) {
    const char *raw = "PUT /api/v1/frame?x=1 HTTP/1.1\r\n"
                      "Host: tt7\r\n"
                      "Content-Type:  image/png \r\n"
                      "content-length: 5\r\n"
                      "X-Empty:\r\n"
                      "\r\n"
                      "hello";
    struct http_request req;
    char *buf;
    int rc = parse(raw, &req, &buf);
    CHECK(rc == HTTP_OK, "rc %d", rc);
    if (rc == HTTP_OK) {
        CHECK(strcmp(req.method, "PUT") == 0, "method %s", req.method);
        CHECK(strcmp(req.path, "/api/v1/frame") == 0, "path %s", req.path);
        CHECK(strcmp(req.query, "x=1") == 0, "query %s", req.query);
        CHECK(req.minor_version == 1, "version %d", req.minor_version);
        CHECK(req.nheaders == 4, "nheaders %d", req.nheaders);
        CHECK(req.head_len == strlen(raw) - 5, "head_len %zu", req.head_len);
        const char *ct = http_header(&req, "CONTENT-TYPE");
        CHECK(ct && strcmp(ct, "image/png") == 0, "content-type '%s'", ct ? ct : "(nil)");
        const char *empty = http_header(&req, "x-empty");
        CHECK(empty && *empty == 0, "empty header");
        CHECK(http_header(&req, "Authorization") == NULL, "absent header");
        size_t len = 99;
        CHECK(http_body_length(&req, 100, &len) == 0 && len == 5, "body len %zu", len);
        CHECK(http_body_length(&req, 4, &len) == 413, "too large");
    }
    free(buf);
}

static void test_good_get(void) {
    struct http_request req;
    char *buf;
    int rc = parse("GET /api/v1/info HTTP/1.0\r\n\r\n", &req, &buf);
    CHECK(rc == HTTP_OK, "rc %d", rc);
    if (rc == HTTP_OK) {
        CHECK(strcmp(req.path, "/api/v1/info") == 0 && *req.query == 0 && req.minor_version == 0, "get");
        size_t len = 99;
        CHECK(http_body_length(&req, 100, &len) == 0 && len == 0, "no body");
    }
    free(buf);
}

static void test_incomplete(void) {
    CHECK(status_of("") == HTTP_INCOMPLETE, "empty");
    CHECK(status_of("GET / HTTP/1.1\r\nHost: x\r\n") == HTTP_INCOMPLETE, "no blank line");
    CHECK(status_of("GET / HTTP/1.1\r\nHost: x\r\n\r") == HTTP_INCOMPLETE, "half blank line");
}

static void test_malformed(void) {
    CHECK(status_of("GET\r\n\r\n") == 400, "no target");
    CHECK(status_of("GET /\r\n\r\n") == 400, "no version");
    CHECK(status_of("GET  / HTTP/1.1\r\n\r\n") == 400, "double space");
    CHECK(status_of("get / HTTP/1.1\r\n\r\n") == 400, "lowercase method");
    CHECK(status_of("GET x HTTP/1.1\r\n\r\n") == 400, "target not origin-form");
    CHECK(status_of("GET / HTTP/1.1 \r\n\r\n") == 400, "trailing space");
    CHECK(status_of("GET / HTTX/1.1\r\n\r\n") == 400, "bad protocol");
    CHECK(status_of("GET / HTTP/2.0\r\n\r\n") == 505, "http/2");
    CHECK(status_of("GET / HTTP/1.1\r\nNoColon\r\n\r\n") == 400, "header without colon");
    CHECK(status_of("GET / HTTP/1.1\r\nBad Name: x\r\n\r\n") == 400, "space in name");
    CHECK(status_of("GET / HTTP/1.1\r\nName : x\r\n\r\n") == 400, "space before colon");
    CHECK(status_of("GET / HTTP/1.1\r\n: x\r\n\r\n") == 400, "empty name");
    CHECK(status_of("GET / HTTP/1.1\r\nA: x\r\n folded\r\n\r\n") == 400, "obsolete folding");
    CHECK(status_of("GET / HTTP/1.1\r\nA: x\ny\r\n\r\n") == 400, "bare LF");
    char nul[] = "GET / HTTP/1.1\r\nA: x\0y\r\n\r\n";
    struct http_request req;
    CHECK(http_parse_head(nul, sizeof nul - 1, MAX_HEAD, &req) == 400, "NUL inside head");
    char ctl[] = "GET / HTTP/1.1\r\nA: x\x01y\r\n\r\n";
    CHECK(http_parse_head(ctl, sizeof ctl - 1, MAX_HEAD, &req) == 400, "control char in value");
}

static void test_oversized(void) {
    /* A head that never ends: once longer than max_head it is 431, not "keep reading". */
    size_t n = MAX_HEAD + 100;
    char *big = malloc(n + 1);
    memcpy(big, "GET / HTTP/1.1\r\nX: ", 19);
    memset(big + 19, 'a', n - 19);
    big[n] = 0;
    struct http_request req;
    CHECK(http_parse_head(big, n, MAX_HEAD, &req) == 431, "endless head");
    /* Complete, but longer than max_head. */
    memcpy(big + n - 4, "\r\n\r\n", 4);
    CHECK(http_parse_head(big, n, MAX_HEAD, &req) == 431, "long complete head");
    free(big);

    /* More headers than HTTP_MAX_HEADERS. */
    char many[4096] = "GET / HTTP/1.1\r\n";
    for (int i = 0; i <= HTTP_MAX_HEADERS; i++) {
        char line[32];
        snprintf(line, sizeof line, "H%d: v\r\n", i);
        strcat(many, line);
    }
    strcat(many, "\r\n");
    CHECK(status_of(many) == 431, "too many headers");
}

static int body_status(const char *raw, size_t max, size_t *len) {
    struct http_request req;
    char *buf;
    int rc = parse(raw, &req, &buf);
    if (rc == HTTP_OK) rc = http_body_length(&req, max, len);
    else rc = -rc;
    free(buf);
    return rc;
}

static void test_body_framing(void) {
    size_t len;
    CHECK(body_status("PUT / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 100, &len) == 411, "chunked");
    CHECK(body_status("PUT / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n", 100, &len) == 400,
          "both TE and CL");
    CHECK(body_status("PUT / HTTP/1.1\r\n\r\n", 100, &len) == 411, "PUT without length");
    CHECK(body_status("POST / HTTP/1.1\r\n\r\n", 100, &len) == 411, "POST without length");
    CHECK(body_status("PATCH / HTTP/1.1\r\n\r\n", 100, &len) == 411, "PATCH without length");
    CHECK(body_status("PUT / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", 100, &len) == 400, "negative");
    CHECK(body_status("PUT / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n", 100, &len) == 400, "junk");
    CHECK(body_status("PUT / HTTP/1.1\r\nContent-Length: \r\n\r\n", 100, &len) == 400, "empty");
    CHECK(body_status("PUT / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n", 100, &len) == 413,
          "overflow is too large, not wrapped");
    CHECK(body_status("PUT / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n", 100, &len) == 400,
          "conflicting lengths");
    CHECK(body_status("PUT / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\n", 100, &len) == 0 && len == 3,
          "repeated equal lengths");
    CHECK(body_status("PUT / HTTP/1.1\r\nContent-Length: 0\r\n\r\n", 100, &len) == 0 && len == 0, "zero");
}

static void test_reason(void) {
    CHECK(strcmp(http_reason(200), "OK") == 0, "200");
    CHECK(strcmp(http_reason(422), "Unprocessable Content") == 0, "422 %s", http_reason(422));
    CHECK(strcmp(http_reason(431), "Request Header Fields Too Large") == 0, "431");
    CHECK(strcmp(http_reason(599), "Unknown") == 0, "unknown");
}

int main(void) {
    test_good_put();
    test_good_get();
    test_incomplete();
    test_malformed();
    test_oversized();
    test_body_framing();
    test_reason();
    return test_finish("test_http");
}
