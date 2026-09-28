/* ABOUTME: ed25519 signature check (RFC 8032) for update bundles, over vendored TweetNaCl.
 * ABOUTME: Verify only: tt7d never makes keys or signs; tools/make-bundle.sh signs with openssl. */
#ifndef TT7D_SIGN_H
#define TT7D_SIGN_H

#include <stddef.h>
#include <stdint.h>

#define SIGN_PUBKEY_BYTES 32
#define SIGN_SIG_BYTES 64

/* Parse exactly 2*n hex digits (either case) into out. Leading and trailing
 * whitespace (spaces, tabs, CR, LF) is allowed around them. Returns 0 or -1. */
int sign_hex_decode(const char *text, size_t len, uint8_t *out, size_t n);

/* 1 if sig is a valid ed25519 signature of msg[0..len) under pubkey, else 0.
 * Returns 0 too if memory for the check cannot be had. */
int sign_verify(const uint8_t pubkey[SIGN_PUBKEY_BYTES], const uint8_t sig[SIGN_SIG_BYTES], const uint8_t *msg,
                size_t len);

#endif
