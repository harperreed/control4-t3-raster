#!/usr/bin/env bash
# ABOUTME: Read-only full NAND backup of a Control4 T3 (RK3188) in Rockchip Loader mode.
# ABOUTME: Dumps every mtdparts region plus the pre-partition head to backup/<label>/, with sha256 sums.
#
# When/why: run before ANY write to the panel. The dump is the only restore path.
# Usage:    scripts/backup-flash.sh <label>        e.g. scripts/backup-flash.sh tt7-stock
# Needs:    panel in Loader mode (2207:310b); rkdeveloptool (pine64 fork: read <sector> <BYTES> <file>).
# Output:   backup/<label>/NN_<name>.bin, parameter.txt, flash-info.txt, SHA256SUMS, backup.log

set -euo pipefail

SECTOR=512
root="$(cd "$(dirname "$0")/.." && pwd)"
label="${1:-}"
[[ -n "$label" ]] || { echo "usage: $0 <label>" >&2; exit 2; }
out="$root/backup/$label"
[[ ! -e "$out" ]] || { echo "refusing: $out already exists (backups are never overwritten)" >&2; exit 1; }

rkdeveloptool list 2>&1 | grep -q 'Pid=0x310b.*Loader' \
  || { echo "no RK3188 in Loader mode (want 2207:310b). Hold volume-up while plugging micro-USB." >&2; exit 1; }

mkdir -p "$out"
log="$out/backup.log"
exec > >(tee -a "$log") 2>&1
echo "== backup $label  $(date -Is)"

rkdeveloptool read-flash-info > "$out/flash-info.txt"
rkdeveloptool read-flash-id  >> "$out/flash-info.txt"
rkdeveloptool read-chip-info >> "$out/flash-info.txt"
total_sectors=$(awk '/Sectors/ {print $3; exit}' "$out/flash-info.txt")
[[ "$total_sectors" =~ ^[0-9]+$ ]] || { echo "could not parse flash size from flash-info.txt" >&2; exit 1; }

# The Rockchip parameter block sits at LBA 0; mtdparts in it is the authoritative map.
rkdeveloptool read 0 $((8192 * SECTOR)) "$out/.param-probe.bin" >/dev/null
strings -n 8 "$out/.param-probe.bin" | sed -n '/^FIRMWARE_VER/,/^CMDLINE/p' > "$out/parameter.txt"
rm "$out/.param-probe.bin"
mtdparts=$(grep -o 'mtdparts=[^ ]*' "$out/parameter.txt" | head -1)
[[ -n "$mtdparts" ]] || { echo "no mtdparts found in parameter block" >&2; exit 1; }
echo "$mtdparts"

# Build the region list: head (LBA 0 up to first partition), then each partition; '-' size = to end of flash.
regions=()
first_off=""
IFS=',' read -ra parts <<< "${mtdparts#*:}"
for p in "${parts[@]}"; do
  [[ "$p" =~ ^(-|0x[0-9a-fA-F]+)@(0x[0-9a-fA-F]+)\((.+)\)$ ]] || { echo "unparsed mtdparts entry: $p" >&2; exit 1; }
  size="${BASH_REMATCH[1]}"; off=$((BASH_REMATCH[2])); name="${BASH_REMATCH[3]}"
  [[ "$size" == "-" ]] && size=$((total_sectors - off)) || size=$((size))
  [[ -n "$first_off" ]] || first_off=$off
  regions+=("$name $off $size")
done
regions=("head 0 $first_off" "${regions[@]}")

i=0
for r in "${regions[@]}"; do
  read -r name off size <<< "$r"
  file=$(printf '%02d_%s.bin' "$i" "$name")
  printf '%-14s LBA %-9d sectors %-9d (%d MiB) -> %s\n' "$name" "$off" "$size" $((size * SECTOR / 1048576)) "$file"
  start=$SECONDS
  rkdeveloptool read "$off" $((size * SECTOR)) "$out/$file" >/dev/null
  got=$(stat -c %s "$out/$file")
  [[ "$got" -eq $((size * SECTOR)) ]] || { echo "SHORT READ on $name: $got bytes, want $((size * SECTOR))" >&2; exit 1; }
  echo "   ok in $((SECONDS - start))s"
  i=$((i + 1))
done

(cd "$out" && sha256sum ./*.bin > SHA256SUMS)
chmod a-w "$out"/*.bin
echo "== done $(date -Is). $(du -sh "$out" | cut -f1) in $out"
