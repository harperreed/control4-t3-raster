/* ABOUTME: Update bundle checks: walks a ustar tar in memory, parses manifest.json with a strict little
 * ABOUTME: JSON reader, matches files both ways, checks sha256 and the optional ed25519 signature. */
#include "bundle.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"

const char *const bundle_allowed_paths[BUNDLE_MAX_FILES] = {"bin/tt7d", "bin/tt7probe", "bin/tt7-ntp-hook", "app"};

#define JSON_MAX_DEPTH 16

static int fail(struct bundle_error *e, const char *code, const char *member, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static int fail(struct bundle_error *e, const char *code, const char *member, const char *fmt, ...) {
    e->code = code;
    snprintf(e->member, sizeof e->member, "%s", member ? member : "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->detail, sizeof e->detail, fmt, ap);
    va_end(ap);
    return -1;
}

static int is_allowed(const char *path) {
    for (int i = 0; i < BUNDLE_MAX_FILES; i++)
        if (!strcmp(path, bundle_allowed_paths[i])) return 1;
    return 0;
}

/* Absolute, or with a ".." component: never acceptable, whatever the allowlist says. */
static int is_unsafe(const char *path) {
    if (path[0] == '/') return 1;
    for (const char *p = path; *p;) {
        size_t n = strcspn(p, "/");
        if (n == 2 && p[0] == '.' && p[1] == '.') return 1;
        p += n;
        if (*p == '/') p++;
    }
    return 0;
}

/* The path rules shared by manifest entries and tar members. */
static int check_path(const char *path, const char *what, struct bundle_error *e) {
    if (is_unsafe(path)) return fail(e, "unsafe_path", path, "%s path is absolute or leaves the release directory", what);
    if (!is_allowed(path))
        return fail(e, "unknown_file", path, "%s path is not one of bin/tt7d, bin/tt7probe, bin/tt7-ntp-hook, app", what);
    return 0;
}

int bundle_id_valid(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > BUNDLE_ID_MAX || s[0] == '.' || s[0] == '-') return 0;
    return strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-") == n;
}

/* Next dotted part of a version: 1-9 digits. Returns -1 if malformed. */
static int version_part(const char **p, unsigned long *v) {
    size_t n = strspn(*p, "0123456789");
    if (n < 1 || n > 9) return -1;
    *v = strtoul(*p, NULL, 10);
    *p += n;
    if (**p == '.') {
        (*p)++;
        if (!**p) return -1; /* trailing dot */
    } else if (**p) {
        return -1;
    }
    return 0;
}

static int version_valid(const char *s) {
    unsigned long v;
    if (!*s) return 0;
    while (*s)
        if (version_part(&s, &v) != 0) return 0;
    return 1;
}

int bundle_version_cmp(const char *a, const char *b) {
    if (!version_valid(a) || !version_valid(b)) return BUNDLE_VERSION_BAD;
    while (*a || *b) {
        unsigned long va = 0, vb = 0;
        if (*a) version_part(&a, &va);
        if (*b) version_part(&b, &vb);
        if (va != vb) return va < vb ? -1 : 1;
    }
    return 0;
}

/* ---- a strict JSON reader, just enough for manifest.json ---------------------------- */

struct jp {
    const char *p, *end;
    int depth;
};

static void ws(struct jp *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int eat(struct jp *j, char c) {
    ws(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return 1;
    }
    return 0;
}

/* A string into out (NULL: skip it). Simple escapes only: a manifest has no
 * use for \u, and every value it needs is printable ASCII. -1 if malformed or
 * too long for out. */
static int jstring(struct jp *j, char *out, size_t size) {
    ws(j);
    if (j->p >= j->end || *j->p != '"') return -1;
    j->p++;
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if ((unsigned char)c < 0x20) return -1;
        if (c == '\\') {
            if (j->p >= j->end) return -1;
            char x = *j->p++;
            if (x == 'n') c = '\n';
            else if (x == 't') c = '\t';
            else if (x == '"' || x == '\\' || x == '/') c = x;
            else return -1;
        }
        if (out) {
            if (n + 1 >= size) return -1;
            out[n] = c;
        }
        n++;
    }
    if (j->p >= j->end) return -1;
    j->p++;
    if (out) out[n] = 0;
    return 0;
}

static int jskip(struct jp *j);

/* Walk an object or array, skipping every value. */
static int jskip_container(struct jp *j, char open) {
    char close = open == '{' ? '}' : ']';
    if (++j->depth > JSON_MAX_DEPTH) return -1;
    j->p++;
    if (eat(j, close)) {
        j->depth--;
        return 0;
    }
    do {
        if (open == '{' && (jstring(j, NULL, 0) != 0 || !eat(j, ':'))) return -1;
        if (jskip(j) != 0) return -1;
    } while (eat(j, ','));
    if (!eat(j, close)) return -1;
    j->depth--;
    return 0;
}

static int jskip(struct jp *j) {
    ws(j);
    if (j->p >= j->end) return -1;
    char c = *j->p;
    if (c == '"') return jstring(j, NULL, 0);
    if (c == '{' || c == '[') return jskip_container(j, c);
    static const char *const words[] = {"true", "false", "null"};
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(words[i]);
        if ((size_t)(j->end - j->p) >= n && !memcmp(j->p, words[i], n)) {
            j->p += n;
            return 0;
        }
    }
    const char *start = j->p;
    while (j->p < j->end && strchr("-+0123456789.eE", *j->p)) j->p++;
    return j->p > start ? 0 : -1;
}

