/* ABOUTME: SHA-1 (RFC 3174), one-shot over a memory buffer. Only for the WebSocket handshake's
 * ABOUTME: Sec-WebSocket-Accept (RFC 6455 fixes SHA-1 there); never use it for anything that needs security. */
#ifndef TT7D_SHA1_H
#define TT7D_SHA1_H

#include <stddef.h>
#include <stdint.h>

/* The 20-byte digest of `len` bytes. */
void sha1(const uint8_t *data, size_t len, uint8_t digest[20]);

#endif
