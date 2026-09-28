#!/usr/bin/env bash
# ABOUTME: Write a boot image to the TT7's boot partition, then read it back and compare; never reboots the panel.
# ABOUTME: Loader mode over USB (rkdeveloptool, LBA 40960) or --net over ssh to a panel running our firmware (/dev/mtdblock2).
#
# When/why: M0 recovery proof and every later boot-image test. Run by hand, after
# Doctor Biz's go-ahead for the write.
#
# Loader mode: power fully off (undock, hold power), hold volume-up, plug in
# micro-USB (2207:310b). rkdeveloptool is Ubuntu's pine64 fork: `write
# <start-sector> <file>` writes the whole file; `read <start-sector> <num-BYTES>
# <file>` (bytes, not sectors).
#
# --net: the panel is up on our firmware and reachable by ssh. The image goes
# over ssh's stdin into the panel's RAM (/tmp is the initramfs; nothing lands in
# /data), is sha256-checked there, and only then dd'd to /dev/mtdblock2, the
# kernel's view of the boot partition through its own NAND driver. This follows
# MMKeypad's tools/flash.sh --net (c95555d, net_write_partition): dd bs=65536 to
# /dev/mtdblock2, sync, read back through dd and hash. Differences: we stage in
# RAM instead of /data/boot.new, hash with sha256, add conv=fsync, drop the page
# cache before each of two read-backs (otherwise the read-back just returns the
# cached copy of what we wrote), and ignore SIGHUP/SIGPIPE around dd so a USB
# gadget reset mid-write cannot kill dd and leave a half-written partition.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
BOOT_LBA=40960                  # 0xa000, mtdparts 0x00006000@0x0000a000(boot)
BOOT_BYTES=12582912             # 0x6000 sectors * 512 = 0x00c00000
DD_BS=65536                     # MMKeypad's block size; 192 blocks = BOOT_BYTES
# Per-unit stock backup (boot + parameter.txt). TT7_STOCK_DIR selects another unit's, e.g. backup/wall-000fff80e822-2026-09-28.
STOCK_DIR="${TT7_STOCK_DIR:-$root/backup/tt7-stock-2026-09-27}"
STOCK_BOOT="$STOCK_DIR/03_boot.bin"
READBACK_TRIES=3
NET_DEV=/dev/mtdblock2
NET_STAGE=/tmp/tt7-boot.new     # panel RAM (initramfs), never /data
NET_DROP_CACHES=/proc/sys/vm/drop_caches

usage() {
  cat <<EOF
usage: scripts/flash-boot.sh <image>                 Loader mode (USB): write <image>
       scripts/flash-boot.sh --restore               Loader mode (USB): write the stock boot back
       scripts/flash-boot.sh --net <host> <image>    over ssh: write <image> on a running panel
       scripts/flash-boot.sh --net <host> --restore  over ssh: write the stock boot back
       scripts/flash-boot.sh --self-test             host-only checks of this script (no device)
       scripts/flash-boot.sh --help

<host> is an ssh destination such as root@10.55.0.1. ssh uses
-o UserKnownHostsFile=build/known_hosts. The stock boot is $STOCK_BOOT.

Every mode first checks that the image is <= $BOOT_BYTES bytes, starts with
ANDROID! and has a valid embedded Rockchip SHA1 id (scripts/bootimg.py verify),
and that the backup's parameter block maps boot to $BOOT_BYTES bytes at LBA
$BOOT_LBA. It zero-pads the image to the full partition and asks you to type
'write'.

Loader mode: needs rkdeveloptool list to show Pid=0x310b ... Loader. Writes at
LBA $BOOT_LBA, then reads back up to $READBACK_TRIES times (loader reads on this NAND are
flaky) and compares byte for byte.

--net: needs /proc/mtd on the panel to show mtd2 as "boot" of size 00c00000,
and $NET_DEV not mounted. Streams the image into panel RAM, checks its sha256
there, dd's it to $NET_DEV, syncs, then reads the partition back twice (page
cache dropped each time) and compares both sha256s with the padded image. Use
the Wi-Fi address if the USB link is flaky: a USB reset mid-stream aborts the
flash harmlessly, but a reset mid-write leaves you verifying by hand.

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

check_partition_map() {
  grep -q '0x00006000@0x0000a000(boot)' "$STOCK_DIR/parameter.txt" \
    || die "$STOCK_DIR/parameter.txt does not map boot to 0x6000 sectors at 0xa000; refusing to guess"
}