static int bad_manifest(struct bundle_error *e, const char *fmt, const char *arg) {
    return fail(e, "invalid_manifest", "", fmt, arg);
}

/* One {"path", "sha256", "mode"} entry of "files". */
static int parse_file(struct jp *j, struct bundle *b, struct bundle_error *e) {
    if (b->nfiles >= BUNDLE_MAX_FILES) return bad_manifest(e, "files lists more than %s entries", "4");
    struct bundle_file *f = &b->files[b->nfiles];
    memset(f, 0, sizeof *f);
    char mode[8] = "";
    int seen_path = 0, seen_sha = 0, seen_mode = 0;
    if (!eat(j, '{')) return bad_manifest(e, "files entries must be objects%s", "");
    if (!eat(j, '}')) {
        do {
            char key[32];
            if (jstring(j, key, sizeof key) != 0 || !eat(j, ':')) return bad_manifest(e, "malformed files entry%s", "");
            int *seen = NULL;
            char *dst = NULL;
            size_t dsize = 0;
            if (!strcmp(key, "path")) seen = &seen_path, dst = f->path, dsize = sizeof f->path;
            else if (!strcmp(key, "sha256")) seen = &seen_sha, dst = f->sha256, dsize = sizeof f->sha256;
            else if (!strcmp(key, "mode")) seen = &seen_mode, dst = mode, dsize = sizeof mode;
            if (!seen) {
                if (jskip(j) != 0) return bad_manifest(e, "malformed value in a files entry%s", "");
                continue;
            }
            if (*seen) return bad_manifest(e, "files entry has \"%s\" twice", key);
            if (jstring(j, dst, dsize) != 0) return bad_manifest(e, "files \"%s\" must be a short JSON string", key);
            *seen = 1;
        } while (eat(j, ','));
        if (!eat(j, '}')) return bad_manifest(e, "malformed files entry%s", "");
    }
    if (!seen_path || !seen_sha || !seen_mode) return bad_manifest(e, "each files entry needs path, sha256 and mode%s", "");
    if (check_path(f->path, "manifest", e) != 0) return -1;
    for (int i = 0; i < b->nfiles; i++)
        if (!strcmp(b->files[i].path, f->path)) return fail(e, "invalid_manifest", f->path, "listed twice");
    if (strlen(f->sha256) != 64 || strspn(f->sha256, "0123456789abcdef") != 64)
        return fail(e, "invalid_manifest", f->path, "sha256 must be 64 lowercase hex digits");
    if (strcmp(mode, "0755") && strcmp(mode, "0644"))
        return fail(e, "invalid_manifest", f->path, "mode must be \"0755\" or \"0644\"");
    f->mode = mode[1] == '7' ? 0755 : 0644;
    b->nfiles++;
    return 0;
}

static int has_file(const struct bundle *b, const char *path) {
    for (int i = 0; i < b->nfiles; i++)
        if (!strcmp(b->files[i].path, path)) return 1;
    return 0;
}

