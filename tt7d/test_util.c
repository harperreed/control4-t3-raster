/* ABOUTME: Host unit tests for sha256.c and ident.c: FIPS vectors, constant-time token compare,
 * ABOUTME: frame-id rules, and token/device-id files in a real temporary directory. */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ident.h"
#include "sha256.h"
#include "test_common.h"

static void expect_sha(const char *msg, size_t len, const char *want) {
    char hex[65];
    sha256_hex((const uint8_t *)msg, len, hex);
    CHECK(strcmp(hex, want) == 0, "sha256 of %zu bytes: %s", len, hex);
}

static void test_sha256(void) {
    /* FIPS 180-2 appendix B vectors, plus the empty string and a
     * 64-byte block boundary (both paddings). */
    expect_sha("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect_sha("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    expect_sha(two, strlen(two), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    char *million = malloc(1000000);
    memset(million, 'a', 1000000);
    expect_sha(million, 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    free(million);
    char sixty4[64];
    memset(sixty4, 'a', 64);
    expect_sha(sixty4, 64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    expect_sha(sixty4, 55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    expect_sha(sixty4, 56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
}

static void test_token_equal(void) {
    CHECK(token_equal("secret", "secret"), "equal");
    CHECK(!token_equal("secret", "secreT"), "last char differs");
    CHECK(!token_equal("secret", "Secret"), "first char differs");
    CHECK(!token_equal("secret", "secre"), "prefix");
    CHECK(!token_equal("secret", "secrets"), "longer");
    CHECK(!token_equal("secret", ""), "empty given");
    CHECK(!token_equal("secret", NULL), "NULL given");
    CHECK(!token_equal("", ""), "an empty expected token never matches");
}

static void test_frame_id(void) {
    CHECK(frame_id_valid("01K8R8CY1EP54"), "ulid");
    CHECK(frame_id_valid("a"), "one char");
    CHECK(frame_id_valid("dash-under_dot.colon:~!"), "punctuation");
    CHECK(!frame_id_valid(""), "empty");
    CHECK(!frame_id_valid("has space"), "space");
    CHECK(!frame_id_valid("tab\t"), "tab");
    CHECK(!frame_id_valid("caf\xc3\xa9"), "non-ASCII");
    char id[130];
    memset(id, 'x', 128);
    id[128] = 0;
    CHECK(frame_id_valid(id), "128 chars");
    id[128] = 'x';
    id[129] = 0;
    CHECK(!frame_id_valid(id), "129 chars");
}

static void read_file(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    size_t r = f ? fread(buf, 1, n - 1, f) : 0;
    buf[r] = 0;
    if (f) fclose(f);
}

static void test_files(void) {
    char dir[] = "/tmp/tt7d-test-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    char path[256], err[256], tok[128], tok2[128], id[32], id2[32], text[256];

    CHECK(token_load(dir, tok, sizeof tok, err, sizeof err) == 0, "token create: %s", err);
    CHECK(strlen(tok) == 64 && strspn(tok, "0123456789abcdef") == 64, "token '%s'", tok);
    snprintf(path, sizeof path, "%s/token", dir);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "token mode %o", st.st_mode & 0777);
    CHECK(token_load(dir, tok2, sizeof tok2, err, sizeof err) == 0 && strcmp(tok, tok2) == 0, "token reloads");

    /* A hand-written token with a trailing newline is accepted as is. */
    FILE *f = fopen(path, "w");
    fputs("hand-made-token\n", f);
    fclose(f);
    CHECK(token_load(dir, tok2, sizeof tok2, err, sizeof err) == 0 && strcmp(tok2, "hand-made-token") == 0,
          "hand token '%s'", tok2);
    f = fopen(path, "w");
    fclose(f);
    CHECK(token_load(dir, tok2, sizeof tok2, err, sizeof err) != 0, "an empty token file is an error");

    CHECK(device_id_load(dir, id, sizeof id, err, sizeof err) == 0, "id create: %s", err);
    CHECK(strlen(id) == 10 && strncmp(id, "tt7-", 4) == 0 && strspn(id + 4, "0123456789abcdef") == 6, "id '%s'", id);
    CHECK(device_id_load(dir, id2, sizeof id2, err, sizeof err) == 0 && strcmp(id, id2) == 0, "id persists");
    snprintf(path, sizeof path, "%s/device.json", dir);
    read_file(path, text, sizeof text);
    CHECK(strstr(text, id) != NULL && text[0] == '{', "device.json: %s", text);

    /* Corrupt file: never silently replaced by a new identity. */
    f = fopen(path, "w");
    fputs("{\"device_id\": 12}\n", f);
    fclose(f);
    CHECK(device_id_load(dir, id2, sizeof id2, err, sizeof err) != 0, "corrupt id is an error");
    read_file(path, text, sizeof text);
    CHECK(strcmp(text, "{\"device_id\": 12}\n") == 0, "corrupt file left alone: %s", text);

    snprintf(path, sizeof path, "%s/atomic.bin", dir);
    CHECK(write_file_atomic(path, "abc", 3, 0644) == 0, "atomic write");
    read_file(path, text, sizeof text);
    CHECK(strcmp(text, "abc") == 0, "atomic content %s", text);
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    CHECK(access(tmp, F_OK) != 0, "no temp file left behind");

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0, "cleanup");
}

static void test_random_hex(void) {
    char a[33], b[33];
    CHECK(random_hex(a, 16) == 0 && random_hex(b, 16) == 0, "random_hex");
    CHECK(strlen(a) == 32 && strspn(a, "0123456789abcdef") == 32, "hex '%s'", a);
    CHECK(strcmp(a, b) != 0, "two draws differ");
}

int main(void) {
    test_sha256();
    test_token_equal();
    test_frame_id();
    test_files();
    test_random_hex();
    return test_finish("test_util");
}
