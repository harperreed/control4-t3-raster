/* ABOUTME: The messages between tt7d and its camera worker process over a socketpair: an 8-byte header
 * ABOUTME: (type, payload length) and a payload. Pure encode/parse helpers, so framing is unit tested. */
#ifndef TT7D_CAMERA_PROTO_H
#define TT7D_CAMERA_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* Both ends are the same binary on the same machine, so the header uses
 * host byte order. */
enum camera_msg_type {
    CMSG_SNAPSHOT = 1, /* tt7d -> worker: send a JPEG (no payload) */
    CMSG_TICK = 2,     /* worker -> tt7d: one presence frame was scored (struct camera_tick) */
    CMSG_JPEG = 3,     /* worker -> tt7d: the JPEG asked for */
    CMSG_ERROR = 4,    /* worker -> tt7d: what failed, as text */
};

#define CAMERA_MSG_HEADER 8
#define CAMERA_MSG_MAX_PAYLOAD (4u << 20) /* a 1280x720 JPEG at quality 80 is far smaller */

struct camera_tick {
    int32_t present; /* the detector's presence after this frame */
    uint32_t frame;  /* frames scored since the worker started */
    float score;     /* motion_result.score */
};

void camera_msg_header(uint8_t out[CAMERA_MSG_HEADER], uint32_t type, uint32_t len);

/* Look for one whole message at the start of buf[0..len). Returns its size
 * (header + payload) with *type, *payload and *plen set; 0 if more bytes are
 * needed; -1 if the header cannot be right (unknown type, payload too long,
 * or a tick of the wrong size): the stream is then out of step. */
long camera_msg_parse(const uint8_t *buf, size_t len, uint32_t *type, const uint8_t **payload, uint32_t *plen);

#endif
