/* ABOUTME: Host unit tests for update bundles: manifest parsing, tar member rules, hash and ed25519 checks
 * ABOUTME: (RFC 8032 vectors), version order and release pruning. Tars are built here, byte by byte. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bundle.h"
#include "sha256.h"
#include "sign.h"
#include "test_common.h"

/* TweetNaCl's signer, compiled into sign.c; only the tests sign. */
int crypto_sign_ed25519_tweet(unsigned char *sm, unsigned long long *smlen, const unsigned char *m,
                              unsigned long long n, const unsigned char *sk);

/* ---- a tiny ustar writer ------------------------------------------------------- */

struct tar {
    uint8_t buf[64 * 1024];
    size_t len;
};

static void octal(char *field, size_t width, unsigned long v) {
    snprintf(field, width, "%0*lo", (int)width - 1, v);
}

/* One member. name goes in the name field; prefix (may be NULL) in the prefix field. */
static void tar_member_ex(struct tar *t, const char *prefix, const char *name, char type, const void *data, size_t n,
                          const char *magic) {
    uint8_t *h = t->buf + t->len;
    memset(h, 0, 512);
    memcpy(h, name, strlen(name));
    octal((char *)h + 100, 8, 0644);
    octal((char *)h + 108, 8, 0);
    octal((char *)h + 116, 8, 0);
    octal((char *)h + 124, 12, (unsigned long)n);
    octal((char *)h + 136, 12, 0);
    h[156] = (uint8_t)type;
    if (type == '2' || type == '1') memcpy(h + 157, "/etc/passwd", 11);
    memcpy(h + 257, magic, 8);
    if (prefix) memcpy(h + 345, prefix, strlen(prefix));
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    t->len += 512;
    if (n) {
        memcpy(t->buf + t->len, data, n);
        t->len += (n + 511) / 512 * 512;
    }
}

static void tar_member(struct tar *t, const char *name, char type, const void *data, size_t n) {
    tar_member_ex(t, NULL, name, type, data, n, "ustar\0" "00");
}

static void tar_file(struct tar *t, const char *name, const char *text) { tar_member(t, name, '0', text, strlen(text)); }

static void tar_end(struct tar *t) {
    memset(t->buf + t->len, 0, 1024);
    t->len += 1024;
}

/* ---- the usual bundle ------------------------------------------------------------- */

static const char *TT7D_BIN = "\x7f" "ELF pretend tt7d";
static const char *APP_SH = "#!/bin/sh\necho app\n";

static char *sha(const char *text) {
    static char hex[4][65];
    static int k;
    char *h = hex[k++ % 4];
    sha256_hex((const uint8_t *)text, strlen(text), h);
    return h;
}

/* A manifest listing bin/tt7d and app with their real hashes. */
static void good_manifest(char *out, size_t size) {
    snprintf(out, size,
             "{\"format\":\"tt7-bundle\",\"version\":\"0.1.0\",\"build\":\"v1-3-gabc123\","
             "\"created\":\"2026-09-28T12:00:00Z\",\"files\":["
             "{\"path\":\"bin/tt7d\",\"sha256\":\"%s\",\"mode\":\"0755\"},"
             "{\"path\":\"app\",\"sha256\":\"%s\",\"mode\":\"0755\"}]}",
             sha(TT7D_BIN), sha(APP_SH));
}

static void good_bundle(struct tar *t) {
    char m[1024];
    good_manifest(m, sizeof m);
    t->len = 0;
    tar_file(t, "manifest.json", m);
    tar_file(t, "bin/tt7d", TT7D_BIN);
    tar_file(t, "app", APP_SH);
    tar_end(t);
}

static int verify(const struct tar *t, const uint8_t *pubkey, struct bundle_error *e) {
    static struct bundle b;
    memset(e, 0, sizeof *e);
    return bundle_verify(t->buf, t->len, pubkey, &b, e);
}

static int manifest(const char *json, struct bundle_error *e) {
    static struct bundle b;
    memset(e, 0, sizeof *e);
    return bundle_manifest_parse(json, strlen(json), &b, e);
}

#define EXPECT_CODE(rc, e, want)                                                                        \
    CHECK((rc) == -1 && (e).code && !strcmp((e).code, want), "want %s, got rc %d code %s (%s %s)", want, rc, \
          (e).code ? (e).code : "(none)", (e).member, (e).detail)

