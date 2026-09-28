/* ABOUTME: PUT /api/v1/frame: hash, dedup, PNG validation and decode, optional persistence, then one copy to the screen.
 * ABOUTME: Also restores last-frame.png at startup and renders the frame metadata JSON. */
#include "frame.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ident.h"
#include "lodepng.h"
#include "sha256.h"

void frame_store_init(struct frame_store *fs, const char *data_dir, struct display *disp) {
    memset(fs, 0, sizeof *fs);
    fs->data_dir = data_dir;
    fs->disp = disp;
}

static int is_hex64(const char *s) { return strlen(s) == 64 && strspn(s, "0123456789abcdefABCDEF") == 64; }

/* X-Persist: absent -> 0, "true" -> 1, "false" -> 0, anything else -> -1. */
static int persist_flag(const struct http_request *req) {
    const char *v = http_header(req, "X-Persist");
    if (!v || strcasecmp(v, "false") == 0) return 0;
    return strcasecmp(v, "true") == 0 ? 1 : -1;
}

int frame_check_head(const char *token, const struct http_request *req, struct response *resp) {
    const char *auth = http_header(req, "Authorization");
    const char *given = NULL;
    if (auth && strncasecmp(auth, "Bearer ", 7) == 0) given = auth + 7 + strspn(auth + 7, " ");
    if (!token_equal(token, given)) {
        resp_error(resp, 401, "unauthorized", "PUT /api/v1/frame needs Authorization: Bearer <token>");
        snprintf(resp->extra_headers, sizeof resp->extra_headers, "WWW-Authenticate: Bearer realm=\"tt7d\"\r\n");
        return -1;
    }
    const char *ct = http_header(req, "Content-Type");
    size_t n = ct ? strcspn(ct, "; \t") : 0;
    if (!ct || n != 9 || strncasecmp(ct, "image/png", 9) != 0) {
        resp_error_begin(resp, 415, "unsupported_media_type", "frames must be sent as Content-Type: image/png");
        sb_puts(&resp->body, ",\"supported\":[\"image/png\"]");
        resp_error_end(resp);
        return -1;
    }
    const char *id = http_header(req, "X-Frame-ID");
    if (id && !frame_id_valid(id)) {
        resp_error(resp, 400, "invalid_frame_id", "X-Frame-ID must be 1 to 128 printable ASCII characters, no spaces");
        return -1;
    }
    const char *sha = http_header(req, "X-Frame-SHA256");
    if (sha && !is_hex64(sha)) {
        resp_error(resp, 400, "invalid_sha256", "X-Frame-SHA256 must be 64 hex digits");
        return -1;
    }
    if (persist_flag(req) < 0) {
        resp_error(resp, 400, "invalid_persist", "X-Persist must be true or false");
        return -1;
    }
    return 0;
}

static void path_in(const struct frame_store *fs, const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/%s", fs->data_dir, name);
}

/* last-frame.png, then last-frame.id ("<sha256> <frame id>"). The id file is
 * written second, so a crash in between leaves an id that no longer matches
 * the PNG's hash, and restore reports the id as unknown. */
static int persist_write(const struct frame_store *fs, const uint8_t *png, size_t len, const char *sha,
                         const char *id) {
    char path[512], line[200];
    path_in(fs, "last-frame.png", path, sizeof path);
    if (write_file_atomic(path, png, len, 0644) != 0) return -1;
    path_in(fs, "last-frame.id", path, sizeof path);
    snprintf(line, sizeof line, "%s %s\n", sha, id);
    return write_file_atomic(path, line, strlen(line), 0644);
}

/* Check the PNG header and decode it to RGBA at the logical size. Returns
 * the pixels, or NULL with resp filled (422). */
static uint8_t *decode_png(const struct frame_store *fs, const uint8_t *png, size_t len, struct response *resp) {
    unsigned w = 0, h = 0;
    LodePNGState st;
    lodepng_state_init(&st);
    unsigned err = lodepng_inspect(&w, &h, &st, png, len);
    lodepng_state_cleanup(&st);
    if (!err && (w != fs->disp->logical_w || h != fs->disp->logical_h)) {
        resp_error_begin(resp, 422, "invalid_dimensions", "the frame must be exactly the display's logical size");
        sb_printf(&resp->body, ",\"expected\":[%u,%u],\"received\":[%u,%u]", fs->disp->logical_w,
                  fs->disp->logical_h, w, h);
        resp_error_end(resp);
        return NULL;
    }
    uint8_t *rgba = NULL;
    if (!err) err = lodepng_decode32(&rgba, &w, &h, png, len);
    if (err) {
        free(rgba);
        resp_error_begin(resp, 422, "invalid_image", "the body is not a valid PNG");
        sb_puts(&resp->body, ",\"detail\":");
        sb_json_str(&resp->body, lodepng_error_text(err));
        resp_error_end(resp);
        return NULL;
    }
    return rgba;
}

static void now_both(struct timespec *wall, struct timespec *mono) {
    clock_gettime(CLOCK_REALTIME, wall);
    clock_gettime(CLOCK_MONOTONIC, mono);
}

static void reply_frame(const struct frame_store *fs, struct response *resp) {
    resp->status = 200;
    frame_json(fs, &resp->body);
}

