/* ABOUTME: Host unit tests for sha1.c and ws.c: RFC 3174 vectors, the RFC 6455 accept-key example,
 * ABOUTME: frame encode/decode at every length boundary, masking, and a real socketpair through the hub. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sha1.h"
#include "test_common.h"
#include "ws.h"

static void expect_sha1(const void *msg, size_t len, const char *want) {
    uint8_t d[20];
    char hex[41];
    sha1((const uint8_t *)msg, len, d);
    for (int i = 0; i < 20; i++) snprintf(hex + 2 * i, 3, "%02X", d[i]);
    CHECK(strcmp(hex, want) == 0, "sha1 of %zu bytes: %s, want %s", len, hex, want);
}

static void test_sha1(void) {
    /* RFC 3174 section 7.3, TEST1 to TEST4. */
    expect_sha1("abc", 3, "A9993E364706816ABA3E25717850C26C9CD0D89D");
    const char *t2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    expect_sha1(t2, strlen(t2), "84983E441C3BD26EBAAE4AA1F95129E5E54670F1");
    char *million = malloc(1000000);
    memset(million, 'a', 1000000);
    expect_sha1(million, 1000000, "34AA973CD4C4DAA4F61EEB2BDBAD27316534016F");
    free(million);
    char t4[640];
    for (int i = 0; i < 80; i++) memcpy(t4 + 8 * i, "01234567", 8);
    expect_sha1(t4, sizeof t4, "DEA356A2CDDD90C7A7ECEDC5EBB563934F460452");
    expect_sha1("", 0, "DA39A3EE5E6B4B0D3255BFEF95601890AFD80709");
}

static void test_accept_key(void) {
    char out[WS_ACCEPT_LEN + 1];
    /* RFC 6455 section 1.3. */
    ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out);
    CHECK(strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0, "accept %s", out);
    CHECK(ws_key_valid("dGhlIHNhbXBsZSBub25jZQ=="), "the RFC key is valid");
    CHECK(!ws_key_valid("dGhlIHNhbXBsZSBub25jZQ="), "23 characters");
    CHECK(!ws_key_valid("dGhlIHNhbXBsZSBub25jZ*=="), "not base64");
    CHECK(!ws_key_valid(NULL), "missing");
}

static void test_headers_and_query(void) {
    CHECK(ws_header_has_token("Upgrade", "upgrade"), "single token");
    CHECK(ws_header_has_token("keep-alive, Upgrade", "upgrade"), "Firefox's Connection header");
    CHECK(ws_header_has_token(" websocket ", "websocket"), "spaces trimmed");
    CHECK(!ws_header_has_token("upgraded", "upgrade"), "whole tokens only");
    CHECK(!ws_header_has_token(NULL, "upgrade"), "missing header");

    char v[16];
    CHECK(ws_query_param("token=abc", "token", v, sizeof v) == 0 && !strcmp(v, "abc"), "plain");
    CHECK(ws_query_param("x=1&token=a%2Fb%3d&y=2", "token", v, sizeof v) == 0 && !strcmp(v, "a/b="), "decoded: %s", v);
    CHECK(ws_query_param("tokens=1&token=", "token", v, sizeof v) == 0 && !strcmp(v, ""), "empty value, not 'tokens'");
    CHECK(ws_query_param("xtoken=1", "token", v, sizeof v) != 0, "only whole names");
    CHECK(ws_query_param("token=%4", "token", v, sizeof v) != 0, "truncated escape");
    CHECK(ws_query_param("token=%zz", "token", v, sizeof v) != 0, "bad escape");
    CHECK(ws_query_param("token=0123456789abcdef", "token", v, sizeof v) != 0, "too long for out");
    CHECK(ws_query_param("", "token", v, sizeof v) != 0, "empty query");
}

