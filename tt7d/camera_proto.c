/* ABOUTME: Framing for the tt7d <-> camera worker socketpair: a (type, length) header, then the payload.
 * ABOUTME: A bad header means the stream is out of step; the supervisor then restarts the worker. */
#include "camera_proto.h"

#include <string.h>

void camera_msg_header(uint8_t out[CAMERA_MSG_HEADER], uint32_t type, uint32_t len) {
    memcpy(out, &type, 4);
    memcpy(out + 4, &len, 4);
}

long camera_msg_parse(const uint8_t *buf, size_t len, uint32_t *type, const uint8_t **payload, uint32_t *plen) {
    if (len < CAMERA_MSG_HEADER) return 0;
    uint32_t t, n;
    memcpy(&t, buf, 4);
    memcpy(&n, buf + 4, 4);
    if (t < CMSG_SNAPSHOT || t > CMSG_ERROR || n > CAMERA_MSG_MAX_PAYLOAD) return -1;
    if (t == CMSG_TICK && n != sizeof(struct camera_tick)) return -1;
    if (len - CAMERA_MSG_HEADER < n) return 0;
    *type = t;
    *payload = buf + CAMERA_MSG_HEADER;
    *plen = n;
    return (long)(CAMERA_MSG_HEADER + n);
}
