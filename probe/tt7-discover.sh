#!/bin/sh
# ABOUTME: One-shot hardware discovery for the TT7 (SPEC.md section 44 command list plus extras).
# ABOUTME: Usage: tt7-discover <outdir>. Writes one text file per command into <outdir>; reads only.

out=${1:?usage: tt7-discover <outdir>}
mkdir -p "$out" || exit 1
PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH

# cap <file> <command...>: run with a 10 s limit, keep stdout+stderr and the exit code.
cap() {
    f=$1
    shift
    {
        echo "\$ $*"
        timeout 10 "$@"
        echo "[exit $?]"
    } > "$out/$f.txt" 2>&1
}

# dump <file> <dir>...: every readable regular file directly in each dir, with its name.
# sysfs attributes are small; a 5 s limit guards against a driver that blocks.
dump() {
    f=$1
    shift
    for d in "$@"; do
        [ -d "$d" ] || { echo "== $d: absent"; continue; }
        for a in "$d"/*; do
            [ -f "$a" ] || continue
            echo "== $a"
            timeout 5 cat "$a" 2>&1
        done
    done > "$out/$f.txt" 2>&1
}

# SPEC.md section 44, in its order.
cap uname-a uname -a
cap uname-v uname -v
cap cpuinfo cat /proc/cpuinfo
cap cmdline cat /proc/cmdline
cap partitions cat /proc/partitions
cap mtd cat /proc/mtd
cap mount mount
cap df df -h
cap sys-class find /sys/class -maxdepth 2
cap dev-fb ls -la /dev/fb0 /dev/graphics
cap dev-input ls -la /dev/input
cap dev-video sh -c 'ls -la /dev/video*'
cap dev-snd ls -la /dev/snd
cap input-devices cat /proc/bus/input/devices
cap ip-addr ip addr
cap ip-link ip link
cap dmesg dmesg

# "Where available" tools. The image has fbset (BusyBox) and our ioctl dump;
# evtest/aplay/arecord/v4l2-ctl are not in the image, so record stand-ins.
cap fbset fbset
cap fb-ioctl tt7probe fbinfo
echo "not in image: tt7probe logs raw events to input-events.log instead" > "$out/evtest.txt"
cap asound-cards cat /proc/asound/cards /proc/asound/devices /proc/asound/pcm
echo "not in image: see dev-video.txt and sys-class.txt (video4linux)" > "$out/v4l2-ctl.txt"

# Extras for the display, touch, battery and board.
cap version cat /proc/version
cap meminfo cat /proc/meminfo
cap modules cat /proc/modules
cap interrupts cat /proc/interrupts
cap proc-fb cat /proc/fb
cap mounts cat /proc/mounts
cap net-dev cat /proc/net/dev
# shellcheck disable=SC2016 # the sh -c bodies expand on the panel, not here
cap i2c-devices sh -c 'for d in /sys/bus/i2c/devices/*; do echo "$d $(cat "$d/name" 2>/dev/null)"; done'
# shellcheck disable=SC2016
cap sdio-devices sh -c 'for d in /sys/bus/sdio/devices/*; do echo "$d $(cat "$d/vendor" "$d/device" 2>/dev/null | tr "\n" " ")"; done'
cap platform-devices ls /sys/bus/platform/devices
cap rkwifi-chip cat /sys/class/rkwifi/chip
dump fb0-sysfs /sys/class/graphics/fb0
dump backlight-sysfs /sys/class/backlight/*
dump power-supply-sysfs /sys/class/power_supply/*
dump input-sysfs /sys/class/input/input*
dump android-usb-sysfs /sys/class/android_usb/android0
# Never dump all of usb20_otg: the DWC OTG driver's wr_reg_test attribute runs a
# register write test when read, which could drop the USB console mid-probe.
cap usb-otg-mode cat /sys/devices/platform/usb20_otg/force_usb_mode

echo "discovery done: $(find "$out" -type f | wc -l) files in $out"