/* ---- manifest ------------------------------------------------------------------- */

static void test_manifest_valid(void) {
    char m[1024];
    good_manifest(m, sizeof m);
    struct bundle b;
    struct bundle_error e = {0};
    CHECK(bundle_manifest_parse(m, strlen(m), &b, &e) == 0, "good manifest: %s %s", e.code, e.detail);
    CHECK(!strcmp(b.version, "0.1.0") && !strcmp(b.build, "v1-3-gabc123"), "version %s build %s", b.version, b.build);
    CHECK(!strcmp(b.created, "2026-09-28T12:00:00Z"), "created");
    CHECK(b.nfiles == 2 && !strcmp(b.files[0].path, "bin/tt7d") && b.files[0].mode == 0755, "files");
    CHECK(!strcmp(b.files[1].sha256, sha(APP_SH)), "app hash");

    /* Unknown members, nested values and whitespace are fine. */
    char m2[1400];
    snprintf(m2, sizeof m2,
             " {\n \"comment\": {\"a\": [1, 2.5e3, true, null, {\"b\": \"c\\\"d\"}]},\n \"format\" : \"tt7-bundle\","
             "\"version\":\"1.2\",\"build\":\"dev\",\"created\":\"x\",\"files\":[{\"path\":\"bin/tt7d\",\"sha256\":"
             "\"%s\",\"mode\":\"0755\",\"extra\":[]},{\"mode\":\"0644\",\"path\":\"app\",\"sha256\":\"%s\"}]}\n",
             sha(TT7D_BIN), sha(APP_SH));
    CHECK(bundle_manifest_parse(m2, strlen(m2), &b, &e) == 0, "extras ignored: %s %s", e.code, e.detail);
    CHECK(b.files[1].mode == 0644, "mode 0644");
}

