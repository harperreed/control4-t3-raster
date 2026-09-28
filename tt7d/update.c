/* ABOUTME: Web update: checks an uploaded bundle in RAM, installs it into a new <root>/releases/<id>/ with
 * ABOUTME: fsync and rename, flips update/current, then exits 75 so tt7-app starts the release under trial. */
#include "update.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "bundle.h"
#include "panel.h"
#include "sign.h"

/* Layout under cfg.root (/data/tt7), shared with probe/tt7-app.sh:
 *   releases/<id>/{app,bin/tt7d,bin/tt7probe,bin/tt7-ntp-hook,manifest.json}
 *   update/current    "<id>": the release tt7-app runs; absent: the image's own build
 *   update/previous   "<id>": what current replaced; the rollback target
 *   update/trial      "<id> <starts>": not yet confirmed; tt7-app counts starts,
 *                     tt7d deletes it once healthy
 *   update/history    one line per install, confirm, rollback: "<time> <event> <id> <detail>" */

#define PATH_UPDATE "/api/v1/system/update"
#define PATH_ROLLBACK "/api/v1/system/update/rollback"
#define MEM_HEADROOM (16ull << 20) /* bytes of RAM left over after the bundle and its checks */
#define HISTORY_SHOWN 20
#define MAX_RELEASE_DIRS 64

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void iso_now(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void join(char *out, size_t n, const struct update *u, const char *a, const char *b) {
    snprintf(out, n, "%s/%s%s%s", u->cfg.root, a, b ? "/" : "", b ? b : "");
}

void update_init(struct update *u, const struct update_config *cfg) {
    memset(u, 0, sizeof *u);
    u->cfg = *cfg;
    if (u->cfg.release && !*u->cfg.release) u->cfg.release = NULL;
    u->started_ms = now_ms();
}

int update_owns(const char *path) { return !strcmp(path, PATH_UPDATE) || !strcmp(path, PATH_ROLLBACK); }

/* ---- files ------------------------------------------------------------------------------ */

static int fsync_dir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = fsync(fd);
    close(fd);
    return r;
}

static int write_file(const char *path, const void *data, size_t len, mode_t mode, int excl) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (excl ? O_EXCL : 0), 0600);
    if (fd < 0) return -1;
    const uint8_t *p = data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int e = errno;
            close(fd);
            errno = n < 0 ? e : EIO;
            return -1;
        }
        off += (size_t)n;
    }
    if (fchmod(fd, mode) != 0 || fsync(fd) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return close(fd);
}

/* Replace path's contents in one step: write path.tmp, fsync, rename. */
static int write_atomic(const char *path, const char *text) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (write_file(tmp, text, strlen(text), 0644, 0) != 0) return -1;
    return rename(tmp, path);
}

static int read_small(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

/* rm -rf for a release directory: never follows symlinks. */
static int remove_tree(const char *path, int depth) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    if (depth > 4) {
        errno = ELOOP;
        return -1;
    }
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[600];
        snprintf(child, sizeof child, "%s/%s", path, de->d_name);
        if (remove_tree(child, depth + 1) != 0) rc = -1;
    }
    closedir(d);
    return rc == 0 ? rmdir(path) : -1;
}

/* ---- the update/ files ------------------------------------------------------------------ */

/* update/<name>'s release id into id; 1 if there is a valid one. */
static int pointer_read(const struct update *u, const char *name, char id[BUNDLE_ID_MAX + 1]) {
    char path[512], buf[BUNDLE_ID_MAX + 8];
    join(path, sizeof path, u, "update", name);
    if (read_small(path, buf, sizeof buf) < 0) return 0;
    buf[strcspn(buf, "\r\n")] = 0;
    if (!bundle_id_valid(buf)) return 0;
    memcpy(id, buf, strlen(buf) + 1); /* valid ids are at most BUNDLE_ID_MAX */
    return 1;
}

/* Point update/<name> at id, or remove it for NULL. */
static int pointer_write(const struct update *u, const char *name, const char *id) {
    char path[512], text[BUNDLE_ID_MAX + 2];
    join(path, sizeof path, u, "update", name);
    if (!id) return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
    snprintf(text, sizeof text, "%s\n", id);
    return write_atomic(path, text);
}