/* Encode a frame with the header helper, then parse it back. */
static void roundtrip(size_t len, const uint8_t *mask) {
    size_t total = WS_MAX_HEADER + len;
    uint8_t *buf = malloc(total);
    uint8_t *payload = malloc(len ? len : 1);
    for (size_t i = 0; i < len; i++) payload[i] = (uint8_t)(i * 7 + 3);
    size_t h = ws_encode_header(buf, 1, WS_OP_TEXT, len, mask);
    size_t want_h = (len < 126 ? 2 : len < 65536 ? 4 : 10) + (mask ? 4 : 0);
    CHECK(h == want_h, "header for %zu bytes is %zu, want %zu", len, h, want_h);
    memcpy(buf + h, payload, len);
    if (mask) ws_mask(buf + h, len, mask);
    if (mask && len) CHECK(memcmp(buf + h, payload, len) != 0, "masking changed the payload");

    struct ws_frame f;
    CHECK(ws_parse_frame(buf, h + len - (len ? 1 : 0), &f) == (len ? 0 : (long)h), "one byte short is incomplete (%zu)",
          len);
    long used = ws_parse_frame(buf, h + len, &f);
    CHECK(used == (long)(h + len), "parsed %ld of %zu", used, h + len);
    CHECK(f.fin == 1 && f.opcode == WS_OP_TEXT && f.len == len && f.masked == (mask != NULL), "fields for %zu", len);
    CHECK(f.payload == buf + h, "payload pointer");
    if (mask) ws_mask(buf + h, len, f.mask);
    CHECK(memcmp(buf + h, payload, len) == 0, "payload of %zu survives", len);
    for (size_t k = 1; k < h; k++) CHECK(ws_parse_frame(buf, k, &f) == 0, "a %zu-byte header prefix is incomplete", k);
    free(buf);
    free(payload);
}

