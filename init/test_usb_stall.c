/* ABOUTME: Host unit tests for usb_stall.c: rx/tx counter sequences in, watchdog verdicts out.
 * ABOUTME: Build and run with `make test-host`; exits non-zero on the first failed check. */
#include <stdio.h>
#include <string.h>

#include "usb_stall.h"

static int failures;

static const char *name(enum usb_stall_verdict v) {
    return v == USB_LINK_OK ? "OK" : v == USB_LINK_SUSPECT ? "SUSPECT" : "STALLED";
}

/* Feed `n` ticks 3 s apart starting at t=100 and compare every verdict. */
static void run(const char *what, int n, const unsigned long long rx[], const unsigned long long tx[],
                const enum usb_stall_verdict want[]) {
    struct usb_stall st;
    memset(&st, 0, sizeof st);
    for (int i = 0; i < n; i++) {
        enum usb_stall_verdict got = usb_stall_tick(&st, rx[i], tx[i], 100 + 3L * i);
        if (got != want[i]) {
            fprintf(stderr, "FAIL %s: tick %d (rx %llu tx %llu) got %s want %s\n", what, i, rx[i], tx[i],
                    name(got), name(want[i]));
            failures++;
            return;
        }
    }
    printf("  ok   %s\n", what);
}

#define O USB_LINK_OK
#define S USB_LINK_SUSPECT
#define X USB_LINK_STALLED

int main(void) {
    { /* The field case: pings arrive (rx +3/tick), replies never leave (tx frozen at 289514). */
        unsigned long long rx[] = {1000, 1003, 1006, 1009};
        unsigned long long tx[] = {289514, 289514, 289514, 289514};
        enum usb_stall_verdict want[] = {O, S, X, S};
        run("field stall: rx rising, tx frozen -> probe, then remedy after 2 ticks", 4, rx, tx, want);
    }
    { unsigned long long rx[] = {10, 13, 16, 19, 22};
      unsigned long long tx[] = {10, 13, 16, 19, 22};
      enum usb_stall_verdict want[] = {O, O, O, O, O};
      run("healthy traffic: tx follows rx -> never acts", 5, rx, tx, want); }
    { unsigned long long rx[] = {5, 5, 5, 5, 5};
      unsigned long long tx[] = {7, 7, 7, 7, 7};
      enum usb_stall_verdict want[] = {O, O, O, O, O};
      run("idle link: nothing moves -> never acts", 5, rx, tx, want); }
    { /* Host multicast needs no reply; our probe goes out and completes, clearing it. */
        unsigned long long rx[] = {50, 53, 53, 56, 56};
        unsigned long long tx[] = {9, 9, 10, 10, 11};
        enum usb_stall_verdict want[] = {O, S, O, S, O};
        run("unanswered multicast on a healthy link: probe tx clears it", 5, rx, tx, want); }
    { /* One stray packet: tx frozen, but not enough traffic to call it a stall. */
        unsigned long long rx[] = {50, 51, 51, 51, 51};
        unsigned long long tx[] = {9, 9, 9, 9, 9};
        enum usb_stall_verdict want[] = {O, S, S, S, S};
        run("a single received packet never triggers the remedy", 5, rx, tx, want); }
    { /* Slow pings: 1 packet per tick is enough once the window has 2. */
        unsigned long long rx[] = {50, 51, 52};
        unsigned long long tx[] = {9, 9, 9};
        enum usb_stall_verdict want[] = {O, S, X};
        run("slow traffic: 2 packets over 2 frozen ticks acts", 3, rx, tx, want); }
    { /* Stall continues: remedy at t=106, then held off until 30 s have passed. */
        unsigned long long rx[16], tx[16];
        enum usb_stall_verdict want[16];
        for (int i = 0; i < 16; i++) { rx[i] = 100 + 3 * i; tx[i] = 7; want[i] = S; }
        want[0] = O;
        want[2] = X;   /* t=106 */
        want[12] = X;  /* t=136: exactly 30 s after t=106; ticks 3..11 were held off */
        run("rate limit: at most one remedy per 30 s while stalled", 16, rx, tx, want); }
    { /* Remedy recreates the netdev: counters restart from 0, which is a new baseline. */
        unsigned long long rx[] = {100, 103, 106, 2, 5, 8};
        unsigned long long tx[] = {7, 7, 7, 1, 4, 7};
        enum usb_stall_verdict want[] = {O, S, X, O, O, O};
        run("counters going backwards reset the baseline", 6, rx, tx, want); }

    if (failures) {
        fprintf(stderr, "test_usb_stall: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("test_usb_stall: all checks passed\n");
    return 0;
}
