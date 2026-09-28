/* ABOUTME: Web update of the tt7d bundle (SPEC M8): PUT/GET /api/v1/system/update, POST .../rollback,
 * ABOUTME: install into <root>/releases/<id>, the current/previous/trial files, confirm-when-healthy, restart. */
#ifndef TT7D_UPDATE_H
#define TT7D_UPDATE_H

#include <stddef.h>
#include <stdint.h>

#include "http.h"
#include "json.h"
#include "server.h"

#define UPDATE_MAX_BYTES (4u << 20)   /* largest bundle accepted; tt7d itself is about 0.3 MiB */
#define UPDATE_EXIT_RESTART 75        /* exit status that makes tt7-app start over (probe/tt7-app.sh) */
#define UPDATE_KEEP_RELEASES 3        /* at most this many release dirs; never current or previous */
#define UPDATE_RESTART_DELAY_MS 1000  /* lets the 202 reply get out before tt7d exits */
#define UPDATE_DEFAULT_MIN_FREE (8ull << 20) /* bytes /data must keep free after an install */

struct update_config {
    const char *root;      /* /data/tt7: releases/ and update/ live here */
    const char *data_dir;  /* tt7d's data dir: update-pubkey lives here */
    const char *proc_root; /* for meminfo */
    const char *token;
    const char *firmware_version, *build; /* this tt7d's */
    const char *release;   /* the release this tt7d runs from (TT7_RELEASE from tt7-app), or NULL */
    int refuse_downgrade;
    unsigned confirm_after_s; /* serving this long confirms a release under trial */
    unsigned long long min_free_bytes;
};

struct update {
    struct update_config cfg;
    int64_t started_ms;
    int confirm_checked;   /* the confirm step ran (it runs once) */
    int64_t restart_at_ms; /* 0: no restart pending */
    /* The last refused request, in RAM only: refusals never reach flash. */
    char last_error[48], last_error_time[32];
    int last_error_status;
};

void update_init(struct update *u, const struct update_config *cfg);

/* 1 if path is one of the update endpoints. */
int update_owns(const char *path);

/* Head-time checks for update_owns() paths: method, token, Content-Type, size,
 * free RAM, a restart already pending. 0 to go on to update_handle(), or -1
 * with resp filled. */
int update_check_head(struct update *u, const struct http_request *req, struct response *resp);

void update_handle(struct update *u, const struct http_request *req, const uint8_t *body, size_t len,
                   struct response *resp);

/* Remember a refused update request for GET's last_error. */
void update_on_reply(struct update *u, const struct http_request *req, const struct response *resp);

/* Poll loop hooks: the confirm timer and the restart after an install or
 * rollback. update_service exits the process with UPDATE_EXIT_RESTART when
 * that restart is due. */
void update_prepare(struct update *u, int64_t *wait_ms);
void update_service(struct update *u);

#endif
