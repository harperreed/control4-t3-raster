/* ABOUTME: Readers for tt7d's settings files and bodies: KEY=VALUE lines (mqtt.conf, camera.conf) and flat
 * ABOUTME: JSON objects of strings, integers, booleans and null (the PUT /api/v1/config bodies). No nesting, no arrays. */
#ifndef TT7D_FLATCONF_H
#define TT7D_FLATCONF_H

#include <stddef.h>

enum jtype { J_STR, J_INT, J_BOOL, J_NULL, J_OTHER };

/* One setting from a KEY=VALUE line (key and value trimmed). Return >= 0 if
 * it was taken, or -1 with a message in err. */
typedef int (*flatconf_set_fn)(void *ctx, const char *key, const char *value, char *err, size_t errlen);

/* Apply KEY=VALUE lines: '#' comments and blank lines are skipped. Returns
 * 0, or -1 with "line N: ..." in err; the lines before the bad one are
 * already applied. */
int flatconf_parse_lines(const char *text, flatconf_set_fn set, void *ctx, char *err, size_t errlen);

/* One member of a flat JSON object. value is the string's UTF-8 text, or
 * the literal text of an integer, boolean or null (J_OTHER: any other
 * number). has_nul: the string held a \u0000. Return 0 to go on; any other
 * value stops the walk and is returned by flatjson_each. */
typedef int (*flatjson_member_fn)(void *ctx, const char *key, enum jtype type, const char *value, int has_nul);

#define FLATJSON_SYNTAX (-100)

/* Walk one flat JSON object (body[0..len)), calling member for each member
 * in order. Returns 0 at the end, a nonzero return of member, or
 * FLATJSON_SYNTAX if the body is not a flat JSON object. On a malformed or
 * too long value (longer than 299 bytes), bad_key[bad_key_size] names its
 * member; for any other syntax error it is "". */
int flatjson_each(const char *body, size_t len, flatjson_member_fn member, void *ctx, char *bad_key,
                  size_t bad_key_size);

#endif
