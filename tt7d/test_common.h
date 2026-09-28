/* ABOUTME: Shared CHECK macro and exit convention for tt7d's host unit tests.
 * ABOUTME: Each test binary counts failures and exits 1 if any check failed. */
#ifndef TT7D_TEST_COMMON_H
#define TT7D_TEST_COMMON_H

#include <stdio.h>

static int failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

static inline int test_finish(const char *name) {
    if (failures) {
        fprintf(stderr, "%s: %d failure(s)\n", name, failures);
        return 1;
    }
    printf("  ok   %s\n", name);
    return 0;
}

#endif