static int trial_read(const struct update *u, char id[BUNDLE_ID_MAX + 1], unsigned *starts) {
    char path[512], buf[BUNDLE_ID_MAX + 32], rid[BUNDLE_ID_MAX + 2];
    join(path, sizeof path, u, "update", "trial");
    if (read_small(path, buf, sizeof buf) < 0) return 0;
    if (sscanf(buf, "%65s %u", rid, starts) != 2 || !bundle_id_valid(rid)) return 0;
    memcpy(id, rid, strlen(rid) + 1);
    return 1;
}

static int trial_start(const struct update *u, const char *id) {
    char path[512], text[BUNDLE_ID_MAX + 4];
    join(path, sizeof path, u, "update", "trial");
    snprintf(text, sizeof text, "%s 0\n", id);
    return write_atomic(path, text);
}

static void history_add(const struct update *u, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void history_add(const struct update *u, const char *fmt, ...) {
    char path[512], line[400], when[32];
    iso_now(when, sizeof when);
    int n = snprintf(line, sizeof line, "%s ", when);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    fprintf(stderr, "tt7d: update: %s", line + n);
    join(path, sizeof path, u, "update", "history");
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, line, strlen(line));
    (void)w;
    fsync(fd);
    close(fd);
}

static int release_usable(const struct update *u, const char *id) {
    char p[600];
    snprintf(p, sizeof p, "%s/releases/%s/bin/tt7d", u->cfg.root, id);
    if (access(p, X_OK) != 0) return 0;
    snprintf(p, sizeof p, "%s/releases/%s/app", u->cfg.root, id);
    return access(p, X_OK) == 0;
}

/* The configured signing key: 0 none, 1 loaded into pk, -1 present but unreadable. */
static int pubkey_load(const struct update *u, uint8_t pk[SIGN_PUBKEY_BYTES]) {
    char path[512], buf[256];
    snprintf(path, sizeof path, "%s/update-pubkey", u->cfg.data_dir);
    int n = read_small(path, buf, sizeof buf);
    if (n < 0) return errno == ENOENT ? 0 : -1;
    return sign_hex_decode(buf, (size_t)n, pk, SIGN_PUBKEY_BYTES) == 0 ? 1 : -1;
}

static int pubkey_configured(const struct update *u) {
    char path[512];
    snprintf(path, sizeof path, "%s/update-pubkey", u->cfg.data_dir);
    return access(path, F_OK) == 0;
}

/* ---- head checks ------------------------------------------------------------------------- */

static void method_not_allowed(struct response *resp, const char *allow) {
    resp_error(resp, 405, "method_not_allowed", "this method is not supported on this path");
    snprintf(resp->extra_headers, sizeof resp->extra_headers, "Allow: %s\r\n", allow);
}

static int restart_pending(const struct update *u, struct response *resp) {
    if (!u->restart_at_ms) return 0;
    resp_error(resp, 409, "restart_pending", "an update or rollback was just applied; tt7d restarts in a moment");
    return 1;
}

/* RAM that can be had, from /proc/meminfo: MemFree + Buffers + Cached (3.0
 * kernels have no MemAvailable). -1 if unknown. */
static long long ram_available(const struct update *u) {
    long f = meminfo_kib(u->cfg.proc_root, "MemFree"), b = meminfo_kib(u->cfg.proc_root, "Buffers"),
         c = meminfo_kib(u->cfg.proc_root, "Cached");
    if (f < 0 || b < 0 || c < 0) return -1;
    return ((long long)f + b + c) * 1024;
}

