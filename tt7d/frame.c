/* ABOUTME: PUT /api/v1/frame (hash, dedup, decode, optional persistence, one copy to the screen) and PATCH (regions
 * ABOUTME: onto the kept logical RGBA, all or nothing). Also restores last-frame.png and renders the frame metadata. */
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
#include "regions.h"
#include "sha256.h"

void frame_store_init(struct frame_store *fs, const char *data_dir, struct display *disp) {
    memset(fs, 0, sizeof *fs);
    fs->data_dir = data_dir;
    fs->disp = disp;
    fs->regions = -1;
}

static int is_hex64(const char *s) { return strlen(s) == 64 && strspn(s, "0123456789abcdefABCDEF") == 64; }

/* X-Persist: absent -> 0, "true" -> 1, "false" -> 0, anything else -> -1. */
static int persist_flag(const struct http_request *req) {
    const char *v = http_header(req, "X-Persist");
    if (!v || strcasecmp(v, "false") == 0) return 0;
    return strcasecmp(v, "true") == 0 ? 1 : -1;
}

int frame_check_head(const char *token, const struct http_request *req, struct response *resp) {
    if (resp_require_bearer(token, req, resp) != 0) return -1;
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

int frame_check_patch_head(const char *token, const struct http_request *req, struct response *resp) {
    if (resp_require_bearer(token, req, resp) != 0) return -1;
    const char *ct = http_header(req, "Content-Type");
    size_t n = ct ? strcspn(ct, "; \t") : 0, want = strlen(REGIONS_CONTENT_TYPE);
    if (!ct || n != want || strncasecmp(ct, REGIONS_CONTENT_TYPE, want) != 0) {
        resp_error_begin(resp, 415, "unsupported_media_type",
                         "a frame PATCH must be sent as Content-Type: " REGIONS_CONTENT_TYPE);
        sb_puts(&resp->body, ",\"supported\":[\"" REGIONS_CONTENT_TYPE "\"]");
        resp_error_end(resp);
        return -1;
    }
    const char *base = http_header(req, "X-Base-Frame-ID");
    if (!base) {
        resp_error(resp, 400, "missing_base_frame_id", "a frame PATCH must name its base frame in X-Base-Frame-ID");
        return -1;
    }
    const char *id = http_header(req, "X-Frame-ID");
    if (!frame_id_valid(base) || (id && !frame_id_valid(id))) {
        resp_error(resp, 400, "invalid_frame_id",
                   "X-Frame-ID and X-Base-Frame-ID must be 1 to 128 printable ASCII characters, no spaces");
        return -1;
    }
    const char *sha = http_header(req, "X-Frame-SHA256");
    if (sha && !is_hex64(sha)) {
        resp_error(resp, 400, "invalid_sha256", "X-Frame-SHA256 must be 64 hex digits");
        return -1;
    }
    int persist = persist_flag(req);
    if (persist < 0) {
        resp_error(resp, 400, "invalid_persist", "X-Persist must be true or false");
        return -1;
    }
    if (persist) {
        resp_error(resp, 400, "persist_not_supported",
                   "a frame PATCH cannot be persisted; PUT the whole frame with X-Persist: true instead");
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

/* X-Frame-ID, or a new "tt7d-<24 hex>". Returns 0, or -1 with resp filled. */
static int request_frame_id(const struct http_request *req, char id[129], struct response *resp) {
    const char *given = http_header(req, "X-Frame-ID");
    if (given) {
        snprintf(id, 129, "%s", given);
        return 0;
    }
    char hex[25];
    if (random_hex(hex, 12) != 0) {
        resp_error(resp, 500, "internal_error", "cannot read /dev/urandom for a frame id");
        return -1;
    }
    snprintf(id, 129, "tt7d-%s", hex);
    return 0;
}

/* The receipt of an update that changed no pixels: a new id and time. */
static void receipt_only(struct frame_store *fs, const char *id) {
    snprintf(fs->id, sizeof fs->id, "%s", id);
    now_both(&fs->received_at, &fs->received_mono);
    fs->received_known = 1;
    fs->deduplicated = 1;
    fs->dedup_count++;
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
    char id[129];
    if (request_frame_id(req, id, resp) != 0) return;
    frame_show(fs, body, len, sha, id, persist_flag(req), resp);
}

void frame_show(struct frame_store *fs, const uint8_t *body, size_t len, const char *sha, const char *id,
                int persist, struct response *resp) {
    /* Same pixels as on screen: no decode, no redraw; only the receipt changes. */
    if (fs->have && fs->on_screen && strcmp(sha, fs->sha256) == 0) {
        if (persist && !fs->persisted && persist_write(fs, body, len, sha, id) != 0) {
            resp_error(resp, 500, "persist_failed", strerror(errno));
            return;
        }
        receipt_only(fs, id);
        fs->persisted |= persist;
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
    if (persist && persist_write(fs, body, len, sha, id) != 0) {
        free(copy);
        free(rgba);
        /* The back buffer holds pixels that never reached the screen. Put the
         * shown frame back, so a PATCH's rect copies show only what it changed. */
        if (fs->rgba && fs->on_screen) display_draw(fs->disp, fs->rgba);
        resp_error(resp, 500, "persist_failed", strerror(errno));
        return;
    }
    display_present(fs->disp);

    memcpy(copy, body, len);
    free(fs->png);
    fs->png = copy;
    fs->png_len = len;
    free(fs->rgba);
    fs->rgba = rgba;
    fs->update_bytes = len;
    fs->regions = -1;
    fs->have = 1;
    fs->on_screen = 1;
    snprintf(fs->id, sizeof fs->id, "%s", id);
    snprintf(fs->sha256, sizeof fs->sha256, "%s", sha);
    now_both(&fs->received_at, &fs->received_mono);
    fs->displayed_at = fs->received_at;
    fs->received_known = 1;
    fs->persisted = persist;
    fs->deduplicated = 0;
    fs->restored = 0;
    fs->accepted++;
    reply_frame(fs, resp);
}

/* 409 base_mismatch, naming what the screen shows instead. */
static void base_mismatch(const struct frame_store *fs, const char *covered_by, struct response *resp) {
    resp_error_begin(resp, 409, "base_mismatch",
                     "X-Base-Frame-ID is not the frame on screen; PUT the whole frame instead");
    sb_puts(&resp->body, ",\"current_frame_id\":");
    sb_json_str(&resp->body, covered_by ? covered_by : fs->have && fs->id[0] ? fs->id : NULL);
    resp_error_end(resp);
}

static void regions_reply_error(const struct regions_error *e, struct response *resp) {
    resp_error_begin(resp, e->status, e->code, e->message);
    if (e->index >= 0) sb_printf(&resp->body, ",\"region\":%d", e->index);
    if (!strcmp(e->code, "too_many_regions")) sb_printf(&resp->body, ",\"max\":%d", REGIONS_MAX);
    if (!strcmp(e->code, "region_size_mismatch"))
        sb_printf(&resp->body, ",\"expected\":[%u,%u],\"received\":[%u,%u]", e->expected_w, e->expected_h,
                  e->received_w, e->received_h);
    if (e->detail) {
        sb_puts(&resp->body, ",\"detail\":");
        sb_json_str(&resp->body, e->detail);
    }
    resp_error_end(resp);
}

void frame_patch(struct frame_store *fs, const struct http_request *req, const uint8_t *body, size_t len,
                 const char *covered_by, struct response *resp) {
    const char *base = http_header(req, "X-Base-Frame-ID");
    if (covered_by || !fs->have || !fs->on_screen || !fs->rgba || !base || strcmp(base, fs->id) != 0) {
        base_mismatch(fs, covered_by, resp);
        return;
    }
    struct region r[REGIONS_MAX];
    int n = 0;
    struct regions_error e = {0};
    if (regions_parse(body, len, fs->disp->logical_w, fs->disp->logical_h, r, &n, &e) != 0) {
        regions_reply_error(&e, resp);
        return;
    }
    char sha[65];
    if (n == 0) { /* no pixels change, so neither does the hash */
        snprintf(sha, sizeof sha, "%s", fs->sha256);
    } else if (regions_frame_sha(fs->sha256, body, len, sha) != 0) {
        resp_error(resp, 500, "internal_error", "out of memory");
        return;
    }
    const char *claimed = http_header(req, "X-Frame-SHA256");
    if (claimed && strcasecmp(claimed, sha) != 0) {
        resp_error_begin(resp, 400, "sha256_mismatch", "X-Frame-SHA256 is not the patched frame's SHA-256");
        sb_puts(&resp->body, ",\"header\":");
        sb_json_str(&resp->body, claimed);
        sb_puts(&resp->body, ",\"computed\":");
        sb_json_str(&resp->body, sha);
        resp_error_end(resp);
        return;
    }
    char id[129];
    if (request_frame_id(req, id, resp) != 0) return;
    if (n == 0) {
        receipt_only(fs, id);
        reply_frame(fs, resp);
        return;
    }
    if (regions_apply(r, n, fs->rgba, fs->disp->logical_w, fs->disp->logical_h, &e) != 0) {
        regions_reply_error(&e, resp); /* nothing was applied */
        return;
    }
    /* Every region decoded and is in fs->rgba: from here nothing can fail. */
    for (int i = 0; i < n; i++) display_draw_rect(fs->disp, fs->rgba, r[i].x, r[i].y, r[i].w, r[i].h);
    for (int i = 0; i < n; i++) display_present_rect(fs->disp, r[i].x, r[i].y, r[i].w, r[i].h);

    free(fs->png);
    fs->png = NULL; /* GET /frame/image encodes the composed frame when asked */
    fs->png_len = 0;
    snprintf(fs->id, sizeof fs->id, "%s", id);
    snprintf(fs->sha256, sizeof fs->sha256, "%s", sha);
    now_both(&fs->received_at, &fs->received_mono);
    fs->displayed_at = fs->received_at;
    fs->received_known = 1;
    fs->persisted = 0;
    fs->deduplicated = 0;
    fs->restored = 0;
    fs->update_bytes = len;
    fs->regions = n;
    fs->accepted++;
    fs->region_updates++;
    reply_frame(fs, resp);
}

/* Filter "zero", a 512-byte window and no lazy matching: a 1280x800 clock
 * face (mostly flat colour) came out about 49 KB in 38 ms on the dev host,
 * against 385 ms with lodepng's defaults (2026-09-28). It blocks the poll
 * loop while it runs; expect several times longer on the panel's ARM core
 * (not measured). */
int frame_encode_png(const uint8_t *rgba, unsigned w, unsigned h, uint8_t **png, size_t *len) {
    LodePNGState st;
    lodepng_state_init(&st);
    st.info_raw.colortype = LCT_RGBA;
    st.info_png.color.colortype = LCT_RGB;
    st.encoder.auto_convert = 0;
    st.encoder.filter_strategy = LFS_ZERO;
    st.encoder.zlibsettings.windowsize = 512;
    st.encoder.zlibsettings.lazymatching = 0;
    *png = NULL;
    unsigned err = lodepng_encode(png, len, rgba, w, h, &st);
    lodepng_state_cleanup(&st);
    if (err) {
        free(*png);
        *png = NULL;
        return -1;
    }
    return 0;
}

void frame_image(struct frame_store *fs, struct response *resp) {
    if (!fs->png && frame_encode_png(fs->rgba, fs->disp->logical_w, fs->disp->logical_h, &fs->png, &fs->png_len)) {
        resp_error(resp, 500, "internal_error", "cannot encode the frame as PNG");
        return;
    }
    resp->content_type = "image/png";
    sb_add(&resp->body, fs->png, fs->png_len);
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
    display_present(fs->disp);

    fs->have = 1;
    fs->on_screen = 1;
    fs->rgba = rgba;
    fs->png = png;
    fs->png_len = len;
    fs->update_bytes = len;
    fs->regions = -1;
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
              "\"deduplicated\":%s,\"restored\":%s,\"updated_via\":\"%s\",\"regions\":",
              fs->disp->logical_w, fs->disp->logical_h, fs->update_bytes, fs->persisted ? "true" : "false",
              fs->deduplicated ? "true" : "false", fs->restored ? "true" : "false",
              fs->regions < 0 ? "full" : "regions");
    if (fs->regions < 0) sb_puts(sb, "null}");
    else sb_printf(sb, "%d}", fs->regions);
}
