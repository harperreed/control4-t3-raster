#!/bin/sh
# ABOUTME: The TT7 image's app, run (and respawned) by init as /usr/bin/tt7-app (or /data/tt7/app).
# ABOUTME: One results dir per boot, discovery once, input logger, Wi-Fi, then tt7d (the display daemon) forever.
#
# Writes only under /data/tt7. If /data did not mount, results go to /tmp/tt7
# (RAM) so nothing lands on the ramdisk's empty /data mount point.
#
# /data/tt7/bin comes first on PATH: a build copied there over ssh (tt7d,
# tt7probe, ...) replaces the image's copy without a reflash. Delete it to go
# back to the image's.

PATH=/data/tt7/bin:/bin:/sbin:/usr/bin:/usr/sbin
export PATH

base=/data/tt7
if ! grep -q ' /data ' /proc/mounts; then
    echo "tt7-app: /data is not mounted; results go to RAM under /tmp/tt7 and vanish at reboot"
    base=/tmp/tt7
fi
disc=$base/discovery
mkdir -p "$disc" || exit 1

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

if [ ! -e "$out/discovery.done" ]; then
    tt7-discover "$out" > "$out/discover.log" 2>&1
    : > "$out/discovery.done"
    sync
    echo "tt7-app: discovery finished"
fi

# Raw input logging (input-events.log) until M3 turns touches into events.
# `log` never opens the framebuffer: tt7d owns it. One logger per boot, so an
# app respawn does not start a second one.
logger_pid=/tmp/tt7probe-log.pid
if ! { [ -f "$logger_pid" ] && kill -0 "$(cat "$logger_pid")" 2> /dev/null; }; then
    tt7probe log "$out" >> "$out/tt7probe.log" 2>&1 &
    echo $! > "$logger_pid"
fi

# Wi-Fi, once scripts/wifi-setup.sh has put a config on the panel. In the
# background: the display must never wait on association or DHCP. It logs to
# /data/tt7/wifi.log and is safe to re-run on a respawn.
if [ -f /data/tt7/wifi/wpa_supplicant.conf ]; then
    echo "tt7-app: starting Wi-Fi (log /data/tt7/wifi.log)"
    tt7-wifi-start &
fi

# The display daemon, in the foreground (its log is this app's log). Restart
# it here rather than exiting, so a tt7d crash or a deliberate `killall tt7d`
# (to pick up a new /data/tt7/bin/tt7d) does not re-run the steps above.
while :; do
    echo "tt7-app: starting $(command -v tt7d)"
    tt7d --data-dir "$base/tt7d"
    echo "tt7-app: tt7d exited ($?); restarting in 2 s"
    sleep 2
done