int update_check_head(struct update *u, const struct http_request *req, struct response *resp) {
    if (!strcmp(req->path, PATH_ROLLBACK)) {
        if (strcmp(req->method, "POST") != 0) {
            method_not_allowed(resp, "POST");
            return -1;
        }
        if (resp_require_bearer(u->cfg.token, req, resp) != 0) return -1;
        return restart_pending(u, resp) ? -1 : 0;
    }
    if (!strcmp(req->method, "GET")) return 0;
    if (strcmp(req->method, "PUT") != 0) {
        method_not_allowed(resp, "GET, PUT");
        return -1;
    }
    if (resp_require_bearer(u->cfg.token, req, resp) != 0) return -1;
    const char *ct = http_header(req, "Content-Type");
    if (!ct || strcspn(ct, "; \t") != 17 || strncasecmp(ct, "application/x-tar", 17) != 0) {
        resp_error_begin(resp, 415, "unsupported_media_type", "send the bundle as Content-Type: application/x-tar");
        sb_puts(&resp->body, ",\"supported\":[\"application/x-tar\"]");
        resp_error_end(resp);
        return -1;
    }
    const char *cl = http_header(req, "Content-Length"); /* the server already checked it is a number */
    unsigned long long len = cl ? strtoull(cl, NULL, 10) : 0;
    if (len > UPDATE_MAX_BYTES) {
        resp_error_begin(resp, 413, "payload_too_large", "a bundle may be at most 4 MiB");
        sb_printf(&resp->body, ",\"max_bytes\":%u", UPDATE_MAX_BYTES);
        resp_error_end(resp);
        return -1;
    }
    if (restart_pending(u, resp)) return -1;
    /* The body is held in RAM while it is checked; the panel has no swap. */
    long long avail = ram_available(u), need = 3 * (long long)len + (long long)MEM_HEADROOM;
    if (avail >= 0 && avail < need) {
        resp_error_begin(resp, 503, "insufficient_memory", "not enough free RAM to receive and check this bundle");
        sb_printf(&resp->body, ",\"available_bytes\":%lld,\"needed_bytes\":%lld", avail, need);
        resp_error_end(resp);
        return -1;
    }
    return 0;
}

void update_on_reply(struct update *u, const struct http_request *req, const struct response *resp) {
    if (!update_owns(req->path) || resp->status < 400) return;
    snprintf(u->last_error, sizeof u->last_error, "%s", resp->error ? resp->error : "error");
    u->last_error_status = resp->status;
    iso_now(u->last_error_time, sizeof u->last_error_time);
}

/* ---- install ----------------------------------------------------------------------------- */

#define TRY(cond, what)                                                          \
    do {                                                                         \
        if (!(cond)) {                                                           \
            snprintf(err, errlen, "%s: %s", what, strerror(errno));              \
            goto fail;                                                           \
        }                                                                        \
    } while (0)

/* Files into releases/.incoming-<id>, fsynced, renamed to releases/<id>; then
 * trial, current and previous. Nothing outside cfg.root is touched. */