static void test_manifest_invalid(void) {
    struct bundle_error e;
    const char *h1 = sha(TT7D_BIN), *h2 = sha(APP_SH);
    char m[1400];
    int rc;

    EXPECT_CODE(manifest("", &e), e, "invalid_manifest");
    EXPECT_CODE(manifest("{", &e), e, "invalid_manifest");
    EXPECT_CODE(manifest("[]", &e), e, "invalid_manifest");
    EXPECT_CODE(manifest("{\"format\":\"tt7-bundle\"", &e), e, "invalid_manifest");
    EXPECT_CODE(manifest("{\"format\":tt7-bundle}", &e), e, "invalid_manifest");

#define FMT(fmt, ...) (snprintf(m, sizeof m, fmt, __VA_ARGS__), manifest(m, &e))
#define FILES "[{\"path\":\"bin/tt7d\",\"sha256\":\"%s\",\"mode\":\"0755\"},{\"path\":\"app\",\"sha256\":\"%s\",\"mode\":\"0755\"}]"
    rc = FMT("{\"format\":\"tt7-bundle\",\"version\":\"0.1\",\"build\":\"b\",\"created\":\"c\",\"files\":" FILES "} x", h1, h2);
    EXPECT_CODE(rc, e, "invalid_manifest");
    rc = FMT("{\"format\":\"other\",\"version\":\"0.1\",\"build\":\"b\",\"created\":\"c\",\"files\":" FILES "}", h1, h2);
    EXPECT_CODE(rc, e, "invalid_manifest");
    rc = FMT("{\"format\":\"tt7-bundle\",\"version\":\"0.1\",\"created\":\"c\",\"files\":" FILES "}", h1, h2);
    EXPECT_CODE(rc, e, "invalid_manifest"); /* no build */
    rc = FMT("{\"format\":\"tt7-bundle\",\"version\":\"one\",\"build\":\"b\",\"created\":\"c\",\"files\":" FILES "}", h1, h2);
    EXPECT_CODE(rc, e, "invalid_manifest"); /* version not dotted numbers */
    const char *bad_builds[] = {"../x", ".hidden", "a/b", "", "-rf", "sp ace"};
    for (size_t i = 0; i < sizeof bad_builds / sizeof bad_builds[0]; i++) {
        rc = FMT("{\"format\":\"tt7-bundle\",\"version\":\"0.1\",\"build\":\"%s\",\"created\":\"c\",\"files\":" FILES "}",
                 bad_builds[i], h1, h2);
        EXPECT_CODE(rc, e, "invalid_manifest");
    }
    rc = FMT("{\"format\":\"tt7-bundle\",\"version\":\"0.1\",\"build\":\"b\",\"created\":\"c\",\"files\":"
             "[{\"path\":\"app\",\"sha256\":\"%s\",\"mode\":\"0755\"}]}", h2);
    EXPECT_CODE(rc, e, "invalid_manifest"); /* no bin/tt7d */

    /* Paths: traversal and absolute are unsafe; anything else not allowed is unknown. */
    const struct {
        const char *path, *code;
    } paths[] = {{"../bin/tt7d", "unsafe_path"}, {"/bin/tt7d", "unsafe_path"}, {"bin/../app", "unsafe_path"},
                 {"bin/sh", "unknown_file"}, {"./app", "unknown_file"}, {"bin/tt7d/", "unknown_file"}};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        rc = FMT("{\"format\":\"tt7-bundle\",\"version\":\"0.1\",\"build\":\"b\",\"created\":\"c\",\"files\":"
                 "[{\"path\":\"bin/tt7d\",\"sha256\":\"%s\",\"mode\":\"0755\"},{\"path\":\"app\",\"sha256\":\"%s\","
                 "\"mode\":\"0755\"},{\"path\":\"%s\",\"sha256\":\"%s\",\"mode\":\"0755\"}]}",
                 h1, h2, paths[i].path, h1);
        EXPECT_CODE(rc, e, paths[i].code);
    }
    rc = FMT("{\"format\":\"tt7-bundle\",\"version\":\"0.1\",\"build\":\"b\",\"created\":\"c\",\"files\":"
             "[{\"path\":\"bin/tt7d\",\"sha256\":\"%s\",\"mode\":\"0755\"},{\"path\":\"app\",\"sha256\":\"%s\","
             "\"mode\":\"0755\"},{\"path\":\"app\",\"sha256\":\"%s\",\"mode\":\"0755\"}]}", h1, h2, h2);
    EXPECT_CODE(rc, e, "invalid_manifest"); /* listed twice */
    rc = FMT("{\"format\":\"tt7-bundle\",\"version\":\"0.1\",\"build\":\"b\",\"created\":\"c\",\"files\":"
             "[{\"path\":\"bin/tt7d\",\"sha256\":\"%.63s\",\"mode\":\"0755\"},{\"path\":\"app\",\"sha256\":\"%s\","
             "\"mode\":\"0755\"}]}", h1, h2);
    EXPECT_CODE(rc, e, "invalid_manifest"); /* short hash */
    rc = FMT("{\"format\":\"tt7-bundle\",\"version\":\"0.1\",\"build\":\"b\",\"created\":\"c\",\"files\":"
             "[{\"path\":\"bin/tt7d\",\"sha256\":\"%.63sZ\",\"mode\":\"0755\"},{\"path\":\"app\",\"sha256\":\"%s\","
             "\"mode\":\"0755\"}]}", h1, h2);
    EXPECT_CODE(rc, e, "invalid_manifest"); /* not hex */
    const char *bad_modes[] = {"4755", "0777", "755", "0"};
    for (size_t i = 0; i < sizeof bad_modes / sizeof bad_modes[0]; i++) {
        rc = FMT("{\"format\":\"tt7-bundle\",\"version\":\"0.1\",\"build\":\"b\",\"created\":\"c\",\"files\":"
                 "[{\"path\":\"bin/tt7d\",\"sha256\":\"%s\",\"mode\":\"%s\"},{\"path\":\"app\",\"sha256\":\"%s\","
                 "\"mode\":\"0755\"}]}", h1, bad_modes[i], h2);
        EXPECT_CODE(rc, e, "invalid_manifest");
    }
    rc = FMT("{\"format\":\"tt7-bundle\",\"version\":\"0.1\",\"build\":\"b\",\"created\":\"c\",\"files\":"
             "[{\"path\":\"bin/tt7d\",\"sha256\":\"%s\",\"mode\":493},{\"path\":\"app\",\"sha256\":\"%s\","
             "\"mode\":\"0755\"}]}", h1, h2);
    EXPECT_CODE(rc, e, "invalid_manifest"); /* mode must be a string */
    /* Deep nesting is refused, not recursed into without bound. */
    char deep[600];
    size_t k = 0;
    deep[k++] = '{';
    memcpy(deep + k, "\"x\":", 4);
    k += 4;
    for (int i = 0; i < 200; i++) deep[k++] = '[';
    deep[k] = 0;
    EXPECT_CODE(manifest(deep, &e), e, "invalid_manifest");