static void test_frames(void) {
    static const uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    size_t lens[] = {0, 1, 125, 126, 127, 65535, 65536, 70000};
    for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        roundtrip(lens[i], NULL);
        roundtrip(lens[i], mask);
    }

    /* RFC 6455 section 5.7: a masked "Hello" from a client. */
    const uint8_t hello[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
    struct ws_frame f;
    CHECK(ws_parse_frame(hello, sizeof hello, &f) == (long)sizeof hello, "hello");
    uint8_t text[5];
    memcpy(text, f.payload, 5);
    ws_mask(text, 5, f.mask);
    CHECK(memcmp(text, "Hello", 5) == 0, "unmasked hello");

    /* And the unmasked server form of it, byte for byte. */
    uint8_t out[16];
    size_t n = ws_encode_header(out, 1, WS_OP_TEXT, 5, NULL);
    CHECK(n == 2 && out[0] == 0x81 && out[1] == 0x05, "server text header %02x %02x", out[0], out[1]);
    n = ws_encode_header(out, 1, WS_OP_TEXT, 256, NULL);
    CHECK(n == 4 && out[1] == 126 && out[2] == 0x01 && out[3] == 0x00, "16-bit length");
    n = ws_encode_header(out, 1, WS_OP_BINARY, 65536, NULL);
    CHECK(n == 10 && out[0] == 0x82 && out[1] == 127 && out[7] == 0x01 && out[8] == 0 && out[9] == 0, "64-bit length");

    /* A 64-bit length with the top bit set is a protocol error. */
    const uint8_t huge[] = {0x82, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
    CHECK(ws_parse_frame(huge, sizeof huge, &f) < 0, "top bit of a 64-bit length");
    /* RSV bits without an extension are a protocol error. */
    const uint8_t rsv[] = {0xc1, 0x80, 1, 2, 3, 4};
    CHECK(ws_parse_frame(rsv, sizeof rsv, &f) < 0, "RSV1 set");
}

static void test_close_and_ping(void) {
    uint8_t buf[64];
    size_t n = ws_encode_close(buf, sizeof buf, 1008, "too slow");
    struct ws_frame f;
    CHECK(ws_parse_frame(buf, n, &f) == (long)n, "close parses");
    CHECK(f.opcode == WS_OP_CLOSE && f.fin && f.len == 2 + 8, "close frame %d len %llu", f.opcode,
          (unsigned long long)f.len);
    CHECK(f.payload[0] == 0x03 && f.payload[1] == 0xf0 && !memcmp(f.payload + 2, "too slow", 8), "code 1008 + reason");
    CHECK(ws_close_code(&f) == 1008, "close code");
    const uint8_t bare_close[] = {0x88, 0x00};
    CHECK(ws_parse_frame(bare_close, 2, &f) == 2 && ws_close_code(&f) == 1005, "no code = 1005");

    /* A ping from the client, masked, with a payload: the pong echoes it. */
    static const uint8_t mask[4] = {1, 2, 3, 4};
    uint8_t ping[32];
    size_t h = ws_encode_header(ping, 1, WS_OP_PING, 4, mask);
    memcpy(ping + h, "abcd", 4);
    ws_mask(ping + h, 4, mask);
    CHECK(ws_parse_frame(ping, h + 4, &f) == (long)(h + 4) && f.opcode == WS_OP_PING, "ping parses");

    /* Control frames: at most 125 bytes, never fragmented. */
    uint8_t big_ping[200];
    h = ws_encode_header(big_ping, 1, WS_OP_PING, 126, mask);
    CHECK(ws_parse_frame(big_ping, h + 126, &f) < 0, "a 126-byte ping is a protocol error");
    h = ws_encode_header(big_ping, 0, WS_OP_PING, 1, mask);
    CHECK(ws_parse_frame(big_ping, h + 1, &f) < 0, "a fragmented ping is a protocol error");
}

/* Bytes a non-blocking fd has for us right now, up to cap. */
static size_t drain(int fd, uint8_t *buf, size_t cap) {
    size_t got = 0;
    for (int tries = 0; tries < 200 && got < cap; tries++) {
        ssize_t n = read(fd, buf + got, cap - got);
        if (n > 0) got += (size_t)n;
        else if (n == 0) break;
        else if (errno == EAGAIN) usleep(1000);
        else break;
    }
    return got;
}

/* Run the hub once as the poll loop would, with every fd marked ready. */
static void service(struct ws_hub *hub, int64_t now) {
    struct pollfd pfd[WS_MAX_CLIENTS];
    int64_t wait = -1;
    int n = ws_hub_prepare(hub, pfd, WS_MAX_CLIENTS, &wait, now);
    for (int i = 0; i < n; i++) pfd[i].revents = pfd[i].events;
    ws_hub_service(hub, pfd, n, now);
}

static void test_hub(void) {
    struct ws_hub hub;
    ws_hub_init(&hub, 4096);
    hub.log = NULL; /* the drops below are on purpose */
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    CHECK(ws_hub_add(&hub, sv[0], "dGhlIHNhbXBsZSBub25jZQ==", "{\"type\":\"hello\"}", 0) == 0, "add");
    CHECK(ws_hub_count(&hub) == 1, "one client");

    uint8_t buf[8192];
    size_t n = drain(sv[1], buf, sizeof buf);
    buf[n < sizeof buf ? n : sizeof buf - 1] = 0;
    CHECK(strstr((char *)buf, "HTTP/1.1 101 Switching Protocols\r\n") == (char *)buf, "101 first: %s", buf);
    CHECK(strstr((char *)buf, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"), "accept header");
    char *end = strstr((char *)buf, "\r\n\r\n");
    CHECK(end != NULL, "end of head");
    struct ws_frame f;
    uint8_t *frame = (uint8_t *)end + 4;
    CHECK(ws_parse_frame(frame, n - (size_t)(frame - buf), &f) > 0 && f.opcode == WS_OP_TEXT && !f.masked,
          "hello frame");
    CHECK(f.len == 16 && !memcmp(f.payload, "{\"type\":\"hello\"}", 16), "hello payload");

    ws_hub_broadcast(&hub, "{\"a\":1}", 7, 1);
    n = drain(sv[1], buf, sizeof buf);
    CHECK(ws_parse_frame(buf, n, &f) == 9 && f.len == 7 && !memcmp(f.payload, "{\"a\":1}", 7), "broadcast frame");

    /* A masked ping from the client gets a pong with the same payload. */
    static const uint8_t mask[4] = {9, 8, 7, 6};
    uint8_t ping[16];
    size_t h = ws_encode_header(ping, 1, WS_OP_PING, 3, mask);
    memcpy(ping + h, "xyz", 3);
    ws_mask(ping + h, 3, mask);
    CHECK(write(sv[1], ping, h + 3) == (ssize_t)(h + 3), "send ping");
    service(&hub, 2);
    n = drain(sv[1], buf, sizeof buf);
    CHECK(ws_parse_frame(buf, n, &f) == 5 && f.opcode == WS_OP_PONG && !memcmp(f.payload, "xyz", 3), "pong");

    /* An unmasked client frame is a protocol error: close 1002, then the socket closes. */
    uint8_t bad[WS_MAX_HEADER + 1];
    h = ws_encode_header(bad, 1, WS_OP_TEXT, 1, NULL);
    bad[h] = 'x';
    CHECK(write(sv[1], bad, h + 1) == (ssize_t)(h + 1), "send unmasked");
    service(&hub, 3);
    service(&hub, 4);
    n = drain(sv[1], buf, sizeof buf);
    CHECK(ws_parse_frame(buf, n, &f) > 0 && f.opcode == WS_OP_CLOSE && ws_close_code(&f) == 1002, "close 1002");
    CHECK(ws_hub_count(&hub) == 0, "client gone after a protocol error");
    close(sv[1]);

    /* A fragmented text message is refused with 1003. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    ws_hub_add(&hub, sv[0], "dGhlIHNhbXBsZSBub25jZQ==", NULL, 10);
    drain(sv[1], buf, sizeof buf);
    h = ws_encode_header(bad, 0, WS_OP_TEXT, 1, mask);
    bad[h] = 'x';
    CHECK(write(sv[1], bad, h + 1) == (ssize_t)(h + 1), "send fragment");
    service(&hub, 11);
    service(&hub, 12);
    n = drain(sv[1], buf, sizeof buf);
    CHECK(ws_parse_frame(buf, n, &f) > 0 && f.opcode == WS_OP_CLOSE && ws_close_code(&f) == 1003, "close 1003");
    CHECK(ws_hub_count(&hub) == 0, "client gone after a fragment");
    close(sv[1]);

    /* The client's close is answered with a close carrying the same code. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    ws_hub_add(&hub, sv[0], "dGhlIHNhbXBsZSBub25jZQ==", NULL, 20);
    drain(sv[1], buf, sizeof buf);
    uint8_t cl[16];
    h = ws_encode_header(cl, 1, WS_OP_CLOSE, 2, mask);
    cl[h] = 0x03;
    cl[h + 1] = 0xe8; /* 1000 */
    ws_mask(cl + h, 2, mask);
    CHECK(write(sv[1], cl, h + 2) == (ssize_t)(h + 2), "send close");
    service(&hub, 21);
    service(&hub, 22);
    n = drain(sv[1], buf, sizeof buf);
    CHECK(ws_parse_frame(buf, n, &f) > 0 && f.opcode == WS_OP_CLOSE && ws_close_code(&f) == 1000, "close echoed");
    CHECK(ws_hub_count(&hub) == 0, "client gone after closing");
    close(sv[1]);

    /* A client that never reads: the queue (4096 bytes here) overflows and it is dropped. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    int small = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    ws_hub_add(&hub, sv[0], "dGhlIHNhbXBsZSBub25jZQ==", NULL, 30);
    char msg[200];
    memset(msg, 'x', sizeof msg);
    for (int i = 0; i < 2000 && ws_hub_count(&hub); i++) ws_hub_broadcast(&hub, msg, sizeof msg, 31);
    CHECK(ws_hub_count(&hub) == 0, "the slow client was dropped");
    CHECK(hub.dropped_slow == 1, "counted as dropped (%lu)", hub.dropped_slow);
    close(sv[1]);

    /* A silent client is pinged, then dropped when nothing comes back. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    ws_hub_add(&hub, sv[0], "dGhlIHNhbXBsZSBub25jZQ==", NULL, 1000);
    drain(sv[1], buf, sizeof buf);
    service(&hub, 1000 + WS_PING_INTERVAL_MS);
    n = drain(sv[1], buf, sizeof buf);
    CHECK(ws_parse_frame(buf, n, &f) > 0 && f.opcode == WS_OP_PING, "ping after the interval");
    service(&hub, 1000 + WS_IDLE_TIMEOUT_MS);
    CHECK(ws_hub_count(&hub) == 0, "dropped after the idle timeout");
    close(sv[1]);
    ws_hub_free(&hub);
}

int main(void) {
    test_sha1();
    test_accept_key();
    test_headers_and_query();
    test_frames();
    test_close_and_ping();
    test_hub();
    return test_finish("test_ws");
}
