/* ABOUTME: ed25519 verification for update bundles: hex parsing plus TweetNaCl's crypto_sign_open.
 * ABOUTME: Compiles the vendored, unmodified tweetnacl.c into this file (third_party/tweetnacl/PROVENANCE). */
#include "sign.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Two warnings the unmodified upstream file raises under -Wall -Wextra, for
 * this file only. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunterminated-string-initialization"
#include "tweetnacl.c"
#pragma GCC diagnostic pop

/* TweetNaCl calls this only to make keys, which tt7d never does. Failing
 * loudly beats handing out a key made from no randomness. */
void randombytes(u8 *out, u64 n) {
    (void)out;
    (void)n;
    fprintf(stderr, "tt7d: randombytes called; tt7d never generates keys\n");
    abort();
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int sign_hex_decode(const char *text, size_t len, uint8_t *out, size_t n) {
    while (len && is_space(*text)) text++, len--;
    while (len && is_space(text[len - 1])) len--;
    if (len != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(text[2 * i]), lo = hexval(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

/* crypto_sign_open wants the signature and message joined (sm = sig || msg)
 * and room for the same length out. */
int sign_verify(const uint8_t pubkey[SIGN_PUBKEY_BYTES], const uint8_t sig[SIGN_SIG_BYTES], const uint8_t *msg,
                size_t len) {
    size_t smlen = SIGN_SIG_BYTES + len;
    uint8_t *sm = malloc(smlen), *m = malloc(smlen);
    int ok = 0;
    if (sm && m) {
        memcpy(sm, sig, SIGN_SIG_BYTES);
        if (len) memcpy(sm + SIGN_SIG_BYTES, msg, len);
        unsigned long long mlen;
        ok = crypto_sign_open(m, &mlen, sm, smlen, pubkey) == 0 && mlen == len;
    }
    free(sm);
    free(m);
    return ok;
}