#undef FMT
#undef FILES
}

/* ---- tar ---------------------------------------------------------------------------- */

static void test_tar_valid(void) {
    static struct tar t;
    good_bundle(&t);
    static struct bundle b;
    struct bundle_error e = {0};
    CHECK(bundle_verify(t.buf, t.len, NULL, &b, &e) == 0, "good bundle: %s %s %s", e.code, e.member, e.detail);
    CHECK(b.nfiles == 2 && b.files[0].len == strlen(TT7D_BIN) && !memcmp(b.files[0].data, TT7D_BIN, b.files[0].len),
          "bin/tt7d points into the tar");
    CHECK(b.files[1].len == strlen(APP_SH) && !memcmp(b.files[1].data, APP_SH, b.files[1].len), "app data");
    CHECK(b.payload_bytes == strlen(TT7D_BIN) + strlen(APP_SH), "payload bytes %zu", b.payload_bytes);
    CHECK(b.signed_ok == 0, "unsigned");

    /* GNU tar's old magic ("ustar  \0") and a single end block are fine; so is a
     * member name split into the prefix field. */
    char m[1024];
    good_manifest(m, sizeof m);
    t.len = 0;
    tar_member_ex(&t, NULL, "manifest.json", '0', m, strlen(m), "ustar  ");
    tar_member_ex(&t, "bin", "tt7d", '0', TT7D_BIN, strlen(TT7D_BIN), "ustar\0" "00");
    tar_member_ex(&t, NULL, "app", '\0', APP_SH, strlen(APP_SH), "ustar\0" "00");
    memset(t.buf + t.len, 0, 512);
    t.len += 512;
    CHECK(verify(&t, NULL, &e) == 0, "gnu magic, prefix, NUL type: %s %s %s", e.code, e.member, e.detail);
}

static void test_tar_member_types(void) {
    static struct tar t;
    struct bundle_error e;
    char m[1024];
    good_manifest(m, sizeof m);
    const char types[] = {'1', '2', '3', '4', '5', '6', '7', 'x', 'g', 'L', 'K'};
    for (size_t i = 0; i < sizeof types; i++) {
        t.len = 0;
        tar_file(&t, "manifest.json", m);
        tar_file(&t, "bin/tt7d", TT7D_BIN);
        tar_member(&t, "app", types[i], NULL, 0);
        tar_end(&t);
        int rc = verify(&t, NULL, &e);
        EXPECT_CODE(rc, e, "bad_member_type");
        CHECK(!strcmp(e.member, "app"), "names the member (type %c): %s", types[i], e.member);
    }
}