void frame_put(struct frame_store *fs, const struct http_request *req, const uint8_t *body, size_t len,
               struct response *resp) {
    char sha[65];
    sha256_hex(body, len, sha);
    const char *claimed = http_header(req, "X-Frame-SHA256");
    if (claimed && strcasecmp(claimed, sha) != 0) {
        resp_error_begin(resp, 400, "sha256_mismatch", "the body does not match X-Frame-SHA256");
        sb_puts(&resp->body, ",\"header\":");
        sb_json_str(&resp->body, claimed);
        sb_puts(&resp->body, ",\"computed\":");
        sb_json_str(&resp->body, sha);
        resp_error_end(resp);
        return;
    }
    int persist = persist_flag(req);
    char id[129];
    const char *given = http_header(req, "X-Frame-ID");
    if (given) {
        snprintf(id, sizeof id, "%s", given);
    } else {
        char hex[25];
        if (random_hex(hex, 12) != 0) {
            resp_error(resp, 500, "internal_error", "cannot read /dev/urandom for a frame id");
            return;
        }
        snprintf(id, sizeof id, "tt7d-%s", hex);
    }

    /* Same pixels as on screen: no decode, no redraw; only the receipt changes. */
    if (fs->have && strcmp(sha, fs->sha256) == 0) {
        if (persist && !fs->persisted && persist_write(fs, body, len, sha, id) != 0) {
            resp_error(resp, 500, "persist_failed", strerror(errno));
            return;
        }
        snprintf(fs->id, sizeof fs->id, "%s", id);
        now_both(&fs->received_at, &fs->received_mono);
        fs->received_known = 1;
        fs->persisted |= persist;
        fs->deduplicated = 1;
        fs->dedup_count++;
        reply_frame(fs, resp);
        return;
    }

    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) {
        resp_error(resp, 500, "internal_error", "out of memory");
        return;
    }
    uint8_t *rgba = decode_png(fs, body, len, resp);
    if (!rgba) {
        free(copy);
        return;
    }
    display_draw(fs->disp, rgba); /* into the back buffer; the screen is untouched */
    free(rgba);
    if (persist && persist_write(fs, body, len, sha, id) != 0) {
        free(copy);
        resp_error(resp, 500, "persist_failed", strerror(errno));
        return;
    }
    display_present(fs->disp);

    memcpy(copy, body, len);
    free(fs->png);
    fs->png = copy;
    fs->png_len = len;
    fs->have = 1;
    snprintf(fs->id, sizeof fs->id, "%s", id);
    memcpy(fs->sha256, sha, sizeof sha);
    now_both(&fs->received_at, &fs->received_mono);
    fs->displayed_at = fs->received_at;
    fs->received_known = 1;
    fs->persisted = persist;
    fs->deduplicated = 0;
    fs->restored = 0;
    fs->accepted++;
    reply_frame(fs, resp);
}

int frame_restore(struct frame_store *fs, size_t max_bytes, char *err, size_t errlen) {
    char path[512];
    path_in(fs, "last-frame.png", path, sizeof path);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    uint8_t *png = NULL;
    size_t len = 0;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && (size_t)st.st_size <= max_bytes &&
        (png = malloc((size_t)st.st_size)) != NULL) {
        len = (size_t)st.st_size;
        size_t got = 0;
        while (got < len) {
            ssize_t r = read(fd, png + got, len - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (got != len) {
            free(png);
            png = NULL;
        }
    }
    close(fd);
    if (!png) {
        snprintf(err, errlen, "%s: unreadable, empty, or larger than %zu bytes", path, max_bytes);
        return -1;
    }

    struct response probe = {0};
    uint8_t *rgba = decode_png(fs, png, len, &probe);
    if (!rgba) {
        snprintf(err, errlen, "%s: %s", path, probe.body.buf ? probe.body.buf : "cannot decode");
        sb_free(&probe.body);
        free(png);
        return -1;
    }
    display_draw(fs->disp, rgba);
    free(rgba);
    display_present(fs->disp);

    fs->have = 1;
    fs->png = png;
    fs->png_len = len;
    sha256_hex(png, len, fs->sha256);
    fs->id[0] = 0;
    char line[256], sha[65], id[129];
    path_in(fs, "last-frame.id", path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f && fgets(line, sizeof line, f) && sscanf(line, "%64s %128s", sha, id) == 2 && strcmp(sha, fs->sha256) == 0)
        snprintf(fs->id, sizeof fs->id, "%s", id);
    if (f) fclose(f);
    struct timespec wall;
    now_both(&wall, &fs->received_mono);
    fs->displayed_at = wall;
    fs->received_known = 0;
    fs->persisted = 1;
    fs->restored = 1;
    return 1;
}

void frame_json(const struct frame_store *fs, struct sbuf *sb) {
    sb_puts(sb, "{\"frame_id\":");
    sb_json_str(sb, fs->id[0] ? fs->id : NULL);
    sb_puts(sb, ",\"sha256\":");
    sb_json_str(sb, fs->sha256);
    sb_puts(sb, ",\"received_at\":");
    if (fs->received_known) sb_json_time(sb, &fs->received_at);
    else sb_puts(sb, "null");
    sb_puts(sb, ",\"displayed_at\":");
    sb_json_time(sb, &fs->displayed_at);
    sb_printf(sb,
              ",\"width\":%u,\"height\":%u,\"content_type\":\"image/png\",\"bytes\":%zu,\"persisted\":%s,"
              "\"deduplicated\":%s,\"restored\":%s}",
              fs->disp->logical_w, fs->disp->logical_h, fs->png_len, fs->persisted ? "true" : "false",
              fs->deduplicated ? "true" : "false", fs->restored ? "true" : "false");
}
