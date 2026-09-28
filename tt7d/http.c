/* ABOUTME: Minimal HTTP/1.1 request-head parser and Content-Length body framing for tt7d.
 * ABOUTME: Strict on purpose: CRLF line ends only, no obsolete folding, no control bytes in the head. */
#include "http.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

/* RFC 9110 tchar: the characters allowed in a method or header name. */
static int is_tchar(unsigned char c) {
    if (c >= '0' && c <= '9') return 1;
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') return 1;
    return c && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* Visible ASCII, space, tab, or obs-text (>= 0x80): what a field value may hold. */
static int is_value_char(unsigned char c) { return c == '\t' || (c >= 0x20 && c != 0x7f); }

static const char *find_head_end(const char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return buf + i;
    return NULL;
}

/* Split one line off [*p, end): returns it NUL-terminated, or NULL if a bare
 * CR or LF sits inside it. */
static char *take_line(char **p, char *end) {
    char *line = *p;
    char *crlf = line;
    while (crlf < end && !(crlf[0] == '\r' && crlf + 1 < end && crlf[1] == '\n')) {
        if (*crlf == '\r' || *crlf == '\n') return NULL;
        crlf++;
    }
    if (crlf >= end) return NULL;
    *crlf = 0;
    *p = crlf + 2;
    return line;
}

static int parse_request_line(char *line, struct http_request *req) {
    char *sp1 = strchr(line, ' ');
    if (!sp1 || sp1 == line) return 400;
    *sp1 = 0;
    for (char *c = line; *c; c++)
        if (!is_tchar((unsigned char)*c) || (*c >= 'a' && *c <= 'z')) return 400;
    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2 || target[0] != '/') return 400;
    *sp2 = 0;
    for (char *c = target; *c; c++)
        if ((unsigned char)*c <= 0x20 || (unsigned char)*c >= 0x7f) return 400;
    const char *ver = sp2 + 1;
    if (strncmp(ver, "HTTP/", 5) != 0 || strlen(ver) != 8 || ver[6] != '.' || ver[5] < '0' || ver[5] > '9' ||
        ver[7] < '0' || ver[7] > '9')
        return 400;
    if (ver[5] != '1' || (ver[7] != '0' && ver[7] != '1')) return 505;
    req->method = line;
    req->minor_version = ver[7] - '0';
    char *q = strchr(target, '?');
    if (q) {
        *q = 0;
        req->query = q + 1;
    } else {
        req->query = "";
    }
    req->path = target;
    return 0;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

int http_parse_head(char *buf, size_t len, size_t max_head, struct http_request *req) {
    const char *end_mark = find_head_end(buf, len > max_head ? max_head : len);
    if (!end_mark) return len >= max_head ? 431 : HTTP_INCOMPLETE;
    memset(req, 0, sizeof *req);
    req->head_len = (size_t)(end_mark - buf) + 4;
    for (size_t i = 0; i < req->head_len; i++)
        if (buf[i] == 0) return 400;

    char *p = buf, *end = buf + req->head_len - 2; /* keep the last line's CRLF */
    char *line = take_line(&p, end);
    if (!line) return 400;
    int rc = parse_request_line(line, req);
    if (rc) return rc;

    while (p < end) {
        line = take_line(&p, end);
        if (!line) return 400;
        if (req->nheaders == HTTP_MAX_HEADERS) return 431;
        char *colon = strchr(line, ':');
        if (!colon || colon == line) return 400;
        for (char *c = line; c < colon; c++)
            if (!is_tchar((unsigned char)*c)) return 400; /* also rejects folding and "Name :" */
        *colon = 0;
        for (char *c = colon + 1; *c; c++)
            if (!is_value_char((unsigned char)*c)) return 400;
        req->headers[req->nheaders].name = line;
        req->headers[req->nheaders].value = trim(colon + 1);
        req->nheaders++;
    }
    return HTTP_OK;
}

const char *http_header(const struct http_request *req, const char *name) {
    for (int i = 0; i < req->nheaders; i++)
        if (strcasecmp(req->headers[i].name, name) == 0) return req->headers[i].value;
    return NULL;
}

/* Parse a Content-Length value. Returns 0, 400 if not all digits, 413 if
 * above max (including values too big for size_t). */
static int parse_length(const char *s, size_t max, size_t *out) {
    if (!*s) return 400;
    size_t v = 0;
    int big = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 400;
        if (v > (SIZE_MAX - 9) / 10) big = 1;
        else v = v * 10 + (size_t)(*s - '0');
    }
    if (big || v > max) return 413;
    *out = v;
    return 0;
}

int http_body_length(const struct http_request *req, size_t max_body, size_t *len) {
    const char *cl = NULL;
    int has_te = 0;
    for (int i = 0; i < req->nheaders; i++) {
        const struct http_header *h = &req->headers[i];
        if (strcasecmp(h->name, "Transfer-Encoding") == 0) has_te = 1;
        if (strcasecmp(h->name, "Content-Length") == 0) {
            if (cl && strcmp(cl, h->value) != 0) return 400;
            cl = h->value;
        }
    }
    if (has_te) return cl ? 400 : 411;
    if (!cl) {
        if (strcmp(req->method, "PUT") == 0 || strcmp(req->method, "POST") == 0) return 411;
        *len = 0;
        return 0;
    }
    return parse_length(cl, max_body, len);
}

const char *http_reason(int status) {
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Content";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
    }
}
