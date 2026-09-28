/* ABOUTME: The fallback clock on the real display: state machine ticks from the poll loop, redraws through
 * ABOUTME: the same display_draw/display_present path as frames, and the heartbeat, /frame and /state glue. */
#include "fallback_screen.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "lodepng.h"
#include "sha256.h"
#include "sysinfo.h"
#include "timesync.h"

/* How often, while the clock shows, tt7d looks at the time, the sync marker
 * and the IP address. A redraw happens only when the words change: once a
 * minute, and when the clock gets synchronized or the address changes. */
#define CHECK_MS 5000

static int64_t mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int fallback_screen_init(struct fallback_screen *s, const struct fallback_screen_config *c, struct display *d,
                         struct frame_store *frames, int restored, char *err, size_t errlen) {
    memset(s, 0, sizeof *s);
    s->disp = d;
    s->frames = frames;
    s->hour12 = c->hour12;
    s->marker = c->marker;
    snprintf(s->tz, sizeof s->tz, "%s", c->tz);
    const struct asset *ta = asset_find(ASSET_FONT_TIME), *xa = asset_find(ASSET_FONT_TEXT);
    s->fonts.time = ta ? font_load(ta->data, ta->len) : NULL;
    s->fonts.text = xa ? font_load(xa->data, xa->len) : NULL;
    if (!s->fonts.time || !s->fonts.text) {
        snprintf(err, errlen, "the embedded clock fonts do not load");
        return -1;
    }
    int64_t now = mono_ms();
    fallback_init(&s->state, c->timeout_s, now);
    if (restored) fallback_frame(&s->state, now);
    return 0;
}

void fallback_screen_frame_accepted(struct fallback_screen *s) {
    fallback_frame(&s->state, mono_ms());
    s->drawn = 0;
}

/* Wi-Fi's address if it has one, else the first other non-loopback IPv4. */
static void screen_ip(char *out, size_t n) {
    ipv4_of("wlan0", out, n);
    if (*out) return;
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) return;
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, (socklen_t)n);
        break;
    }
    freeifaddrs(list);
}

static void redraw(struct fallback_screen *s, const struct clockface_text *t, const struct timespec *wall) {
    size_t len = (size_t)s->disp->logical_w * s->disp->logical_h * 4;
    uint8_t *rgba = malloc(len);
    if (!rgba) {
        fprintf(stderr, "tt7d: fallback clock: cannot allocate %zu bytes\n", len);
        return;
    }
    clockface_render(&s->fonts, t, rgba, (int)s->disp->logical_w, (int)s->disp->logical_h);
    display_draw(s->disp, rgba); /* the frames' own path: rotation and pixel format */
    display_present(s->disp);
    free(rgba);
    s->frames->on_screen = 0; /* the next frame PUT redraws even if it is a duplicate */
    s->drawn = 1;
    s->shown = *t;
    s->redraws++;
    snprintf(s->id, sizeof s->id, "fallback-clock-%lld", (long long)(wall->tv_sec / 60));
    s->drawn_wall = *wall;
    clock_gettime(CLOCK_MONOTONIC, &s->drawn_mono);
}

void fallback_screen_service(struct fallback_screen *s) {
    int64_t now = mono_ms();
    if (fallback_update(&s->state, now))
        fprintf(stderr, "tt7d: no frame or heartbeat for %u s: showing the fallback clock\n", s->state.timeout_s);
    if (!s->state.active) return;
    if (s->drawn && now < s->next_check_ms) return;

    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    time_t synced_at;
    int synced = timesync_synced(s->marker, wall.tv_sec, &synced_at);
    if (synced && !s->synced_logged) {
        fprintf(stderr, "tt7d: clock synchronized (%s); fallback clock times in TZ %s\n", s->marker, s->tz);
        s->synced_logged = 1;
    }
    char ip[INET_ADDRSTRLEN] = "";
    screen_ip(ip, sizeof ip);
    struct clockface_text t;
    clockface_text(wall.tv_sec, synced, s->hour12, ip, &t);
    if (!s->drawn || memcmp(&t, &s->shown, sizeof t) != 0) {
        if (!s->drawn && s->state.reason == FALLBACK_NO_FRAME && !s->redraws)
            fprintf(stderr, "tt7d: no frame yet: showing the fallback clock (%s)\n",
                    synced ? "clock synchronized" : "waiting for the clock to be set");
        redraw(s, &t, &wall);
    }
    /* Look again at the next minute boundary or in CHECK_MS, whichever is first. */
    int64_t to_minute = 60000 - ((int64_t)(wall.tv_sec % 60) * 1000 + wall.tv_nsec / 1000000) + 5;
    s->next_check_ms = now + (to_minute < CHECK_MS ? to_minute : CHECK_MS);
}

