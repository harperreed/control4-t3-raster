#!/usr/bin/env bash
# ABOUTME: Full NAND dump of a TT7 running our firmware, read through the kernel's own mtd/FTL driver over SSH.
# ABOUTME: Each partition is read twice (stream to host, then hashed on the panel) and the two hashes compared.
#
# When/why: Loader-mode reads of this NAND are flaky (gotchas.md); the kernel driver is what Android uses,
# so this is the trustworthy stock backup. Mounted read-write partitions (userdata) can legitimately
# change between the two reads; those are reported as MOUNTED-RW rather than failures.
# Usage: scripts/dump-via-ssh.sh <label>      -> backup/<label>/NN_<name>.bin, SHA256SUMS, dump.log
# Env:   TT7_HOST (default root@10.55.0.1)

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
label="${1:-}"; [[ -n "$label" ]] || { echo "usage: $0 <label>" >&2; exit 2; }
host="${TT7_HOST:-root@10.55.0.1}"
out="$root/backup/$label"
[[ ! -e "$out" ]] || { echo "refusing: $out already exists (backups are never overwritten)" >&2; exit 1; }
# -n: never read stdin. The partition loop below feeds /proc/mtd on stdin, and ssh would swallow it.
ssh_() { ssh -n -o UserKnownHostsFile="$root/build/known_hosts" -o ServerAliveInterval=15 "$host" "$@"; }

mkdir -p "$out"
exec > >(tee -a "$out/dump.log") 2>&1
echo "== kernel-side dump $label from $host  $(date -Is)"
ssh_ 'cat /proc/mtd' > "$out/proc-mtd.txt"
rw_mounted=$(ssh_ "mount | awk '/mtdblock/ && /[(,]rw[,)]/ {print \$1}' | sort -u")

bad=0
dumped=0
while read -r dev size _ name; do
  [[ "$dev" =~ ^mtd([0-9]+):$ ]] || continue
  n="${BASH_REMATCH[1]}"; name="${name//\"/}"; bytes=$((16#$size))
  file=$(printf '%02d_%s.bin' $((n + 1)) "$name")   # 00_ is the loader-only head region
  printf '%-10s mtdblock%-2s %6d MiB -> %s\n' "$name" "$n" $((bytes / 1048576)) "$file"
  start=$SECONDS
  ssh_ "dd if=/dev/mtdblock$n bs=1M 2>/dev/null" > "$out/$file"
  got=$(stat -c %s "$out/$file")
  [[ "$got" -eq "$bytes" ]] || { echo "   SHORT: $got of $bytes bytes"; bad=1; continue; }
  host_sum=$(sha256sum "$out/$file" | cut -d' ' -f1)
  panel_sum=$(ssh_ "dd if=/dev/mtdblock$n bs=1M 2>/dev/null | sha256sum" | cut -d' ' -f1)
  if [[ "$host_sum" == "$panel_sum" ]]; then
    dumped=$((dumped + 1))
    echo "   ok, two reads agree ($((SECONDS - start))s) $host_sum"
  elif grep -qx "/dev/mtdblock$n" <<< "$rw_mounted"; then
    dumped=$((dumped + 1))
    echo "   MOUNTED-RW: reads differ, expected while mounted ($((SECONDS - start))s)"
  else
    echo "   MISMATCH: host $host_sum panel $panel_sum"; bad=1
  fi
done < <(tail -n +2 "$out/proc-mtd.txt")

expected=$(grep -c "^mtd" "$out/proc-mtd.txt")
[[ "$dumped" -eq "$expected" ]] || { echo "INCOMPLETE: dumped $dumped of $expected partitions"; bad=1; }

(cd "$out" && sha256sum ./*.bin > SHA256SUMS)
chmod a-w "$out"/*.bin
echo "== done $(date -Is). $(du -sh "$out" | cut -f1) in $out"
exit $bad