static void test_tar_paths(void) {
    static struct tar t;
    struct bundle_error e;
    char m[1024];
    good_manifest(m, sizeof m);
    const struct {
        const char *prefix, *name, *code;
    } cases[] = {
        {NULL, "../app", "unsafe_path"},     {NULL, "/app", "unsafe_path"},
        {NULL, "bin/../../x", "unsafe_path"}, {"..", "app", "unsafe_path"},
        {"/data", "app", "unsafe_path"},     {NULL, "etc/passwd", "unknown_file"},
        {NULL, "./app", "unknown_file"},     {NULL, "bin/tt7d.new", "unknown_file"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        t.len = 0;
        tar_file(&t, "manifest.json", m);
        tar_file(&t, "bin/tt7d", TT7D_BIN);
        tar_file(&t, "app", APP_SH);
        tar_member_ex(&t, cases[i].prefix, cases[i].name, '0', "x", 1, "ustar\0" "00");
        tar_end(&t);
        int rc = verify(&t, NULL, &e);
        EXPECT_CODE(rc, e, cases[i].code);
    }
    /* The same member twice. */
    t.len = 0;
    tar_file(&t, "manifest.json", m);
    tar_file(&t, "bin/tt7d", TT7D_BIN);
    tar_file(&t, "app", APP_SH);
    tar_file(&t, "app", APP_SH);
    tar_end(&t);
    EXPECT_CODE(verify(&t, NULL, &e), e, "duplicate_member");
}

static void test_tar_framing(void) {
    static struct tar t;
    struct bundle_error e;
    EXPECT_CODE(bundle_verify(t.buf, 0, NULL, &(struct bundle){0}, &e), e, "invalid_bundle");
    memset(t.buf, 0, 1024);
    EXPECT_CODE(bundle_verify(t.buf, 1024, NULL, &(struct bundle){0}, &e), e, "no_manifest"); /* an empty tar */
    memcpy(t.buf, "hello, not a tar", 16);
    EXPECT_CODE(bundle_verify(t.buf, 1024, NULL, &(struct bundle){0}, &e), e, "invalid_bundle");

    good_bundle(&t);
    t.buf[0] ^= 1; /* the first header no longer matches its checksum */
    EXPECT_CODE(verify(&t, NULL, &e), e, "invalid_bundle");

    good_bundle(&t);
    size_t cut = t.len - 1024 - 512 + 5; /* inside app's data */
    EXPECT_CODE(bundle_verify(t.buf, cut, NULL, &(struct bundle){0}, &e), e, "invalid_bundle");
    EXPECT_CODE(bundle_verify(t.buf, 700, NULL, &(struct bundle){0}, &e), e, "invalid_bundle"); /* not whole blocks */

    /* A base-256 size (GNU, for > 8 GiB) is not octal: refused. */
    good_bundle(&t);
    uint8_t *h = t.buf;
    h[124] = 0x80;
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    EXPECT_CODE(verify(&t, NULL, &e), e, "invalid_bundle");

    /* No manifest at all. */
    t.len = 0;
    tar_file(&t, "bin/tt7d", TT7D_BIN);
    tar_file(&t, "app", APP_SH);
    tar_end(&t);
    EXPECT_CODE(verify(&t, NULL, &e), e, "no_manifest");

    /* A manifest too big to be one. */
    static char big[BUNDLE_MANIFEST_MAX + 2];
    memset(big, ' ', sizeof big - 1);
    t.len = 0;
    tar_file(&t, "manifest.json", big);
    tar_end(&t);
    EXPECT_CODE(verify(&t, NULL, &e), e, "invalid_manifest");
}

static void test_tar_contents(void) {
    static struct tar t;
    struct bundle_error e;
    char m[1024];
    good_manifest(m, sizeof m);

    /* Listed but absent. */
    t.len = 0;
    tar_file(&t, "manifest.json", m);
    tar_file(&t, "bin/tt7d", TT7D_BIN);
    tar_end(&t);
    int rc = verify(&t, NULL, &e);
    EXPECT_CODE(rc, e, "missing_file");
    CHECK(!strcmp(e.member, "app"), "missing: %s", e.member);

    /* Present (and allowed) but not listed. */
    t.len = 0;
    tar_file(&t, "manifest.json", m);
    tar_file(&t, "bin/tt7d", TT7D_BIN);
    tar_file(&t, "app", APP_SH);
    tar_file(&t, "bin/tt7probe", "probe");
    tar_end(&t);
    rc = verify(&t, NULL, &e);
    EXPECT_CODE(rc, e, "unlisted_file");
    CHECK(!strcmp(e.member, "bin/tt7probe"), "unlisted: %s", e.member);

    /* One byte changed. */
    t.len = 0;
    tar_file(&t, "manifest.json", m);
    tar_file(&t, "bin/tt7d", "\x7f" "ELF pretend tt7D");
    tar_file(&t, "app", APP_SH);
    tar_end(&t);
    rc = verify(&t, NULL, &e);
    EXPECT_CODE(rc, e, "hash_mismatch");
    CHECK(!strcmp(e.member, "bin/tt7d") && !strcmp(e.expected, sha(TT7D_BIN)) &&
              !strcmp(e.computed, sha("\x7f" "ELF pretend tt7D")),
          "mismatch names file and both hashes: %s %s %s", e.member, e.expected, e.computed);
}

/* ---- signatures ------------------------------------------------------------------------ */

static void hex(const char *s, uint8_t *out, size_t n) {
    CHECK(sign_hex_decode(s, strlen(s), out, n) == 0, "hex decode %s", s);
}

static void test_rfc8032_vectors(void) {
    /* RFC 8032 section 7.1, TEST 1-3 (https://www.rfc-editor.org/rfc/rfc8032.txt). */
    static const struct {
        const char *pk, *msg, *sig;
    } v[] = {
        {"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint8_t pk[32], sig[64], msg[2];
        size_t n = strlen(v[i].msg) / 2;
        hex(v[i].pk, pk, 32);
        hex(v[i].sig, sig, 64);
        if (n) hex(v[i].msg, msg, n);
        CHECK(sign_verify(pk, sig, msg, n) == 1, "RFC 8032 test %zu verifies", i + 1);
        sig[10] ^= 0x01;
        CHECK(sign_verify(pk, sig, msg, n) == 0, "test %zu: a flipped signature bit fails", i + 1);
        sig[10] ^= 0x01;
        if (n) {
            msg[0] ^= 0x80;
            CHECK(sign_verify(pk, sig, msg, n) == 0, "test %zu: a changed message fails", i + 1);
            msg[0] ^= 0x80;
        }
        pk[0] ^= 0x02;
        CHECK(sign_verify(pk, sig, msg, n) == 0, "test %zu: another key fails", i + 1);
    }
    uint8_t out[4];
    CHECK(sign_hex_decode(" 0aFf\n", 6, out, 2) == 0 && out[0] == 0x0a && out[1] == 0xff, "hex with whitespace");
    CHECK(sign_hex_decode("0aff00", 6, out, 2) == -1, "too long");
    CHECK(sign_hex_decode("0af", 3, out, 2) == -1, "too short");
    CHECK(sign_hex_decode("0agf", 4, out, 2) == -1, "not hex");
}

/* RFC 8032 TEST 1's key pair signs a bundle's manifest here. */
static const char *SEED = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
static const char *PUB = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";

static void sig_hex_of(const char *msg, char out[129]) {
    uint8_t sk[64], sm[64 + 1024];
    hex(SEED, sk, 32);
    hex(PUB, sk + 32, 32);
    unsigned long long smlen;
    crypto_sign_ed25519_tweet(sm, &smlen, (const uint8_t *)msg, strlen(msg), sk);
    for (int i = 0; i < 64; i++) snprintf(out + 2 * i, 3, "%02x", sm[i]);
}

static void test_signed_bundles(void) {
    static struct tar t;
    static struct bundle b;
    struct bundle_error e;
    uint8_t pk[32];
    hex(PUB, pk, 32);
    char m[1024], sig[130];
    good_manifest(m, sizeof m);
    sig_hex_of(m, sig);
    strcat(sig, "\n");

    good_bundle(&t); /* unsigned */
    EXPECT_CODE(verify(&t, pk, &e), e, "signature_required");

    t.len = 0;
    tar_file(&t, "manifest.json", m);
    tar_file(&t, "manifest.sig", sig);
    tar_file(&t, "bin/tt7d", TT7D_BIN);
    tar_file(&t, "app", APP_SH);
    tar_end(&t);
    memset(&e, 0, sizeof e);
    CHECK(bundle_verify(t.buf, t.len, pk, &b, &e) == 0 && b.signed_ok == 1, "signed bundle: %s %s", e.code, e.detail);
    CHECK(bundle_verify(t.buf, t.len, NULL, &b, &e) == 0 && b.signed_ok == 0, "no key configured: signature unused");

    uint8_t other[32];
    memcpy(other, pk, 32);
    other[31] ^= 0x40;
    EXPECT_CODE(verify(&t, other, &e), e, "invalid_signature");

    /* The signature covers the manifest's exact bytes: add a space and it fails. */
    char m2[1030];
    snprintf(m2, sizeof m2, "%s ", m);
    t.len = 0;
    tar_file(&t, "manifest.json", m2);
    tar_file(&t, "manifest.sig", sig);
    tar_file(&t, "bin/tt7d", TT7D_BIN);
    tar_file(&t, "app", APP_SH);
    tar_end(&t);
    EXPECT_CODE(verify(&t, pk, &e), e, "invalid_signature");

    t.len = 0;
    tar_file(&t, "manifest.json", m);
    tar_file(&t, "manifest.sig", "not hex at all");
    tar_file(&t, "bin/tt7d", TT7D_BIN);
    tar_file(&t, "app", APP_SH);
    tar_end(&t);
    EXPECT_CODE(verify(&t, pk, &e), e, "invalid_signature");
}

/* ---- versions, ids, pruning ------------------------------------------------------------- */

static void test_versions_and_ids(void) {
    CHECK(bundle_version_cmp("0.1.0", "0.1.0") == 0, "equal");
    CHECK(bundle_version_cmp("0.1", "0.1.0") == 0, "missing parts are 0");
    CHECK(bundle_version_cmp("0.10.0", "0.9.9") > 0, "numeric, not text");
    CHECK(bundle_version_cmp("0.1.0", "1.0") < 0, "older");
    CHECK(bundle_version_cmp("0.1.x", "1.0") == BUNDLE_VERSION_BAD, "not a version");
    CHECK(bundle_version_cmp("", "1.0") == BUNDLE_VERSION_BAD && bundle_version_cmp("1..0", "1") == BUNDLE_VERSION_BAD,
          "empty parts");

    CHECK(bundle_id_valid("43e351a") && bundle_id_valid("v0.1.0-3-g43e351a-dirty") && bundle_id_valid("a+b_c"), "ok ids");
    CHECK(!bundle_id_valid("") && !bundle_id_valid(".x") && !bundle_id_valid("-x") && !bundle_id_valid("a/b") &&
              !bundle_id_valid("..") && !bundle_id_valid("a b"),
          "bad ids");
    char longid[BUNDLE_ID_MAX + 2];
    memset(longid, 'a', sizeof longid - 1);
    longid[sizeof longid - 1] = 0;
    CHECK(!bundle_id_valid(longid), "too long");
    longid[BUNDLE_ID_MAX] = 0;
    CHECK(bundle_id_valid(longid), "exactly the limit");
}

static int has(const int *out, int n, int idx) {
    for (int i = 0; i < n; i++)
        if (out[i] == idx) return 1;
    return 0;
}

static void test_prune(void) {
    int out[8];
    const struct release_entry list[] = {{"a", 100}, {"b", 200}, {"c", 300}, {"d", 400}, {"e", 500}};
    int n = bundle_prune_plan(list, 5, "e", "d", 3, out);
    CHECK(n == 2 && has(out, n, 0) && has(out, n, 1), "oldest two go: n=%d", n);

    /* current and previous are kept even when they are the oldest. */
    n = bundle_prune_plan(list, 5, "a", "b", 3, out);
    CHECK(n == 2 && has(out, n, 2) && has(out, n, 3), "keeps current a, previous b, newest other e: n=%d", n);

    n = bundle_prune_plan(list, 3, "c", NULL, 3, out);
    CHECK(n == 0, "nothing to do at the limit");
    n = bundle_prune_plan(list, 5, NULL, NULL, 3, out);
    CHECK(n == 2 && has(out, n, 0) && has(out, n, 1), "no pointers: oldest go");

    /* keep smaller than the protected set: never delete current or previous. */
    n = bundle_prune_plan(list, 5, "a", "b", 1, out);
    CHECK(n == 3 && !has(out, n, 0) && !has(out, n, 1), "protected survive: n=%d", n);

    /* Equal mtimes: by name, so the plan is stable. */
    const struct release_entry same[] = {{"z", 1}, {"y", 1}, {"x", 1}, {"w", 1}};
    n = bundle_prune_plan(same, 4, "w", NULL, 3, out);
    CHECK(n == 1 && out[0] == 2, "tie broken by name (x before y, z): %d", out[0]);
}

int main(void) {
    test_manifest_valid();
    test_manifest_invalid();
    test_tar_valid();
    test_tar_member_types();
    test_tar_paths();
    test_tar_framing();
    test_tar_contents();
    test_rfc8032_vectors();
    test_signed_bundles();
    test_versions_and_ids();
    test_prune();
    return test_finish("test_bundle");
}
