#!/bin/sh
# ABOUTME: The TT7 probe image's app, run (and respawned) by init as /usr/bin/tt7-app.
# ABOUTME: Picks one results dir per boot under /data/tt7/discovery, runs discovery once, runs tt7probe.
#
# Writes only under /data/tt7. If /data did not mount, results go to /tmp/tt7
# (RAM) so nothing lands on the ramdisk's empty /data mount point.

PATH=/bin:/sbin:/usr/bin:/usr/sbin
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

# A dark backlight would hide the test pattern; turn it up only if it is at 0.
for bl in /sys/class/backlight/*; do
    [ -f "$bl/brightness" ] || continue
    if [ "$(cat "$bl/brightness")" = 0 ]; then
        max=$(cat "$bl/max_brightness")
        echo "tt7-app: $bl brightness was 0, setting $max"
        echo "$max" > "$bl/brightness"
    fi
done

tt7probe run "$out" >> "$out/tt7probe.log" 2>&1 &
probe=$!

if [ ! -e "$out/discovery.done" ]; then
    tt7-discover "$out" > "$out/discover.log" 2>&1
    : > "$out/discovery.done"
    sync
    echo "tt7-app: discovery finished"
fi

wait "$probe"
echo "tt7-app: tt7probe exited ($?); init respawns this app in 5 s"
sleep 5
