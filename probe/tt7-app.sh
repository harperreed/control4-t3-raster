#!/bin/sh
# ABOUTME: The TT7 image's app, run (and respawned) by init as /usr/bin/tt7-app (or /data/tt7/app).
# ABOUTME: Picks the web-installed release to run; NTP, Wi-Fi, discovery and input logger in the background; tt7d forever.
#
# Writes only under /data/tt7. If /data did not mount, results go to /tmp/tt7
# (RAM) so nothing lands on the ramdisk's empty /data mount point.
#
# Releases (web update, SPEC M8): tt7d's PUT /api/v1/system/update installs a
# bundle into /data/tt7/releases/<id>/ (app, bin/tt7d, ...), writes <id> to
# /data/tt7/update/current, the one before to update/previous, and "<id> 0" to
# update/trial. This script decides what runs (select_release below). A
# release under trial gets TT7_TRIAL_MAX starts, and tt7d deletes update/trial
# once it has served for a while. Otherwise the chain is: new release ->
# previous release (under trial too) -> the image's own build.
#
# PATH: a current release's bin first, then /data/tt7/bin, then the image's
# own directories. A build copied to /data/tt7/bin over ssh (tt7d, tt7probe,
# ...) replaces the image's copy without a reflash, when no release is current.

TT7_TRIAL_MAX=2       # starts a release gets before it is rolled back (init quarantines /data/tt7/app at 3)
TT7_TRIAL_DEADLINE=90 # seconds tt7d has to confirm a release under trial before it is stopped
TT7_EXIT_RESTART=75   # tt7d exits with this after an update or rollback: start over from the entry script

# The release id in $1/update/$2, or nothing if there is none or it is not an id.
rel_read() {
    rr_id=$(head -n 1 "$1/update/$2" 2> /dev/null)
    case $rr_id in '' | .* | -* | *[!A-Za-z0-9._+-]*) return 0 ;; esac
    echo "$rr_id"
}

# Point $1/update/$2 at release $3, or remove it if $3 is empty. rename() is atomic.
rel_write() {
    if [ -z "$3" ]; then
        rm -f "$1/update/$2"
    else
        echo "$3" > "$1/update/$2.tmp" && mv -f "$1/update/$2.tmp" "$1/update/$2"
    fi
}

rel_usable() { [ -x "$1/releases/$2/app" ] && [ -x "$1/releases/$2/bin/tt7d" ]; }

# One line to $1/update/history (GET /api/v1/system/update shows it) and to the log.
rel_log() {
    rl_root=$1
    shift
    echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*" >> "$rl_root/update/history"
    echo "tt7-app: update: $*" >&2
}

# The trial in $1/update/trial as "id starts", or nothing.
trial_read() {
    tr_id='' tr_n=''
    read -r tr_id tr_n 2> /dev/null < "$1/update/trial"
    case $tr_n in '' | *[!0-9]*) return 0 ;; esac
    [ -n "$tr_id" ] && echo "$tr_id $tr_n"
}

trial_write() { echo "$2 $3" > "$1/update/trial.tmp" && mv -f "$1/update/trial.tmp" "$1/update/trial"; }

# Exit 0 if release $2 is under trial.
trial_pending() {
    [ -n "$2" ] || return 1
    tp=$(trial_read "$1")
    [ "${tp% *}" = "$2" ]
}

# Release $2 failed ($3 says how): run the previous release under trial, or
# else the image's own build.
rel_fail_over() {
    fo_prev=$(rel_read "$1" previous)
    if [ -n "$fo_prev" ] && [ "$fo_prev" != "$2" ] && rel_usable "$1" "$fo_prev"; then
        trial_write "$1" "$fo_prev" 0
        rel_write "$1" current "$fo_prev"
        rel_write "$1" previous ""
        rel_log "$1" rolled_back "$2" "to=$fo_prev" "$3"
    else
        rel_write "$1" current ""
        rel_write "$1" previous ""
        rm -f "$1/update/trial"
        rel_log "$1" rolled_back "$2" to=image "$3"
    fi
    sync
}

