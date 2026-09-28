/* ABOUTME: SHA-256 (FIPS 180-4) for frame hashes: deduplication and the X-Frame-SHA256 check.
 * ABOUTME: Plain portable C, checked against the FIPS test vectors in test_util.c. */
#ifndef TT7D_SHA256_H
#define TT7D_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* Hash `len` bytes and write 64 lowercase hex digits plus a NUL to hex. */
void sha256_hex(const uint8_t *data, size_t len, char hex[65]);

#endif
