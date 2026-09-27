#!/usr/bin/env bash
# ABOUTME: Cross-build static wpa_supplicant 2.10 + wpa_cli (nl80211 via libnl-tiny, internal crypto, no OpenSSL).
# ABOUTME: Run via `make wifi`; outputs build/wifi/{wpa_supplicant,wpa_cli} for the ramdisk or /data/tt7/bin.
#
# Ported from MMKeypad firmware-linux-t3/tools/build-wpa.sh (c95555d, Apache-2.0),
# which reports this exact recipe working end-to-end on an in-wall T3 (AP6330/
# BCM4330, stock 3.0.36 kernel, rkwifi.oob.ko, -D nl80211). Differences: pinned,
# hash-checked sources (libnl-tiny was an unpinned git clone), out-of-tree build
# under build/, and explicit link flags so zig links a stripped binary.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
: "${ZIG:?ZIG unset: run through make}"
nl_commit=40493a655d8caa2ccf5206dde1e733abe2920432
work="$root/build/wpa"
out="$root/build/wifi"
nl="$work/libnl-tiny-$nl_commit"
wpa="$work/wpa_supplicant-2.10"
log="$work/build.log"

"$root/scripts/fetch-sources.sh" wpa libnl-tiny
rm -rf "$work" "$out"
mkdir -p "$work" "$out"
tar -xzf "$root/third_party/src/wpa_supplicant-2.10.tar.gz" -C "$work"
tar -xzf "$root/third_party/src/libnl-tiny-$nl_commit.tar.gz" -C "$work"

fail() { echo "build-wpa: FAILED ($1), last lines of $log:" >&2; tail -20 "$log" >&2; exit 1; }

# 1) libnl-tiny, OpenWrt's minimal libnl: every top-level .c into one archive.
( cd "$nl"
  for c in *.c; do
    arm-linux-musleabihf-cc -Os -fPIC -Iinclude -c "$c" -o "${c%.c}.o"
  done
  arm-linux-musleabihf-ar rcs libnl-tiny.a ./*.o ) >> "$log" 2>&1 || fail libnl-tiny

# 2) wpa_supplicant with the nl80211 driver and internal TLS/crypto. WPA2-PSK
#    needs nothing from OpenSSL. Same .config as upstream.
cat > "$wpa/wpa_supplicant/.config" <<CFG
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL_TINY=y
CONFIG_CTRL_IFACE=y
CONFIG_BACKEND=file
CONFIG_TLS=internal
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_IEEE80211W=y
CFG
# The -Wno flags are upstream's: wpa 2.10 trips clang's stricter pointer checks.
# LDFLAGS: wpa links with bare $(CC); without -O zig keeps debug info.
make -C "$wpa/wpa_supplicant" -j"$(nproc)" \
  CC=arm-linux-musleabihf-cc AR=arm-linux-musleabihf-ar RANLIB=arm-linux-musleabihf-ranlib \
  EXTRA_CFLAGS="-Os -D_GNU_SOURCE -I$nl/include -Wno-error -Wno-incompatible-function-pointer-types -Wno-int-conversion" \
  LDFLAGS="-Os -s" LIBS="-static -L$nl -lnl-tiny" LIBS_c="-static -L$nl -lnl-tiny" \
  wpa_supplicant wpa_cli >> "$log" 2>&1 || fail wpa_supplicant

install -m 0755 "$wpa/wpa_supplicant/wpa_supplicant" "$wpa/wpa_supplicant/wpa_cli" "$out/"
echo "build-wpa: $out/wpa_supplicant ($(stat -c %s "$out/wpa_supplicant") B), $out/wpa_cli ($(stat -c %s "$out/wpa_cli") B); log $log"