static int install(struct update *u, const struct bundle *b, const char *old_current, char *err, size_t errlen) {
    char releases[512], upd[512], inc[600], fin[600], p[700];
    join(releases, sizeof releases, u, "releases", NULL);
    join(upd, sizeof upd, u, "update", NULL);
    snprintf(inc, sizeof inc, "%s/.incoming-%s", releases, b->build);
    snprintf(fin, sizeof fin, "%s/%s", releases, b->build);

    TRY(mkdir(releases, 0755) == 0 || errno == EEXIST, "mkdir releases");
    TRY(mkdir(upd, 0755) == 0 || errno == EEXIST, "mkdir update");
    TRY(remove_tree(inc, 0) == 0, "clear a staging dir left over");
    TRY(mkdir(inc, 0755) == 0, "mkdir the staging dir");
    snprintf(p, sizeof p, "%s/bin", inc);
    TRY(mkdir(p, 0755) == 0, "mkdir bin");
    for (int i = 0; i < b->nfiles; i++) {
        const struct bundle_file *f = &b->files[i];
        snprintf(p, sizeof p, "%s/%s", inc, f->path);
        TRY(write_file(p, f->data, f->len, f->mode, 1) == 0, f->path);
    }
    snprintf(p, sizeof p, "%s/manifest.json", inc);
    TRY(write_file(p, b->manifest, b->manifest_len, 0644, 1) == 0, "manifest.json");
    snprintf(p, sizeof p, "%s/bin", inc);
    TRY(fsync_dir(p) == 0 && fsync_dir(inc) == 0, "fsync the release");
    TRY(remove_tree(fin, 0) == 0, "remove an older copy of this release");
    TRY(rename(inc, fin) == 0, "rename into place");
    TRY(fsync_dir(releases) == 0, "fsync releases");

    /* The trial first: a crash before current is written leaves a trial for
     * a release that is not current, which tt7-app drops. */
    TRY(trial_start(u, b->build) == 0, "write update/trial");
    TRY(pointer_write(u, "current", b->build) == 0, "write update/current");
    TRY(pointer_write(u, "previous", old_current) == 0, "write update/previous");
    TRY(fsync_dir(upd) == 0, "fsync update");
    return 0;
fail:
    remove_tree(inc, 0);
    return -1;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The release directories' names, sorted; returns how many (at most max). */
static int list_releases(const struct update *u, char **names, time_t *mtimes, int max) {
    char releases[512];
    join(releases, sizeof releases, u, "releases", NULL);
    DIR *d = opendir(releases);
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.' || !bundle_id_valid(de->d_name)) continue;
        char p[800]; /* a 511-byte root path plus a 255-byte name */
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", releases, de->d_name);
        if (lstat(p, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (!(names[n] = strdup(de->d_name))) break;
        if (mtimes) mtimes[n] = st.st_mtime;
        n++;
    }
    closedir(d);
    if (!mtimes) qsort(names, (size_t)n, sizeof *names, by_name);
    return n;
}

/* Keep at most UPDATE_KEEP_RELEASES release dirs, never current or previous;
 * also clear staging dirs a crash left behind. */
static void prune(struct update *u, const char *current, const char *previous) {
    char *names[MAX_RELEASE_DIRS];
    time_t mtimes[MAX_RELEASE_DIRS];
    struct release_entry list[MAX_RELEASE_DIRS];
    int out[MAX_RELEASE_DIRS];
    int n = list_releases(u, names, mtimes, MAX_RELEASE_DIRS);
    for (int i = 0; i < n; i++) list[i] = (struct release_entry){names[i], mtimes[i]};
    int k = bundle_prune_plan(list, n, current, previous, UPDATE_KEEP_RELEASES, out);
    for (int i = 0; i < k; i++) {
        char p[600];
        snprintf(p, sizeof p, "%s/releases/%s", u->cfg.root, names[out[i]]);
        if (remove_tree(p, 0) == 0) history_add(u, "pruned %s", names[out[i]]);
        else fprintf(stderr, "tt7d: update: could not remove %s: %s\n", p, strerror(errno));
    }
    for (int i = 0; i < n; i++) free(names[i]);

    char releases[512];
    join(releases, sizeof releases, u, "releases", NULL);
    DIR *d = opendir(releases);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (strncmp(de->d_name, ".incoming-", 10) != 0) continue;
        char p[800];
        snprintf(p, sizeof p, "%s/%s", releases, de->d_name);
        remove_tree(p, 0);
    }
    if (d) closedir(d);
}

/* ---- handlers ---------------------------------------------------------------------------- */

static void bundle_refused(struct response *resp, const struct bundle_error *e) {
    int forbidden = !strcmp(e->code, "signature_required") || !strcmp(e->code, "invalid_signature");
    resp_error_begin(resp, forbidden ? 403 : 400, e->code, e->detail);
    if (e->member[0]) {
        sb_puts(&resp->body, ",\"member\":");
        sb_json_str(&resp->body, e->member);
    }
    if (!strcmp(e->code, "hash_mismatch"))
        sb_printf(&resp->body, ",\"expected\":\"%s\",\"computed\":\"%s\"", e->expected, e->computed);
    resp_error_end(resp);
}

static void put_update(struct update *u, const uint8_t *body, size_t len, struct response *resp) {
    int64_t t0 = now_ms();
    uint8_t pk[SIGN_PUBKEY_BYTES];
    int key = pubkey_load(u, pk);
    if (key < 0) {
        resp_error(resp, 500, "invalid_pubkey",
                   "update-pubkey in the data dir is not 64 hex digits; no bundle is accepted until it is fixed");
        return;
    }
    static struct bundle b; /* file pointers go into body; static keeps it off the stack */
    struct bundle_error e = {0};
    if (bundle_verify(body, len, key ? pk : NULL, &b, &e) != 0) {
        bundle_refused(resp, &e);
        return;
    }
    char cur[BUNDLE_ID_MAX + 1];
    int have_cur = pointer_read(u, "current", cur);
    if (have_cur && !strcmp(cur, b.build)) {
        resp_error_begin(resp, 409, "already_current", "this build is already the current release");
        sb_puts(&resp->body, ",\"release\":");
        sb_json_str(&resp->body, b.build);
        resp_error_end(resp);
        return;
    }
    int cmp = bundle_version_cmp(b.version, u->cfg.firmware_version);
    if (u->cfg.refuse_downgrade && cmp != BUNDLE_VERSION_BAD && cmp < 0) {
        resp_error_begin(resp, 409, "downgrade_refused", "this bundle is older than the running tt7d");
        sb_puts(&resp->body, ",\"version\":");
        sb_json_str(&resp->body, b.version);
        sb_puts(&resp->body, ",\"running_version\":");
        sb_json_str(&resp->body, u->cfg.firmware_version);
        resp_error_end(resp);
        return;
    }
    struct statvfs vfs;
    unsigned long long need = b.payload_bytes + b.manifest_len + u->cfg.min_free_bytes;
    if (statvfs(u->cfg.root, &vfs) != 0) {
        resp_error(resp, 500, "install_failed", strerror(errno));
        return;
    }
    unsigned long long avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
    if (avail < need) {
        resp_error_begin(resp, 507, "insufficient_storage", "not enough free space on /data for this release");
        sb_printf(&resp->body, ",\"free_bytes\":%llu,\"needed_bytes\":%llu", avail, need);
        resp_error_end(resp);
        return;
    }
    char err[200];
    if (install(u, &b, have_cur ? cur : NULL, err, sizeof err) != 0) {
        fprintf(stderr, "tt7d: update: install of %s failed: %s\n", b.build, err);
        resp_error(resp, 500, "install_failed", err);
        return;
    }
    prune(u, b.build, have_cur ? cur : NULL);
    long long ms = (long long)(now_ms() - t0);
    history_add(u, "installed %s version=%s previous=%s bytes=%zu signed=%s ms=%lld", b.build, b.version,
                have_cur ? cur : "image", len, b.signed_ok ? "yes" : "no", ms);
    u->restart_at_ms = now_ms() + UPDATE_RESTART_DELAY_MS;

    resp->status = 202;
    sb_puts(&resp->body, "{\"release\":");
    sb_json_str(&resp->body, b.build);
    sb_puts(&resp->body, ",\"version\":");
    sb_json_str(&resp->body, b.version);
    sb_puts(&resp->body, ",\"previous\":");
    sb_json_str(&resp->body, have_cur ? cur : NULL);
    sb_printf(&resp->body,
              ",\"signed\":%s,\"install_ms\":%lld,\"restarting\":true,\"delay\":{\"value\":%d,\"unit\":\"second\"}}",
              b.signed_ok ? "true" : "false", ms, UPDATE_RESTART_DELAY_MS / 1000);
}

static void post_rollback(struct update *u, struct response *resp) {
    char cur[BUNDLE_ID_MAX + 1], prev[BUNDLE_ID_MAX + 1], upd[512];
    int have_cur = pointer_read(u, "current", cur), have_prev = pointer_read(u, "previous", prev);
    if (!have_prev || !release_usable(u, prev) || (have_cur && !strcmp(cur, prev))) {
        resp_error(resp, 409, "no_previous", "there is no previous release to roll back to");
        return;
    }
    join(upd, sizeof upd, u, "update", NULL);
    if (trial_start(u, prev) != 0 || pointer_write(u, "current", prev) != 0 ||
        pointer_write(u, "previous", have_cur ? cur : NULL) != 0 || fsync_dir(upd) != 0) {
        resp_error(resp, 500, "rollback_failed", strerror(errno));
        return;
    }
    history_add(u, "rollback_requested %s to=%s", have_cur ? cur : "image", prev);
    u->restart_at_ms = now_ms() + UPDATE_RESTART_DELAY_MS;
    resp->status = 202;
    sb_puts(&resp->body, "{\"release\":");
    sb_json_str(&resp->body, prev);
    sb_puts(&resp->body, ",\"previous\":");
    sb_json_str(&resp->body, have_cur ? cur : NULL);
    sb_printf(&resp->body, ",\"restarting\":true,\"delay\":{\"value\":%d,\"unit\":\"second\"}}",
              UPDATE_RESTART_DELAY_MS / 1000);
}

/* {"release", "version", "build", "created", "usable"} from the release's own manifest.json. */
static void release_json(const struct update *u, struct sbuf *sb, const char *id) {
    char path[600], buf[BUNDLE_MANIFEST_MAX + 1];
    static struct bundle b;
    struct bundle_error e;
    snprintf(path, sizeof path, "%s/releases/%s/manifest.json", u->cfg.root, id);
    int n = read_small(path, buf, sizeof buf);
    int ok = n > 0 && bundle_manifest_parse(buf, (size_t)n, &b, &e) == 0;
    sb_puts(sb, "{\"release\":");
    sb_json_str(sb, id);
    sb_puts(sb, ",\"version\":");
    sb_json_str(sb, ok ? b.version : NULL);
    sb_puts(sb, ",\"build\":");
    sb_json_str(sb, ok ? b.build : NULL);
    sb_puts(sb, ",\"created\":");
    sb_json_str(sb, ok ? b.created : NULL);
    sb_printf(sb, ",\"usable\":%s}", release_usable(u, id) ? "true" : "false");
}

/* One history line as {"time", "event", "release", "detail"}. */
static void history_entry_json(struct sbuf *sb, char *line) {
    char *f[3] = {NULL, NULL, NULL};
    char *p = line;
    for (int i = 0; i < 3 && p; i++) {
        f[i] = p;
        p = strchr(p, ' ');
        if (p) *p++ = 0;
    }
    sb_puts(sb, "{\"time\":");
    sb_json_str(sb, f[0]);
    sb_puts(sb, ",\"event\":");
    sb_json_str(sb, f[1]);
    sb_puts(sb, ",\"release\":");
    sb_json_str(sb, f[2]);
    sb_puts(sb, ",\"detail\":");
    sb_json_str(sb, p ? p : "");
    sb_puts(sb, "}");
}

/* "history": the last HISTORY_SHOWN lines, oldest first; "last_result": the newest. */
static void history_json(const struct update *u, struct sbuf *sb) {
    char path[512];
    static char buf[16384];
    join(path, sizeof path, u, "update", "history");
    size_t len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        off_t end = lseek(fd, 0, SEEK_END);
        off_t start = end > (off_t)sizeof buf - 1 ? end - (off_t)sizeof buf + 1 : 0;
        ssize_t n = pread(fd, buf, sizeof buf - 1, start);
        close(fd);
        if (n > 0) len = (size_t)n;
    }
    buf[len] = 0;
    char *lines[HISTORY_SHOWN];
    int nl = 0;
    for (char *s = buf; *s;) {
        char *e = strchr(s, '\n');
        if (!e) break; /* a partial line (the tail cut it, or a write in progress) */
        *e = 0;
        if (*s) {
            if (nl == HISTORY_SHOWN) {
                memmove(lines, lines + 1, sizeof lines[0] * (HISTORY_SHOWN - 1));
                nl--;
            }
            lines[nl++] = s;
        }
        s = e + 1;
    }
    sb_puts(sb, "\"history\":[");
    for (int i = 0; i < nl; i++) {
        if (i) sb_puts(sb, ",");
        char copy[512];
        snprintf(copy, sizeof copy, "%s", lines[i]);
        history_entry_json(sb, copy);
    }
    sb_puts(sb, "],\"last_result\":");
    if (nl) history_entry_json(sb, lines[nl - 1]);
    else sb_puts(sb, "null");
}

