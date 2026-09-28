/* ABOUTME: The bearer token and device id files in tt7d's data dir, atomic file writes,
 * ABOUTME: constant-time token comparison, and the X-Frame-ID syntax rule. */
#include "ident.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int token_equal(const char *expected, const char *given) {
    if (!given) given = "";
    size_t el = strlen(expected), gl = strlen(given);
    unsigned diff = (el != gl) | (el == 0);
    /* Always walk all of `expected`, so timing never shows how much matched. */
    for (size_t i = 0; i < el; i++) {
        unsigned char g = i < gl ? (unsigned char)given[i] : 0;
        diff |= (unsigned char)expected[i] ^ g;
    }
    return diff == 0;
}

int random_hex(char *hex, size_t nbytes) {
    unsigned char raw[64];
    if (nbytes > sizeof raw) return -1;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < nbytes) {
        ssize_t r = read(fd, raw + got, nbytes - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            close(fd);
            return -1;
        }
        got += (size_t)r;
    }
    close(fd);
    for (size_t i = 0; i < nbytes; i++) snprintf(hex + 2 * i, 3, "%02x", raw[i]);
    hex[2 * nbytes] = 0;
    return 0;
}

int frame_id_valid(const char *id) {
    size_t n = strlen(id);
    if (n < 1 || n > 128) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)id[i] <= 0x20 || (unsigned char)id[i] >= 0x7f) return 0;
    return 1;
}

int write_file_atomic(const char *path, const void *data, size_t len, int mode) {
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    const char *p = data;
    size_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) goto fail;
        p += w;
        left -= (size_t)w;
    }
    if (fchmod(fd, (mode_t)mode) != 0 || fsync(fd) != 0) goto fail;
    if (close(fd) != 0) {
        fd = -1;
        goto fail;
    }
    fd = -1;
    if (rename(tmp, path) != 0) goto fail;

    /* Make the rename itself durable. */
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else snprintf(dir, sizeof dir, ".");
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return 0;

fail: {
    int saved = errno;
    if (fd >= 0) close(fd);
    unlink(tmp);
    errno = saved;
    return -1;
}
}

/* Read a small file into buf (NUL-terminated). Returns bytes read or -1. */
static ssize_t read_small(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}

int token_load(const char *dir, char *token, size_t size, char *err, size_t errlen) {
    char path[512];
    snprintf(path, sizeof path, "%s/token", dir);
    if (read_small(path, token, size) < 0) {
        if (errno != ENOENT) {
            snprintf(err, errlen, "%s: %s", path, strerror(errno));
            return -1;
        }
        if (size < 65 || random_hex(token, 32) != 0) {
            snprintf(err, errlen, "cannot generate a token from /dev/urandom");
            return -1;
        }
        char line[66];
        snprintf(line, sizeof line, "%s\n", token);
        if (write_file_atomic(path, line, strlen(line), 0600) != 0) {
            snprintf(err, errlen, "%s: %s", path, strerror(errno));
            return -1;
        }
        return 0;
    }
    size_t n = strlen(token);
    while (n > 0 && (token[n - 1] == '\n' || token[n - 1] == '\r' || token[n - 1] == ' ')) token[--n] = 0;
    if (n == 0) {
        snprintf(err, errlen, "%s is empty; delete it to get a new token", path);
        return -1;
    }
    return 0;
}

/* The id format: "tt7-" and six lowercase hex digits. */
static int device_id_valid(const char *id) {
    return strlen(id) == 10 && strncmp(id, "tt7-", 4) == 0 && strspn(id + 4, "0123456789abcdef") == 6;
}

int device_id_load(const char *dir, char *id, size_t size, char *err, size_t errlen) {
    char path[512], text[512];
    snprintf(path, sizeof path, "%s/device.json", dir);
    if (read_small(path, text, sizeof text) < 0) {
        if (errno != ENOENT) {
            snprintf(err, errlen, "%s: %s", path, strerror(errno));
            return -1;
        }
        char hex[7];
        if (size < 11 || random_hex(hex, 3) != 0) {
            snprintf(err, errlen, "cannot generate a device id from /dev/urandom");
            return -1;
        }
        snprintf(id, size, "tt7-%s", hex);
        char json[64];
        snprintf(json, sizeof json, "{\"device_id\": \"%s\"}\n", id);
        if (write_file_atomic(path, json, strlen(json), 0644) != 0) {
            snprintf(err, errlen, "%s: %s", path, strerror(errno));
            return -1;
        }
        return 0;
    }
    /* Find "device_id" : "<id>" without a JSON parser; the file is ours. */
    const char *p = strstr(text, "\"device_id\"");
    if (p) p = strchr(p + 11, ':');
    if (p) p += strspn(p + 1, " \t") + 1;
    if (p && *p == '"') {
        const char *end = strchr(p + 1, '"');
        size_t n = end ? (size_t)(end - p - 1) : 0;
        if (end && n < size) {
            memcpy(id, p + 1, n);
            id[n] = 0;
            if (device_id_valid(id)) return 0;
        }
    }
    id[0] = 0;
    snprintf(err, errlen, "%s has no valid \"device_id\" (want tt7- and 6 hex digits); not replacing it", path);
    return -1;
}
