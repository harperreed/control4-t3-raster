#!/usr/bin/env bash
# ABOUTME: Write a boot image to the TT7's boot partition (LBA 40960) in Loader mode, then read it back and compare.
# ABOUTME: `flash-boot.sh <image>` flashes ours, `--restore` puts the stock boot back; neither reboots the panel.
#
# When/why: M0 recovery proof and every later boot-image test. Run by hand, after
# Doctor Biz's go-ahead for the write. Needs the panel in Loader mode (2207:310b):
# power fully off (undock, hold power), hold volume-up, plug in micro-USB.
#
# rkdeveloptool is Ubuntu's pine64 fork: `write <start-sector> <file>` writes the
# whole file; `read <start-sector> <num-BYTES> <file>` (bytes, not sectors).

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
BOOT_LBA=40960                  # 0xa000, mtdparts 0x00006000@0x0000a000(boot)
BOOT_BYTES=12582912             # 0x6000 sectors * 512
STOCK_DIR="$root/backup/tt7-stock-2026-09-27"
STOCK_BOOT="$STOCK_DIR/03_boot.bin"
READBACK_TRIES=3

usage() {
  cat <<EOF
usage: scripts/flash-boot.sh <image>     write <image> to the boot partition
       scripts/flash-boot.sh --restore   write the stock boot back ($STOCK_BOOT)
       scripts/flash-boot.sh --self-test check this script's image validation (no device)
       scripts/flash-boot.sh --help

Before writing it checks that:
  - the panel is in Loader mode (rkdeveloptool list shows Pid=0x310b ... Loader)
  - the image is <= $BOOT_BYTES bytes, starts with ANDROID!, and its embedded
    Rockchip SHA1 id is valid (scripts/bootimg.py verify)
  - the backup's parameter block still maps boot to ${BOOT_BYTES} bytes at LBA $BOOT_LBA
Then it zero-pads the image to the full partition, asks you to type 'write',
writes it at LBA $BOOT_LBA, reads the partition back (up to $READBACK_TRIES tries,
because loader reads on this NAND are flaky) and compares byte for byte.
It never reboots the panel; it prints the reboot command for you.
EOF
}

die() { echo "flash-boot: $*" >&2; exit 1; }

# validate_image <file>: every check that needs no device. Prints why on failure.
validate_image() {
  local img=$1 size magic
  [[ -f "$img" ]] || { echo "not a file: $img"; return 1; }
  size=$(stat -c %s "$img")
  (( size <= BOOT_BYTES )) || { echo "$img is $size bytes, over the $BOOT_BYTES-byte boot partition"; return 1; }
  magic=$(head -c 8 "$img")
  [[ "$magic" == "ANDROID!" ]] || { echo "$img has no ANDROID! magic"; return 1; }
  python3 "$root/scripts/bootimg.py" verify "$img" || { echo "$img: embedded id or layout is invalid"; return 1; }
}

# pad_image <in> <out>: copy <in> and zero-fill to exactly BOOT_BYTES, so the
# write replaces the whole partition and the read-back compares like for like.
pad_image() {
  cp "$1" "$2"
  truncate -s "$BOOT_BYTES" "$2"
}

self_test() {
  # The self-test must never reach the panel, whatever goes wrong below.
  rkdeveloptool() { echo "flash-boot self-test tried to run rkdeveloptool $*" >&2; exit 99; }
  local tmp img fails=0
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' RETURN
  img="$root/build/tt7-probe-boot.img"
  [[ -f "$img" ]] || die "self-test needs $img (run make image)"

  expect() { # expect <pass|fail> <description> <file>
    if validate_image "$3" > "$tmp/out" 2>&1; then got=pass; else got=fail; fi
    if [[ "$got" == "$1" ]]; then echo "  ok   $2"; else echo "  FAIL $2 (got $got)"; cat "$tmp/out"; fails=$((fails + 1)); fi
  }
  expect pass "built image validates" "$img"
  expect pass "stock boot validates" "$STOCK_BOOT"
  cp "$img" "$tmp/flipped"; printf '\xff' | dd of="$tmp/flipped" bs=1 seek=20000 conv=notrunc status=none
  expect fail "one flipped kernel byte is refused (id mismatch)" "$tmp/flipped"
  cp "$img" "$tmp/nomagic"; printf 'XNDROID!' | dd of="$tmp/nomagic" bs=1 seek=0 conv=notrunc status=none
  expect fail "missing ANDROID! magic is refused" "$tmp/nomagic"
  cp "$img" "$tmp/big"; truncate -s $((BOOT_BYTES + 512)) "$tmp/big"
  expect fail "image over $BOOT_BYTES bytes is refused" "$tmp/big"
  head -c 1048576 "$img" > "$tmp/short"
  expect fail "truncated image is refused" "$tmp/short"

  head -c 9977856 "$img" > "$tmp/unpadded"   # header + kernel pages only
  pad_image "$tmp/unpadded" "$tmp/padded"
  if [[ $(stat -c %s "$tmp/padded") -eq $BOOT_BYTES ]] && cmp -s -n 9977856 "$tmp/unpadded" "$tmp/padded" \
     && [[ -z $(tail -c +9977857 "$tmp/padded" | tr -d '\0' | head -c 1) ]]; then
    echo "  ok   padding keeps the prefix and zero-fills to $BOOT_BYTES bytes"
  else
    echo "  FAIL padding"; fails=$((fails + 1))
  fi
  (( fails == 0 )) || die "self-test: $fails check(s) failed"
  echo "flash-boot self-test: all passed (no device used)"
}