int bundle_manifest_parse(const char *json, size_t len, struct bundle *b, struct bundle_error *e) {
    memset(b, 0, sizeof *b);
    struct jp j = {json, json + len, 0};
    char format[16] = "";
    int seen[5] = {0}; /* format, version, build, created, files */
    if (!eat(&j, '{')) return bad_manifest(e, "not a JSON object%s", "");
    if (!eat(&j, '}')) {
        do {
            char key[32];
            if (jstring(&j, key, sizeof key) != 0 || !eat(&j, ':')) return bad_manifest(e, "malformed member name%s", "");
            static const char *const keys[] = {"format", "version", "build", "created", "files"};
            int k = -1;
            for (int i = 0; i < 5; i++)
                if (!strcmp(key, keys[i])) k = i;
            if (k < 0) {
                if (jskip(&j) != 0) return bad_manifest(e, "malformed value%s", "");
                continue;
            }
            if (seen[k]) return bad_manifest(e, "\"%s\" appears twice", key);
            seen[k] = 1;
            char *dst[] = {format, b->version, b->build, b->created};
            size_t dsize[] = {sizeof format, sizeof b->version, sizeof b->build, sizeof b->created};
            if (k < 4) {
                if (jstring(&j, dst[k], dsize[k]) != 0) return bad_manifest(e, "\"%s\" must be a short JSON string", key);
                continue;
            }
            if (!eat(&j, '[')) return bad_manifest(e, "\"files\" must be an array%s", "");
            if (!eat(&j, ']')) {
                do {
                    if (parse_file(&j, b, e) != 0) return -1;
                } while (eat(&j, ','));
                if (!eat(&j, ']')) return bad_manifest(e, "malformed \"files\" array%s", "");
            }
        } while (eat(&j, ','));
        if (!eat(&j, '}')) return bad_manifest(e, "malformed object%s", "");
    }
    ws(&j);
    if (j.p != j.end) return bad_manifest(e, "text after the JSON object%s", "");
    static const char *const names[] = {"format", "version", "build", "created", "files"};
    for (int i = 0; i < 5; i++)
        if (!seen[i]) return bad_manifest(e, "\"%s\" is missing", names[i]);
    if (strcmp(format, BUNDLE_FORMAT)) return bad_manifest(e, "format must be \"%s\"", BUNDLE_FORMAT);
    if (bundle_version_cmp(b->version, b->version) == BUNDLE_VERSION_BAD)
        return bad_manifest(e, "version must be dotted numbers like 0.1.0, not \"%s\"", b->version);
    if (!bundle_id_valid(b->build))
        return bad_manifest(e, "build \"%s\" cannot name a release: 1-64 of A-Z a-z 0-9 . _ + -, not starting with . or -",
                            b->build);
    if (!has_file(b, "bin/tt7d") || !has_file(b, "app")) return bad_manifest(e, "files must list bin/tt7d and app%s", "");
    return 0;
}

/* ---- tar ------------------------------------------------------------------------------- */

#define TAR_BLOCK 512

struct member {
    char name[256];
    const uint8_t *data;
    size_t len;
};

/* An octal header field: optional leading spaces, octal digits, then NUL or
 * space to the end of the field. -1 for anything else (GNU base-256 included). */
static int octal_field(const uint8_t *f, size_t width, unsigned long long *out) {
    size_t i = 0;
    while (i < width && f[i] == ' ') i++;
    size_t start = i;
    unsigned long long v = 0;
    while (i < width && f[i] >= '0' && f[i] <= '7') {
        v = v * 8 + (unsigned)(f[i] - '0');
        if (v > (1ull << 40)) return -1;
        i++;
    }
    if (i == start) return -1;
    for (; i < width; i++)
        if (f[i] != 0 && f[i] != ' ') return -1;
    *out = v;
    return 0;
}

static int all_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

static const char *type_name(uint8_t t) {
    switch (t) {
    case '1': return "a hard link";
    case '2': return "a symbolic link";
    case '3': return "a character device";
    case '4': return "a block device";
    case '5': return "a directory";
    case '6': return "a FIFO";
    case 'x': case 'g': return "a pax extended header";
    case 'L': case 'K': return "a GNU long name";
    default: return "not a regular file";
    }
}

/* Walk the tar. Payload members go to m (up to BUNDLE_MAX_FILES), the manifest
 * and signature to their own slots. */
