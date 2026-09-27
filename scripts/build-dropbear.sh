#!/usr/bin/env bash
# ABOUTME: Cross-build a static Dropbear multi-binary (dropbear, dropbearkey, scp) into build/dropbear.
# ABOUTME: Run via `make dropbear`. Options come from config/dropbear-localoptions.h (no password auth).

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
ver=2026.94
out="$root/build/dropbear"
: "${ZIG:?ZIG unset: run through make}"

"$root/scripts/fetch-sources.sh" dropbear
# Dropbear builds in its source tree, so start from a clean copy every time.
rm -rf "$out" "$root/build/dropbear-$ver"
tar -xjf "$root/third_party/src/dropbear-$ver.tar.bz2" -C "$root/build"
mv "$root/build/dropbear-$ver" "$out"
cp "$root/config/dropbear-localoptions.h" "$out/localoptions.h"

log="$out/build.log"
cd "$out"
# CFLAGS: -Os without autoconf's default -g. LDFLAGS: Dropbear's link line has
# no -O, and zig cc links in Debug mode (keeping debug info) unless told otherwise.
# -Wno-undef: Dropbear adds -Wundef, which fires ~1000 times inside zig's own
# musl headers (__ARMEB__, __cplusplus), burying any warning from Dropbear itself.
if ! ./configure --host=arm-linux-musleabihf \
      CC=arm-linux-musleabihf-cc AR=arm-linux-musleabihf-ar RANLIB=arm-linux-musleabihf-ranlib \
      STRIP=arm-linux-musleabihf-strip CFLAGS="-Os -Wno-undef" LDFLAGS="-Os -s" \
      --enable-static --enable-bundled-libtom --disable-zlib --disable-pam \
      --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
      --disable-loginfunc --disable-pututline --disable-pututxline > "$log" 2>&1 \
   || ! make -j"$(nproc)" PROGRAMS="dropbear dropbearkey scp" MULTI=1 >> "$log" 2>&1; then
  echo "build-dropbear: FAILED, last lines of $log:" >&2
  tail -20 "$log" >&2
  exit 1
fi
echo "build-dropbear: $out/dropbearmulti ($(stat -c %s dropbearmulti) bytes); log $log"
