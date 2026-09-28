/* ABOUTME: Pure decision logic for PID 1's USB link watchdog: spot an RNDIS link whose transmit side is dead.
 * ABOUTME: See usb_stall.h for the field symptom and why a probe packet guards against false positives. */
#include "usb_stall.h"

static void clear_window(struct usb_stall *st) {
    st->window_ticks = 0;
    st->window_rx = 0;
}

enum usb_stall_verdict usb_stall_tick(struct usb_stall *st, unsigned long long rx, unsigned long long tx, long now) {
    if (!st->have_prev || rx < st->rx || tx < st->tx) {
        st->have_prev = 1;
        st->rx = rx;
        st->tx = tx;
        clear_window(st);
        return USB_LINK_OK;
    }
    unsigned long long drx = rx - st->rx, dtx = tx - st->tx;
    st->rx = rx;
    st->tx = tx;

    if (dtx > 0) { /* the link transmits: healthy */
        clear_window(st);
        return USB_LINK_OK;
    }
    if (st->window_ticks == 0 && drx == 0) /* idle, nothing waiting on a reply */
        return USB_LINK_OK;

    st->window_ticks++;
    st->window_rx += drx;
    if (st->window_ticks < USB_STALL_TICKS || st->window_rx < USB_STALL_MIN_RX)
        return USB_LINK_SUSPECT;
    if (st->acted && now - st->last_act < USB_STALL_HOLDOFF)
        return USB_LINK_SUSPECT; /* stalled, but the last remedy was too recent */

    st->acted = 1;
    st->last_act = now;
    st->last_ticks = st->window_ticks;
    st->last_rx = st->window_rx;
    clear_window(st);
    return USB_LINK_STALLED;
}