static int walk_tar(const uint8_t *tar, size_t len, struct member *m, int *nm, struct member *manifest,
                    struct member *sig, struct bundle_error *e) {
    *nm = 0;
    if (len < TAR_BLOCK) return fail(e, "invalid_bundle", "", "shorter than one tar block");
    size_t off = 0;
    while (off < len) {
        if (len - off < TAR_BLOCK) return fail(e, "invalid_bundle", "", "ends in a partial tar block");
        const uint8_t *h = tar + off;
        if (all_zero(h, TAR_BLOCK)) return 0; /* the end-of-archive block */
        unsigned long long want, size;
        if (octal_field(h + 148, 8, &want) != 0) return fail(e, "invalid_bundle", "", "bad header checksum field at byte %zu", off);
        unsigned long long sum = 0;
        for (int i = 0; i < TAR_BLOCK; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
        if (sum != want) return fail(e, "invalid_bundle", "", "header checksum mismatch at byte %zu", off);
        if (memcmp(h + 257, "ustar", 5) != 0) return fail(e, "invalid_bundle", "", "not a ustar header at byte %zu", off);

        struct member cur = {{0}, NULL, 0};
        size_t plen = strnlen((const char *)h + 345, 155), nlen = strnlen((const char *)h, 100);
        if (plen) snprintf(cur.name, sizeof cur.name, "%.*s/%.*s", (int)plen, h + 345, (int)nlen, h);
        else snprintf(cur.name, sizeof cur.name, "%.*s", (int)nlen, h);

        uint8_t type = h[156];
        if (type != '0' && type != 0)
            return fail(e, "bad_member_type", cur.name, "%s; only regular files are allowed", type_name(type));
        if (octal_field(h + 124, 12, &size) != 0) return fail(e, "invalid_bundle", cur.name, "bad size field");
        off += TAR_BLOCK;
        if (size > len - off) return fail(e, "invalid_bundle", cur.name, "the archive ends inside this member");
        cur.data = tar + off;
        cur.len = (size_t)size;
        off += (cur.len + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
        if (off > len) off = len; /* the last member's padding may be cut; the loop then ends */

        struct member *slot;
        if (!strcmp(cur.name, BUNDLE_MANIFEST)) {
            slot = manifest;
        } else if (!strcmp(cur.name, BUNDLE_SIGNATURE)) {
            slot = sig;
        } else {
            if (check_path(cur.name, "tar member", e) != 0) return -1;
            slot = NULL;
            for (int i = 0; i < *nm; i++)
                if (!strcmp(m[i].name, cur.name)) slot = &m[i];
            if (!slot) {
                m[*nm] = cur; /* at most BUNDLE_MAX_FILES distinct allowed names */
                (*nm)++;
                continue;
            }
        }
        if (slot->data) return fail(e, "duplicate_member", cur.name, "this name appears twice in the tar");
        *slot = cur;
    }
    return 0; /* no end block: tolerated, every member was whole */
}

int bundle_verify(const uint8_t *tar, size_t len, const uint8_t *pubkey, struct bundle *b, struct bundle_error *e) {
    struct member m[BUNDLE_MAX_FILES], manifest = {{0}, NULL, 0}, sig = {{0}, NULL, 0};
    int nm;
    memset(b, 0, sizeof *b);
    if (walk_tar(tar, len, m, &nm, &manifest, &sig, e) != 0) return -1;
    if (!manifest.data) return fail(e, "no_manifest", BUNDLE_MANIFEST, "the bundle has no manifest.json");
    if (manifest.len > BUNDLE_MANIFEST_MAX) return fail(e, "invalid_manifest", BUNDLE_MANIFEST, "larger than 4096 bytes");
    if (bundle_manifest_parse((const char *)manifest.data, manifest.len, b, e) != 0) return -1;

    /* Authenticity before contents: with a key configured, nothing unsigned goes further. */
    if (pubkey) {
        uint8_t s[SIGN_SIG_BYTES];
        if (!sig.data) return fail(e, "signature_required", BUNDLE_SIGNATURE, "this device only takes signed bundles");
        if (sign_hex_decode((const char *)sig.data, sig.len, s, sizeof s) != 0)
            return fail(e, "invalid_signature", BUNDLE_SIGNATURE, "must be 128 hex digits");
        if (!sign_verify(pubkey, s, manifest.data, manifest.len))
            return fail(e, "invalid_signature", BUNDLE_SIGNATURE, "does not match manifest.json and the configured key");
        b->signed_ok = 1;
    }

    for (int i = 0; i < b->nfiles; i++) {
        struct bundle_file *f = &b->files[i];
        for (int k = 0; k < nm; k++)
            if (!strcmp(m[k].name, f->path)) f->data = m[k].data, f->len = m[k].len;
        if (!f->data) return fail(e, "missing_file", f->path, "listed in the manifest but not in the tar");
    }
    for (int k = 0; k < nm; k++)
        if (!has_file(b, m[k].name)) return fail(e, "unlisted_file", m[k].name, "in the tar but not in the manifest");
    for (int i = 0; i < b->nfiles; i++) {
        struct bundle_file *f = &b->files[i];
        char got[65];
        sha256_hex(f->data, f->len, got);
        if (strcmp(got, f->sha256)) {
            fail(e, "hash_mismatch", f->path, "sha256 differs from the manifest");
            memcpy(e->expected, f->sha256, 65);
            memcpy(e->computed, got, 65);
            return -1;
        }
        b->payload_bytes += f->len;
    }
    return 0;
}

/* ---- pruning --------------------------------------------------------------------------- */

static const struct release_entry *sort_base;

static int by_age(const void *x, const void *y) {
    const struct release_entry *a = &sort_base[*(const int *)x], *b = &sort_base[*(const int *)y];
    if (a->mtime != b->mtime) return a->mtime < b->mtime ? -1 : 1;
    return strcmp(a->id, b->id);
}

int bundle_prune_plan(const struct release_entry *list, int n, const char *current, const char *previous, int keep,
                      int *out) {
    int nc = 0;
    for (int i = 0; i < n; i++) {
        if ((current && !strcmp(list[i].id, current)) || (previous && !strcmp(list[i].id, previous))) continue;
        out[nc++] = i;
    }
    int excess = n - keep;
    if (excess <= 0) return 0;
    sort_base = list;
    qsort(out, (size_t)nc, sizeof *out, by_age);
    return excess < nc ? excess : nc;
}