# check_panel_state: reads "<proc/mtd>\n---\n<proc/mounts>" on stdin. Prints
# the problem and returns 1 unless mtd2 is "boot" of 0x00c00000 bytes and its
# block device is not mounted.
check_panel_state() {
  local text mtd mounts
  text=$(cat)
  mtd=$(sed '/^---$/,$d' <<< "$text")
  mounts=$(sed '1,/^---$/d' <<< "$text")
  if ! grep -qE '^mtd2: 00c00000 [0-9a-f]{8} "boot"$' <<< "$mtd"; then
    echo "panel /proc/mtd does not show mtd2 as \"boot\" of size 00c00000:"
    grep '^mtd2:' <<< "$mtd" || echo "  (no mtd2 line)"
    return 1
  fi
  if grep -q "^$NET_DEV " <<< "$mounts"; then
    echo "$NET_DEV is mounted on the panel"
    return 1
  fi
}

# remote_write_script <sha256> <dev> <stage> <drop_caches>: POSIX sh for the
# panel's busybox, with the padded image on stdin. Prints STAGED, WRITTEN,
# READBACK1 <sha>, READBACK2 <sha>, or STAGE_MISMATCH and exits 3.
remote_write_script() {
  local sha=$1 dev=$2 stage=$3 drop=$4 blocks=$((BOOT_BYTES / DD_BS))
  cat <<EOF
set -e
trap 'rm -f "$stage"' EXIT
cat > "$stage"
got=\$(sha256sum "$stage" | cut -d' ' -f1)
if [ "\$got" != "$sha" ]; then echo "STAGE_MISMATCH \$got"; exit 3; fi
echo STAGED
trap '' HUP PIPE
dd if="$stage" of="$dev" bs=$DD_BS conv=fsync 2>/dev/null
sync
echo WRITTEN
for i in 1 2; do
  sync
  echo 3 > "$drop"
  echo "READBACK\$i \$(dd if="$dev" bs=$DD_BS count=$blocks 2>/dev/null | sha256sum | cut -d' ' -f1)"
done
EOF
}

# prepare <image>: validate, check the map, pad into a fresh build/flash-* dir.
# Sets $padded and $sha.
prepare() {
  local img=$1 work
  validate_image "$img" || die "refusing to write $img"
  check_partition_map
  work="$root/build/flash-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$work"
  padded="$work/write.img"
  pad_image "$img" "$padded"
  sha=$(sha256sum "$padded" | cut -d' ' -f1)
  echo "image:   $img"
  echo "padded:  $padded ($BOOT_BYTES bytes, sha256 $sha)"
}

confirm() {
  local answer
  read -r -p "Type 'write' to write it: " answer
  [[ "$answer" == write ]] || die "not confirmed; nothing written"
}