require_loader() {
  local list
  command -v rkdeveloptool >/dev/null || die "rkdeveloptool not installed (apt install rkdeveloptool)"
  list=$(rkdeveloptool list 2>&1) || true
  echo "$list"
  [[ $(grep -c 'Pid=0x310b.*Loader' <<< "$list") -eq 1 ]] \
    || die "want exactly one RK3188 in Loader mode (2207:310b). Power off fully, hold volume-up, plug micro-USB."
}

check_partition_map() {
  grep -q '0x00006000@0x0000a000(boot)' "$STOCK_DIR/parameter.txt" \
    || die "$STOCK_DIR/parameter.txt does not map boot to 0x6000 sectors at 0xa000; refusing to guess"
}

flash() {
  local img=$1 label=$2 work padded sha try
  echo "== flash-boot: $label"
  validate_image "$img" || die "refusing to write $img"
  check_partition_map
  require_loader

  work="$root/build/flash-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$work"
  padded="$work/write.img"
  pad_image "$img" "$padded"
  sha=$(sha256sum "$padded" | cut -d' ' -f1)
  echo "image:   $img"
  echo "padded:  $padded ($BOOT_BYTES bytes, sha256 $sha)"
  echo "target:  boot partition, LBA $BOOT_LBA, $((BOOT_BYTES / 512)) sectors"
  read -r -p "Type 'write' to write it: " answer
  [[ "$answer" == write ]] || die "not confirmed; nothing written"

  rkdeveloptool write "$BOOT_LBA" "$padded" || die "rkdeveloptool write FAILED. The partition may be partly written: do NOT reboot; retry, or run --restore."

  for ((try = 1; try <= READBACK_TRIES; try++)); do
    rm -f "$work/readback-$try.img"
    if rkdeveloptool read "$BOOT_LBA" "$BOOT_BYTES" "$work/readback-$try.img" \
       && cmp -s "$padded" "$work/readback-$try.img"; then
      echo "read-back $try/$READBACK_TRIES matches (sha256 $sha)"
      echo
      echo "Written and verified. The panel is still in Loader mode. To boot it:"
      echo "    rkdeveloptool reboot"
      return 0
    fi
    echo "read-back $try/$READBACK_TRIES does NOT match ($(cmp "$padded" "$work/readback-$try.img" 2>&1 | head -1))" >&2
  done
  echo >&2
  echo "flash-boot: READ-BACK FAILED $READBACK_TRIES TIMES. DO NOT REBOOT THE PANEL." >&2
  echo "  Loader reads on this NAND are known to be flaky (gotchas.md), so the write may be fine," >&2
  echo "  but nothing proves it. Read-backs are kept in $work for comparison." >&2
  echo "  Next: re-run this script to write again, or run: scripts/flash-boot.sh --restore" >&2
  exit 1
}

case "${1:-}" in
  -h | --help) usage ;;
  --self-test) [[ $# -eq 1 ]] || { usage >&2; exit 2; }; self_test ;;
  --restore) [[ $# -eq 1 ]] || { usage >&2; exit 2; }; flash "$STOCK_BOOT" "restore stock boot" ;;
  "" | -*) usage >&2; exit 2 ;;
  *) [[ $# -eq 1 ]] || { usage >&2; exit 2; }; flash "$1" "write $1" ;;
esac