# Print the release id to run from root $1 (nothing: the image's own build).
# Each call is a start: under trial it counts one, and a release that already
# had TT7_TRIAL_MAX starts without being confirmed is rolled back here.
select_release() {
    while :; do
        sr_cur=$(rel_read "$1" current)
        if [ -z "$sr_cur" ]; then
            [ -e "$1/update/current" ] && rel_write "$1" current "" # not a release id: never follow it
            rm -f "$1/update/trial"
            return 0
        fi
        if ! rel_usable "$1" "$sr_cur"; then
            rel_fail_over "$1" "$sr_cur" missing_or_not_executable
            continue
        fi
        sr_trial=$(trial_read "$1")
        if [ -n "$sr_trial" ] && [ "${sr_trial% *}" = "$sr_cur" ]; then
            sr_n=${sr_trial#* }
            if [ "$sr_n" -ge "$TT7_TRIAL_MAX" ]; then
                rel_fail_over "$1" "$sr_cur" "not_confirmed_after_${sr_n}_starts"
                continue
            fi
            trial_write "$1" "$sr_cur" $((sr_n + 1))
        elif [ -e "$1/update/trial" ]; then
            rm -f "$1/update/trial" # for another release, or unreadable
        fi
        echo "$sr_cur"
        return 0
    done
}

# tt7d from release $2 ("" for the image's build) exited with status $3.
# Prints what to do: restart (start over from the entry script), rollback
# (the same, once the trial has run out), or again (restart tt7d).
after_tt7d_exit() {
    if [ "$3" = "$TT7_EXIT_RESTART" ]; then
        echo restart
    elif trial_pending "$1" "$2"; then
        ae_n=$(trial_read "$1")
        ae_n=${ae_n#* }
        if [ "$ae_n" -ge "$TT7_TRIAL_MAX" ]; then
            echo rollback
        else
            trial_write "$1" "$2" $((ae_n + 1))
            echo again
        fi
    else
        echo again
    fi
}

# Tests source this file for the functions above (probe/test_tt7_app.py).
if [ "${TT7_APP_LIB:-}" = 1 ]; then
    return 0
fi

PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH

base=/data/tt7
if ! grep -q ' /data ' /proc/mounts; then
    echo "tt7-app: /data is not mounted; results go to RAM under /tmp/tt7 and vanish at reboot"
    base=/tmp/tt7
fi
disc=$base/discovery
mkdir -p "$disc" || exit 1

# Which release runs. Decided once per start of the entry script (init runs it
# at boot and again if it exits; this script starts over after an update):
# pick one, then exec the release's own app, which skips this step
# (TT7_SELECTED is set).
if [ -z "${TT7_SELECTED:-}" ]; then
    TT7_ENTRY=$0
    TT7_RELEASE=
    if [ "$base" = /data/tt7 ]; then
        mkdir -p "$base/update"
        TT7_RELEASE=$(select_release "$base")
    fi
    TT7_SELECTED=1
    export TT7_ENTRY TT7_RELEASE TT7_SELECTED
    if [ -n "$TT7_RELEASE" ]; then
        echo "tt7-app: running release $TT7_RELEASE"
        exec "$base/releases/$TT7_RELEASE/app"
    fi
    echo "tt7-app: running the image's own build (no release is current)"
fi
PATH=${TT7_RELEASE:+$base/releases/$TT7_RELEASE/bin:}/data/tt7/bin:/bin:/sbin:/usr/bin:/usr/sbin

# One results dir per boot. /tmp is RAM, so the marker dies with the boot and
# app respawns within a boot reuse the same dir.
marker=/tmp/tt7-results-dir
if [ -f "$marker" ]; then
    out=$(cat "$marker")
else
    n=$(cat "$disc/boot-count" 2>/dev/null)
    case "$n" in '' | *[!0-9]*) n=0 ;; esac
    n=$((n + 1))
    echo "$n" > "$disc/boot-count"
    # No RTC: the wall clock is meaningless this early, so name the dir by
    # boot number and seconds since kernel start.
    up=$(cut -d. -f1 /proc/uptime)
    out=$(printf '%s/boot-%04d-up%ss' "$disc" "$n" "$up")
    mkdir -p "$out" || exit 1
    echo "$out" > "$marker"
fi
echo "tt7-app: results in $out"

# Make /dev nodes for everything in sysfs (fb, input, video, sound, mtd...).
# init only creates a handful by hand.
mdev -s

# A dark backlight would hide the frames; turn it up only if it is at 0.
for bl in /sys/class/backlight/*; do
    [ -f "$bl/brightness" ] || continue
    if [ "$(cat "$bl/brightness")" = 0 ]; then
        max=$(cat "$bl/max_brightness")
        echo "tt7-app: $bl brightness was 0, setting $max"
        echo "$max" > "$bl/brightness"
    fi
done

# Everything below except tt7d runs in the background, so tt7d (and its
# fallback clock, SPEC 41.1) is on the screen within seconds of boot (SPEC 38).
# Each background job keeps a pid file in /tmp (RAM), so an app respawn
# within a boot does not start a second copy.
running() { [ -f "$1" ] && kill -0 "$(cat "$1")" 2> /dev/null; }

# NTP: BusyBox ntpd sets the clock; its -S hook (tt7-ntp-hook) writes
# /run/tt7/ntp-synced, and tt7d shows no time until that marker exists.
# Servers: the `server HOST` lines of $base/tt7d/ntp.conf if it exists (IP
# literals work too, for a network without DNS), else pool.ntp.org. It waits
# for a default route first: ntpd backs off failed DNS lookups to minutes
# between tries (networking/ntpd.c, HOSTNAME_INTERVAL * dns_errors), which
# would keep "Setting clock" up long after Wi-Fi connects. ntpd's own output
# goes nowhere (while offline it would reach flash every few minutes); the
# hook logs clock steps and losses of sync to $base/ntp.log.
ntp_pid=/tmp/tt7-ntpd.pid
if ! running "$ntp_pid"; then
    if [ -f "$base/tt7d/ntp.conf" ]; then
        cp "$base/tt7d/ntp.conf" /etc/ntp.conf
    else
        printf 'server %s.pool.ntp.org\n' 0 1 2 3 > /etc/ntp.conf
    fi
    hook=$(command -v tt7-ntp-hook)
    [ -n "$hook" ] || echo "tt7-app: no tt7-ntp-hook on PATH: tt7d will show 'Setting clock' forever"
    (
        while ! awk 'NR > 1 && $2 == "00000000" { found = 1 } END { exit !found }' /proc/net/route; do
            sleep 2
        done
        echo "tt7-app: default route up; starting ntpd ($(grep -c '^server' /etc/ntp.conf) servers, hook $hook)"
        TT7_NTP_LOG="$base/ntp.log" exec ntpd -n -S "$hook" > /dev/null 2>&1
    ) &
    echo $! > "$ntp_pid"
fi

# Wi-Fi, once scripts/wifi-setup.sh has put a config on the panel. In the
# background: the display must never wait on association or DHCP. It logs to
# /data/tt7/wifi.log and is safe to re-run on a respawn.
if [ -f /data/tt7/wifi/wpa_supplicant.conf ]; then
    echo "tt7-app: starting Wi-Fi (log /data/tt7/wifi.log)"
    tt7-wifi-start &
fi

# Hardware discovery, once per boot. It only reads (tt7probe fbinfo reads the
# fb ioctls and never draws), so it runs beside tt7d instead of before it.
discover_pid=/tmp/tt7-discover.pid
if [ ! -e "$out/discovery.done" ] && ! running "$discover_pid"; then
    (tt7-discover "$out" > "$out/discover.log" 2>&1 && : > "$out/discovery.done" && sync && echo "tt7-app: discovery finished") &
    echo $! > "$discover_pid"
fi

# Raw input logging (input-events.log) until M3 turns touches into events.
# `log` never opens the framebuffer: tt7d owns it.
logger_pid=/tmp/tt7probe-log.pid
if ! running "$logger_pid"; then
    tt7probe log "$out" >> "$out/tt7probe.log" 2>&1 &
    echo $! > "$logger_pid"
fi

# The display daemon (its log is this app's log). Restart it here rather than
# exiting, so a tt7d crash or a deliberate `killall tt7d` (to pick up a new
# /data/tt7/bin/tt7d) does not re-run the steps above. Under trial, tt7d has
# TT7_TRIAL_DEADLINE seconds to confirm its release, or it is stopped, which
# counts as a failed start. tt7d exits with TT7_EXIT_RESTART after installing
# an update or a rollback; this script then starts over from the entry script.
while :; do
    echo "tt7-app: starting $(command -v tt7d)${TT7_RELEASE:+ (release $TT7_RELEASE)}"
    tt7d --data-dir "$base/tt7d" &
    tt7d_pid=$!
    watch_pid=
    if trial_pending "$base" "$TT7_RELEASE"; then
        (
            sleep "$TT7_TRIAL_DEADLINE"
            if trial_pending "$base" "$TT7_RELEASE"; then
                echo "tt7-app: release $TT7_RELEASE not confirmed within ${TT7_TRIAL_DEADLINE}s; stopping tt7d"
                kill "$tt7d_pid"
            fi
        ) &
        watch_pid=$!
    fi
    wait "$tt7d_pid"
    rc=$?
    [ -n "$watch_pid" ] && kill "$watch_pid" 2> /dev/null
    case $(after_tt7d_exit "$base" "$TT7_RELEASE" "$rc") in
        restart | rollback)
            echo "tt7-app: tt7d exited ($rc); starting over from $TT7_ENTRY"
            unset TT7_SELECTED
            exec "$TT7_ENTRY"
            ;;
    esac
    echo "tt7-app: tt7d exited ($rc); restarting in 2 s"
    sleep 2
done
