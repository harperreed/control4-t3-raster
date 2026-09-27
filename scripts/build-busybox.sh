#!/usr/bin/env bash
# ABOUTME: Cross-build static BusyBox 1.36.1 for the TT7 into build/busybox from config/busybox.config.
# ABOUTME: Run via `make busybox` (needs ZIG and toolchain/ on PATH, which the Makefile sets).
#
# config/busybox.config is `make defconfig` for 1.36.1 with three changes:
#   CONFIG_STATIC=y                              no shared libs on the panel
#   CONFIG_EXTRA_CFLAGS="-fno-strict-aliasing"   clang (zig cc) vs BusyBox's type punning
#   CONFIG_TC unset                              tc.c needs CBQ symbols that modern kernel headers dropped
#
# awk.c is also built at -O0 without UBSan, per MMKeypad's tools/build-busybox.sh
# (c95555d): at -Os, zig's clang miscompiles awk.c's undefined behaviour and
# `echo "a b" | awk '{print $1}'` segfaults (seen on their hardware).

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
ver=1.36.1
src="$root/build/src/busybox-$ver"
out="$root/build/busybox"
: "${ZIG:?ZIG unset: run through make}"

"$root/scripts/fetch-sources.sh" busybox
if [[ ! -d "$src" ]]; then
  mkdir -p "$root/build/src"
  tar -xjf "$root/third_party/src/busybox-$ver.tar.bz2" -C "$root/build/src"
  printf '\nCFLAGS_awk.o := -O0 -fno-sanitize=undefined\n' >> "$src/editors/Kbuild.src"
fi

mkdir -p "$out"
cp "$root/config/busybox.config" "$out/.config"
mk=(make -C "$src" O="$out" CROSS_COMPILE=arm-linux-musleabihf- HOSTCC=/usr/bin/gcc)

# The committed config must already be complete: oldconfig may not change it.
# (`yes` dies of SIGPIPE when oldconfig exits; `|| true` keeps pipefail quiet about it.)
{ yes "" || true; } | "${mk[@]}" oldconfig > "$out/oldconfig.log" 2>&1
if ! diff <(grep -v '^#' "$root/config/busybox.config") <(grep -v '^#' "$out/.config") > "$out/config.diff"; then
  echo "build-busybox: oldconfig changed config/busybox.config; see $out/config.diff" >&2
  exit 1
fi

log="$out/build.log"
if ! "${mk[@]}" -j"$(nproc)" busybox busybox.links > "$log" 2>&1; then
  echo "build-busybox: FAILED, last lines of $log:" >&2
  tail -20 "$log" >&2
  exit 1
fi
echo "build-busybox: $out/busybox ($(stat -c %s "$out/busybox") bytes), $(wc -l < "$out/busybox.links") applet links; log $log"
