/* ABOUTME: Host unit tests for the embedded control panel assets: lookup by name, content types,
 * ABOUTME: and guards that the HTML and JS stay within the CSP (no inline script/style, no innerHTML). */
#include <string.h>

#include "assets.h"
#include "test_common.h"

/* memmem for a blob that is not NUL-terminated. */
static int contains(const struct asset *a, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; a && i + n <= a->len; i++)
        if (!memcmp(a->data + i, needle, n)) return 1;
    return 0;
}

static void test_lookup(void) {
    const struct asset *html = asset_find("/");
    CHECK(html && !strcmp(html->content_type, "text/html; charset=utf-8"), "/ is the HTML page");
    CHECK(html && html->len > 15 && !memcmp(html->data, "<!doctype html>", 15), "HTML starts with the doctype");
    const struct asset *css = asset_find("/panel.css");
    CHECK(css && !strcmp(css->content_type, "text/css; charset=utf-8"), "css type");
    const struct asset *js = asset_find("/panel.js");
    CHECK(js && !strcmp(js->content_type, "text/javascript; charset=utf-8"), "js type");
    const struct asset *ujs = asset_find("/update.js");
    CHECK(ujs && !strcmp(ujs->content_type, "text/javascript; charset=utf-8"), "update.js type");
    CHECK(asset_find("/nope") == NULL && asset_find("/index.html") == NULL && asset_find("") == NULL, "unknown");
    CHECK(asset_find("/panel.JS") == NULL, "exact match only");

    const struct asset *tp = asset_find(ASSET_TEST_PATTERN);
    CHECK(tp && !strcmp(tp->content_type, "image/png") && tp->len > 8 && !memcmp(tp->data, "\x89PNG\r\n\x1a\n", 8),
          "the built-in test pattern is a PNG");
    CHECK(ASSET_TEST_PATTERN[0] != '/', "the test pattern is not reachable as a URL path");

    size_t total = 0;
    for (size_t i = 0; i < assets_count; i++) total += assets[i].len;
    CHECK(total < 96 * 1024, "embedded assets stay small: %zu bytes", total);
}

static void js_guards(const struct asset *js, const char *name) {
    CHECK(!contains(js, "innerHTML") && !contains(js, "outerHTML") && !contains(js, "insertAdjacentHTML") &&
              !contains(js, "eval(") && !contains(js, "localStorage"),
          "%s: dynamic text goes through textContent; the token lives in sessionStorage only", name);
    CHECK(!contains(js, "http://") && !contains(js, "https://"), "%s: no external fetches", name);
}

static void test_csp_guards(void) {
    const struct asset *html = asset_find("/"), *js = asset_find("/panel.js"), *ujs = asset_find("/update.js");
    CHECK(!contains(html, "<script>") && !contains(html, "<style") && !contains(html, " style=") &&
              !contains(html, " onclick=") && !contains(html, " onload=") && !contains(html, " onchange=") &&
              !contains(html, " oninput=") && !contains(html, " onsubmit="),
          "no inline script, style or event handlers in the HTML");
    CHECK(contains(html, "<script src=\"/panel.js\"") && contains(html, "href=\"/panel.css\""), "external files");
    CHECK(contains(html, "<script src=\"/update.js\" defer>"), "the update section's script, deferred after panel.js");
    js_guards(js, "panel.js");
    js_guards(ujs, "update.js");
}

int main(void) {
    test_lookup();
    test_csp_guards();
    return test_finish("test_assets");
}