void fallback_screen_prepare(struct fallback_screen *s, int64_t *wait_ms) {
    int64_t now = mono_ms(), w = fallback_ms_until_timeout(&s->state, now);
    if (s->state.active) w = s->drawn && s->next_check_ms > now ? s->next_check_ms - now : 0;
    if (w >= 0 && (*wait_ms < 0 || w < *wait_ms)) *wait_ms = w;
}

int fallback_screen_check_head(const char *token, const struct http_request *req, struct response *resp) {
    if (strcmp(req->path, "/api/v1/heartbeat") != 0) return FALLBACK_NOT_MINE;
    if (strcmp(req->method, "POST") != 0) {
        resp_error(resp, 405, "method_not_allowed", "this method is not supported on this path");
        snprintf(resp->extra_headers, sizeof resp->extra_headers, "Allow: POST\r\n");
        return -1;
    }
    return resp_require_bearer(token, req, resp);
}

void fallback_screen_handle(struct fallback_screen *s, struct response *resp) {
    int64_t now = mono_ms();
    fallback_heartbeat(&s->state, now);
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    resp->status = 200;
    sb_puts(&resp->body, "{");
    fallback_json(&s->state, now, &wall, 1, &resp->body);
    sb_puts(&resp->body, "}");
}

/* Encode the current drawing as PNG, once per drawing, and only when asked
 * (the control panel preview). Filter "zero", a 512-byte window and no lazy
 * matching: the face is mostly flat colour, so the PNG stays about 49 KB and
 * took 38 ms on the dev host, against 385 ms with lodepng's defaults
 * (2026-09-28). It blocks the poll loop while it runs; expect several times
 * longer on the panel's ARM core (not measured). */
static int ensure_png(struct fallback_screen *s) {
    if (s->png && s->png_redraw == s->redraws) return 0;
    size_t len = (size_t)s->disp->logical_w * s->disp->logical_h * 4;
    uint8_t *rgba = malloc(len);
    if (!rgba) return -1;
    clockface_render(&s->fonts, &s->shown, rgba, (int)s->disp->logical_w, (int)s->disp->logical_h);
    LodePNGState st;
    lodepng_state_init(&st);
    st.info_raw.colortype = LCT_RGBA;
    st.info_png.color.colortype = LCT_RGB;
    st.encoder.auto_convert = 0;
    st.encoder.filter_strategy = LFS_ZERO;
    st.encoder.zlibsettings.windowsize = 512;
    st.encoder.zlibsettings.lazymatching = 0;
    unsigned char *png = NULL;
    size_t n = 0;
    unsigned err = lodepng_encode(&png, &n, rgba, s->disp->logical_w, s->disp->logical_h, &st);
    lodepng_state_cleanup(&st);
    free(rgba);
    if (err) {
        free(png);
        return -1;
    }
    free(s->png);
    s->png = png;
    s->png_len = n;
    s->png_redraw = s->redraws;
    sha256_hex(png, n, s->png_sha);
    return 0;
}

void fallback_screen_frame_json(struct fallback_screen *s, struct sbuf *sb) {
    int ok = ensure_png(s) == 0;
    sb_puts(sb, "{\"frame_id\":");
    sb_json_str(sb, s->id);
    sb_puts(sb, ",\"sha256\":");
    sb_json_str(sb, ok ? s->png_sha : NULL);
    sb_puts(sb, ",\"received_at\":null,\"displayed_at\":");
    sb_json_time(sb, &s->drawn_wall);
    sb_printf(sb,
              ",\"width\":%u,\"height\":%u,\"content_type\":\"image/png\",\"bytes\":%zu,\"persisted\":false,"
              "\"deduplicated\":false,\"restored\":false}",
              s->disp->logical_w, s->disp->logical_h, ok ? s->png_len : 0);
}

void fallback_screen_image(struct fallback_screen *s, struct response *resp) {
    if (ensure_png(s) != 0) {
        resp_error(resp, 500, "internal_error", "cannot encode the fallback clock as PNG");
        return;
    }
    resp->content_type = "image/png";
    sb_add(&resp->body, s->png, s->png_len);
}

void fallback_screen_state_members(struct fallback_screen *s, struct sbuf *sb) {
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    fallback_json(&s->state, mono_ms(), &wall, 1, sb);
    time_t synced_at;
    int synced = timesync_synced(s->marker, wall.tv_sec, &synced_at);
    sb_printf(sb, ",\"clock\":{\"synced\":%s,\"synced_at\":", synced ? "true" : "false");
    if (synced) {
        struct timespec t = {.tv_sec = synced_at};
        sb_json_time(sb, &t);
    } else {
        sb_puts(sb, "null");
    }
    sb_puts(sb, ",\"timezone\":");
    sb_json_str(sb, s->tz);
    sb_printf(sb, ",\"format\":\"%s\"}", s->hour12 ? "12h" : "24h");
}
