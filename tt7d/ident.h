/* ABOUTME: Secrets and identity kept in the data dir: the bearer token and the persistent device id.
 * ABOUTME: Also the constant-time token compare and the X-Frame-ID syntax rule. */
#ifndef TT7D_IDENT_H
#define TT7D_IDENT_H

#include <stddef.h>

/* 1 if a and b are equal. Runs in time that depends only on the length of
 * `expected`, never on where the strings first differ. An empty expected
 * token never matches. */
int token_equal(const char *expected, const char *given);

/* Fill hex with 2*nbytes random lowercase hex digits from /dev/urandom plus a
 * NUL. Returns 0, or -1 if /dev/urandom failed. */
int random_hex(char *hex, size_t nbytes);

/* Load <dir>/token, creating it (64 hex digits, mode 0600) if absent.
 * Returns 0, or -1 with a message in err. */
int token_load(const char *dir, char *token, size_t size, char *err, size_t errlen);

/* Load the device id ("tt7-" + 6 hex) from <dir>/device.json, creating the
 * file once if absent. An existing file that does not parse is never
 * overwritten: returns -1 with a message, and the caller reports null. */
int device_id_load(const char *dir, char *id, size_t size, char *err, size_t errlen);

/* X-Frame-ID rule: 1 to 128 printable ASCII characters, no spaces. */
int frame_id_valid(const char *id);

/* Write data to path atomically: <path>.tmp, fsync, rename, fsync the dir.
 * Returns 0, or -1 with errno set. */
int write_file_atomic(const char *path, const void *data, size_t len, int mode);

#endif