static void get_status(struct update *u, struct sbuf *sb) {
    char cur[BUNDLE_ID_MAX + 1], prev[BUNDLE_ID_MAX + 1], trial[BUNDLE_ID_MAX + 1];
    unsigned starts = 0;
    sb_puts(sb, "{\"running\":{\"release\":");
    sb_json_str(sb, u->cfg.release);
    sb_puts(sb, ",\"version\":");
    sb_json_str(sb, u->cfg.firmware_version);
    sb_puts(sb, ",\"build\":");
    sb_json_str(sb, u->cfg.build);
    sb_puts(sb, "},\"current\":");
    if (pointer_read(u, "current", cur)) release_json(u, sb, cur);
    else sb_puts(sb, "null");
    sb_puts(sb, ",\"previous\":");
    if (pointer_read(u, "previous", prev)) release_json(u, sb, prev);
    else sb_puts(sb, "null");
    sb_puts(sb, ",\"trial\":");
    if (trial_read(u, trial, &starts)) {
        sb_puts(sb, "{\"release\":");
        sb_json_str(sb, trial);
        sb_printf(sb, ",\"starts\":%u}", starts);
    } else {
        sb_puts(sb, "null");
    }
    char *names[MAX_RELEASE_DIRS];
    int n = list_releases(u, names, NULL, MAX_RELEASE_DIRS);
    sb_puts(sb, ",\"releases\":[");
    for (int i = 0; i < n; i++) {
        if (i) sb_puts(sb, ",");
        sb_json_str(sb, names[i]);
        free(names[i]);
    }
    sb_printf(sb, "],\"restart_pending\":%s,", u->restart_at_ms ? "true" : "false");
    history_json(u, sb);
    sb_puts(sb, ",\"last_error\":");
    if (u->last_error[0]) {
        sb_puts(sb, "{\"time\":");
        sb_json_str(sb, u->last_error_time);
        sb_puts(sb, ",\"error\":");
        sb_json_str(sb, u->last_error);
        sb_printf(sb, ",\"status\":%d}", u->last_error_status);
    } else {
        sb_puts(sb, "null");
    }
    sb_printf(sb,
              ",\"policy\":{\"max_bytes\":%u,\"signature_required\":%s,\"refuse_downgrade\":%s,"
              "\"keep_releases\":%d,\"min_free_bytes\":%llu,\"confirm_after_s\":%u}}",
              UPDATE_MAX_BYTES, pubkey_configured(u) ? "true" : "false", u->cfg.refuse_downgrade ? "true" : "false",
              UPDATE_KEEP_RELEASES, u->cfg.min_free_bytes, u->cfg.confirm_after_s);
}

