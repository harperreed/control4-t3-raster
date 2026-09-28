/* ABOUTME: Update bundles (a ustar tar with manifest.json): tar walk, manifest parse, hash and signature checks.
 * ABOUTME: Pure functions over a buffer in memory, so tests feed them hand-made tars; no file I/O here. */
#ifndef TT7D_BUNDLE_H
#define TT7D_BUNDLE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "sign.h"

#define BUNDLE_FORMAT "tt7-bundle"
#define BUNDLE_MANIFEST "manifest.json"
#define BUNDLE_SIGNATURE "manifest.sig" /* 128 hex digits: ed25519 over manifest.json's exact bytes */
#define BUNDLE_MANIFEST_MAX 4096
#define BUNDLE_MAX_FILES 4 /* one per allowed payload path */
#define BUNDLE_ID_MAX 64   /* a release id is the manifest's build */

/* The payload paths a bundle may carry, relative to the release dir. */
extern const char *const bundle_allowed_paths[BUNDLE_MAX_FILES];

struct bundle_file {
    char path[32];
    char sha256[65]; /* lowercase hex, from the manifest */
    unsigned mode;   /* 0755 or 0644, from the manifest */
    const uint8_t *data; /* into the tar buffer, once matched */
    size_t len;
};

struct bundle {
    char version[32];  /* dotted numbers, e.g. "0.1.0" */
    char build[BUNDLE_ID_MAX + 1]; /* git describe; also the release id */
    char created[40];
    struct bundle_file files[BUNDLE_MAX_FILES];
    int nfiles;
    size_t payload_bytes; /* sum of the files' sizes */
    int signed_ok;        /* 1 if manifest.sig was present and checked against a configured key */
};

/* Why a bundle was refused. code is a stable API error code (tt7d/README.md);
 * member and detail say where and what, "" when not relevant. */
struct bundle_error {
    const char *code;
    char member[112];
    char detail[160];
    char expected[65], computed[65]; /* for hash_mismatch */
};

/* Parse and validate manifest.json. Unknown members are ignored; everything
 * the installer relies on is checked: format "tt7-bundle", a dotted numeric
 * version, a build usable as a directory name (bundle_id_valid), a created
 * string, and files: each an allowed path listed once, 64 hex digits of
 * sha256, mode "0755" or "0644". bin/tt7d and app must be listed. Returns 0,
 * or -1 with e->code "invalid_manifest" (or "unknown_file"/"unsafe_path" for
 * a listed path that is not allowed). */
int bundle_manifest_parse(const char *json, size_t len, struct bundle *b, struct bundle_error *e);

/* Check a whole bundle held in memory: walk the tar (regular files only,
 * checksummed ustar headers, no path outside the allowed set, no duplicates),
 * parse its manifest, match every listed file to a member and back, and check
 * each file's sha256. With pubkey non-NULL the bundle must carry a valid
 * manifest.sig for that key. On success b's files point into tar. Returns 0,
 * or -1 with e filled. */
int bundle_verify(const uint8_t *tar, size_t len, const uint8_t *pubkey, struct bundle *b, struct bundle_error *e);

/* 1 if s can name a release directory: 1..BUNDLE_ID_MAX of [A-Za-z0-9._+-],
 * not starting with '.' or '-'. */
int bundle_id_valid(const char *s);

/* Compare dotted numeric versions ("0.10.2" > "0.9"); missing parts count as 0.
 * Returns <0, 0 or >0, or BUNDLE_VERSION_BAD if either is not dotted numbers. */
#define BUNDLE_VERSION_BAD (-99)
int bundle_version_cmp(const char *a, const char *b);

/* Which releases to delete so that at most `keep` remain. Never current or
 * previous (either may be NULL); the rest go oldest first by mtime (ties by
 * name). Writes the indexes to delete into out (room for n) and returns how
 * many. */
struct release_entry {
    const char *id;
    time_t mtime;
};
int bundle_prune_plan(const struct release_entry *list, int n, const char *current, const char *previous, int keep,
                      int *out);

#endif