self_test() {
  local tmp fails=0 img
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' RETURN
  img="$root/build/tt7-probe-boot.img"
  [[ -f "$img" ]] || die "self-test needs $img (run make image)"

  # Fake ssh and rkdeveloptool that record any call and fail: the self-test
  # must never reach a panel, whatever the code under test does.
  mkdir -p "$tmp/bin"
  for tool in ssh rkdeveloptool; do
    printf '#!/bin/sh\necho "%s $*" >> "%s/calls"\necho "self-test: unexpected %s call" >&2\nexit 99\n' \
      "$tool" "$tmp" "$tool" > "$tmp/bin/$tool"
    chmod +x "$tmp/bin/$tool"
  done
  rkdeveloptool() { echo "rkdeveloptool $*" >> "$tmp/calls"; return 99; }
  ssh() { echo "ssh $*" >> "$tmp/calls"; return 99; }
  export PATH="$tmp/bin:$PATH"  # the script exits after the self-test

  check() { # check <description> <command...>: command must succeed
    local d=$1; shift
    if "$@" > "$tmp/out" 2>&1; then echo "  ok   $d"; else echo "  FAIL $d"; sed 's/^/       /' "$tmp/out"; fails=$((fails + 1)); fi
  }
  refuses() { # refuses <description> <command...>: command must fail
    local d=$1; shift
    if "$@" > "$tmp/out" 2>&1; then echo "  FAIL $d (accepted)"; fails=$((fails + 1)); else echo "  ok   $d"; fi
  }

  # Image validation.
  check "built image validates" validate_image "$img"
  check "stock boot validates" validate_image "$STOCK_BOOT"
  cp "$img" "$tmp/flipped"; printf '\xff' | dd of="$tmp/flipped" bs=1 seek=20000 conv=notrunc status=none
  refuses "one flipped kernel byte is refused (id mismatch)" validate_image "$tmp/flipped"
  cp "$img" "$tmp/nomagic"; printf 'XNDROID!' | dd of="$tmp/nomagic" bs=1 seek=0 conv=notrunc status=none
  refuses "missing ANDROID! magic is refused" validate_image "$tmp/nomagic"
  cp "$img" "$tmp/big"; truncate -s $((BOOT_BYTES + 512)) "$tmp/big"
  refuses "image over $BOOT_BYTES bytes is refused" validate_image "$tmp/big"
  head -c 1048576 "$img" > "$tmp/short"
  refuses "truncated image is refused" validate_image "$tmp/short"

  head -c 9977856 "$img" > "$tmp/unpadded"   # header + kernel pages only
  pad_image "$tmp/unpadded" "$tmp/padded"
  if [[ $(stat -c %s "$tmp/padded") -eq $BOOT_BYTES ]] && cmp -s -n 9977856 "$tmp/unpadded" "$tmp/padded" \
     && [[ -z $(tail -c +9977857 "$tmp/padded" | tr -d '\0' | head -c 1) ]]; then
    echo "  ok   padding keeps the prefix and zero-fills to $BOOT_BYTES bytes"
  else
    echo "  FAIL padding"; fails=$((fails + 1))
  fi

  # Panel state checks, on /proc/mtd text as this unit reports it.
  local good_mtd='dev:    size   erasesize  name
mtd0: 00400000 00004000 "misc"
mtd1: 00c00000 00004000 "kernel"
mtd2: 00c00000 00004000 "boot"
mtd3: 02000000 00004000 "recovery"'
  check "mtd2 \"boot\" 00c00000, not mounted: accepted" check_panel_state <<< "$good_mtd"$'\n---\n'"/dev/mtdblock6 /data ext4 rw 0 0"
  refuses "mtd2 of the wrong size is refused" check_panel_state <<< "${good_mtd/mtd2: 00c00000/mtd2: 00800000}"$'\n---\n'
  refuses "mtd2 with another name is refused" check_panel_state <<< "${good_mtd/\"boot\"/\"kernel\"}"$'\n---\n'
  refuses "missing mtd2 is refused" check_panel_state <<< "${good_mtd/mtd2:/mtdX:}"$'\n---\n'
  refuses "mounted $NET_DEV is refused" check_panel_state <<< "$good_mtd"$'\n---\n'"$NET_DEV /boot ext4 ro 0 0"

  # The exact script the panel runs, run here by sh against temp files.
  local psha
  psha=$(sha256sum "$tmp/padded" | cut -d' ' -f1)
  : > "$tmp/drop"
  head -c "$BOOT_BYTES" /dev/zero | tr '\0' '\377' > "$tmp/dev"
  remote_write_script "$psha" "$tmp/dev" "$tmp/stage" "$tmp/drop" > "$tmp/remote.sh"
  sh "$tmp/remote.sh" < "$tmp/padded" > "$tmp/rout" 2>&1 || true
  if cmp -s "$tmp/dev" "$tmp/padded" && grep -qx "READBACK1 $psha" "$tmp/rout" && grep -qx "READBACK2 $psha" "$tmp/rout" \
     && [[ ! -e "$tmp/stage" ]] && [[ $(cat "$tmp/drop") == 3 ]]; then
    echo "  ok   remote script writes the image, reports two matching read-backs, removes its staging copy"
  else
    echo "  FAIL remote write script"; sed 's/^/       /' "$tmp/rout"; fails=$((fails + 1))
  fi
  head -c "$BOOT_BYTES" /dev/zero | tr '\0' '\377' > "$tmp/dev"
  head -c 5000000 "$tmp/padded" | sh "$tmp/remote.sh" > "$tmp/rout" 2>&1 && rc=0 || rc=$?
  if [[ $rc -eq 3 ]] && grep -q '^STAGE_MISMATCH' "$tmp/rout" && [[ -z $(tr -d '\377' < "$tmp/dev" | head -c 1) ]] \
     && [[ ! -e "$tmp/stage" ]]; then
    echo "  ok   a truncated stream is caught in staging and the device is untouched"
  else
    echo "  FAIL truncated stream (rc $rc)"; sed 's/^/       /' "$tmp/rout"; fails=$((fails + 1))
  fi

  # Whole-script paths that must stop before any device or ssh contact.
  refuses "--net with a bad image stops before ssh" "$0" --net root@192.0.2.1 "$tmp/flipped"
  refuses "loader mode with a bad image stops before rkdeveloptool" "$0" "$tmp/nomagic"
  refuses "--net without an image is a usage error" "$0" --net root@192.0.2.1
  refuses "unknown option is a usage error" "$0" --bogus
  if [[ -s "$tmp/calls" ]]; then
    echo "  FAIL ssh/rkdeveloptool were called:"; sed 's/^/       /' "$tmp/calls"; fails=$((fails + 1))
  else
    echo "  ok   no ssh or rkdeveloptool call during the self-test"
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

flash_loader() {
  local img=$1 label=$2 try padded sha work
  echo "== flash-boot: $label (Loader mode)"
  prepare "$img"
  work=$(dirname "$padded")
  require_loader
  echo "target:  boot partition, LBA $BOOT_LBA, $((BOOT_BYTES / 512)) sectors"
  confirm

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

flash_net() {
  local host=$1 img=$2 label=$3 padded sha out r1 r2 script
  local ssh_opts=(-o UserKnownHostsFile="$root/build/known_hosts" -o ConnectTimeout=10
                  -o ServerAliveInterval=10 -o ServerAliveCountMax=3)
  echo "== flash-boot: $label (over ssh to $host)"
  prepare "$img"

  ssh -n "${ssh_opts[@]}" "$host" 'cat /proc/mtd; echo ---; cat /proc/mounts' > "$(dirname "$padded")/panel-state.txt" \
    || die "cannot read /proc/mtd over ssh from $host"
  check_panel_state < "$(dirname "$padded")/panel-state.txt" || die "refusing: the panel's partition table is not what this script expects"
  echo "target:  $NET_DEV on $host (mtd2 \"boot\", 00c00000 bytes)"
  confirm

  script=$(remote_write_script "$sha" "$NET_DEV" "$NET_STAGE" "$NET_DROP_CACHES")
  printf '%s\n' "$script" > "$(dirname "$padded")/remote.sh"   # a record of exactly what ran
  # shellcheck disable=SC2029 # the script is built here on purpose; it holds no secrets
  out=$(ssh "${ssh_opts[@]}" "$host" "$script" < "$padded") || true
  printf '%s\n' "$out" > "$(dirname "$padded")/remote-output.txt"

  if grep -q '^STAGE_MISMATCH' <<< "$out"; then
    die "the image arrived corrupted in panel RAM; NOTHING was written. Re-run (Wi-Fi if USB is flaky)."
  fi
  if ! grep -q '^STAGED$' <<< "$out"; then
    die "the transfer did not complete; NOTHING was written (the panel only writes after the sha256 check). Re-run."
  fi
  if ! grep -q '^WRITTEN$' <<< "$out"; then
    echo "flash-boot: THE LINK DROPPED DURING THE WRITE. DO NOT REBOOT THE PANEL." >&2
    echo "  dd ignores the hangup, so it most likely completed. Check when the panel is back:" >&2
    echo "    ssh -o UserKnownHostsFile=build/known_hosts $host 'sync; echo 3 > $NET_DROP_CACHES; dd if=$NET_DEV bs=$DD_BS count=$((BOOT_BYTES / DD_BS)) 2>/dev/null | sha256sum'" >&2
    echo "  and compare with $sha. If it differs, re-run this script." >&2
    exit 1
  fi
  r1=$(sed -n 's/^READBACK1 //p' <<< "$out")
  r2=$(sed -n 's/^READBACK2 //p' <<< "$out")
  if [[ "$r1" == "$sha" && "$r2" == "$sha" ]]; then
    echo "written; both read-backs of $NET_DEV match (sha256 $sha)"
    echo
    echo "The panel is still running the old image. To boot the new one:"
    echo "    ssh -o UserKnownHostsFile=build/known_hosts $host 'reboot -f'"
    echo "(plain 'reboot' does nothing here: our PID 1 has no signal handlers, and"
    echo " busybox reboot without -f only signals PID 1. -f syncs, then reboots directly.)"
    return 0
  fi
  echo "flash-boot: READ-BACK MISMATCH. DO NOT REBOOT THE PANEL." >&2
  echo "  want      $sha" >&2
  echo "  readback1 ${r1:-(none)}" >&2
  echo "  readback2 ${r2:-(none)}" >&2
  echo "  Re-run this script, or restore stock: scripts/flash-boot.sh --net $host --restore" >&2
  exit 1
}

case "${1:-}" in
  -h | --help) usage ;;
  --self-test) [[ $# -eq 1 ]] || { usage >&2; exit 2; }; self_test ;;
  --restore) [[ $# -eq 1 ]] || { usage >&2; exit 2; }; flash_loader "$STOCK_BOOT" "restore stock boot" ;;
  --net)
    [[ $# -eq 3 ]] || { usage >&2; exit 2; }
    if [[ "$3" == --restore ]]; then
      flash_net "$2" "$STOCK_BOOT" "restore stock boot"
    else
      [[ "$3" != -* ]] || { usage >&2; exit 2; }
      flash_net "$2" "$3" "write $3"
    fi
    ;;
  "" | -*) usage >&2; exit 2 ;;
  *) [[ $# -eq 1 ]] || { usage >&2; exit 2; }; flash_loader "$1" "write $1" ;;
esac
