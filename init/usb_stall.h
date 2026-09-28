/* ABOUTME: Pure decision logic for PID 1's USB link watchdog: spot an RNDIS link whose transmit side is dead.
 * ABOUTME: No I/O here, so test_usb_stall.c can drive it on the host with counter sequences. */
#ifndef TT7_USB_STALL_H
#define TT7_USB_STALL_H

/* Seen on the TT7 after a USB gadget reset: rndis0 keeps its address and stays
 * up, rx_packets keeps rising (the host's pings arrive), but tx_packets freezes
 * and nothing reaches the host. Only re-enabling the android_usb gadget fixes it.
 *
 * rx rising with tx flat is not proof on its own: a healthy idle link also
 * receives packets that need no reply (host mDNS, IPv6 router solicitations).
 * So on SUSPECT the caller transmits one packet of its own. On a working link
 * that send completes and tx rises by the next tick, clearing the window. */

#define USB_STALL_TICKS 2    /* consecutive ticks with tx flat (~6 s at 3 s/tick) */
#define USB_STALL_MIN_RX 2   /* packets received during those ticks: real traffic */
#define USB_STALL_HOLDOFF 30 /* seconds between two remedies */

enum usb_stall_verdict {
    USB_LINK_OK,      /* nothing to do */
    USB_LINK_SUSPECT, /* tx flat while rx arrived: send a probe packet now */
    USB_LINK_STALLED, /* still flat despite our probes: apply the remedy now */
};

struct usb_stall {
    int have_prev;
    unsigned long long rx, tx;      /* counters at the previous tick */
    int window_ticks;               /* consecutive ticks with tx flat, counted from the first rx */
    unsigned long long window_rx;   /* packets received during the window */
    int acted;
    long last_act;                  /* monotonic seconds of the last STALLED verdict */
    int last_ticks;                 /* window that triggered the last STALLED, for logging */
    unsigned long long last_rx;
};

/* Feed one tick's counters (monotonic seconds in `now`). A counter going
 * backwards (netdev recreated) restarts from a fresh baseline. */
enum usb_stall_verdict usb_stall_tick(struct usb_stall *st, unsigned long long rx, unsigned long long tx, long now);

#endif