void update_handle(struct update *u, const struct http_request *req, const uint8_t *body, size_t len,
                   struct response *resp) {
    resp->status = 200;
    if (!strcmp(req->path, PATH_ROLLBACK)) post_rollback(u, resp);
    else if (!strcmp(req->method, "PUT")) put_update(u, body, len, resp);
    else get_status(u, &resp->body);
}

/* ---- poll loop ----------------------------------------------------------------------------- */

void update_prepare(struct update *u, int64_t *wait_ms) {
    int64_t now = now_ms(), due = -1;
    if (!u->confirm_checked) due = u->started_ms + (int64_t)u->cfg.confirm_after_s * 1000;
    if (u->restart_at_ms && (due < 0 || u->restart_at_ms < due)) due = u->restart_at_ms;
    if (due < 0) return;
    int64_t left = due > now ? due - now : 0;
    if (*wait_ms < 0 || left < *wait_ms) *wait_ms = left;
}

/* Healthy = this tt7d has run its poll loop (the one that answers every
 * request, /api/v1/info included) for confirm_after_s since it started
 * listening. Then it confirms its own release if that is under trial. */
int update_service(struct update *u) {
    int64_t now = now_ms();
    if (!u->confirm_checked && now - u->started_ms >= (int64_t)u->cfg.confirm_after_s * 1000) {
        u->confirm_checked = 1;
        char id[BUNDLE_ID_MAX + 1], path[512], upd[512];
        unsigned starts;
        if (u->cfg.release && trial_read(u, id, &starts) && !strcmp(id, u->cfg.release)) {
            join(path, sizeof path, u, "update", "trial");
            join(upd, sizeof upd, u, "update", NULL);
            if (unlink(path) == 0 && fsync_dir(upd) == 0)
                history_add(u, "confirmed %s after=%us starts=%u", id, u->cfg.confirm_after_s, starts);
            else
                fprintf(stderr, "tt7d: update: could not confirm %s: %s\n", id, strerror(errno));
        }
    }
    return u->restart_at_ms && now >= u->restart_at_ms;
}

void update_state_member(const struct update *u, struct sbuf *sb) {
    sb_puts(sb, "\"update\":{\"release\":");
    sb_json_str(sb, u->cfg.release);
    sb_printf(sb, ",\"restart_pending\":%s}", u->restart_at_ms ? "true" : "false");
}
