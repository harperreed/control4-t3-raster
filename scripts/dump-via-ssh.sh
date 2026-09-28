#!/usr/bin/env bash
# ABOUTME: Full NAND dump of a TT7 running our firmware, read through the kernel's own mtd/FTL driver over SSH.
# ABOUTME: Reads in 64 MiB chunks, each read twice (streamed to host, hashed on panel) and compared; resumable.
#
# When/why: Loader-mode reads of this NAND are flaky (gotchas.md); the kernel driver is what Android uses,
# so this is the trustworthy stock backup. The panel's USB gadget can reset mid-transfer, so every chunk is
# retried until the link is back, and re-running with the same label resumes where it stopped.
# Mounted read-write partitions (userdata) can legitimately change between reads; their chunk mismatches
# are reported as MOUNTED-RW rather than failures.
# Usage: scripts/dump-via-ssh.sh <label> [partition...]   -> backup/<label>/NN_<name>.bin, SHA256SUMS, dump.log
#        With partition names, only those are dumped (in /proc/mtd order); the rest can be resumed later.
# Env:   TT7_HOST (default root@10.55.0.1), TT7_DUMP_PAUSE (seconds between chunks, default 0)

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
label="${1:-}"; [[ -n "$label" ]] || { echo "usage: $0 <label> [partition...]" >&2; exit 2; }
shift
only=" $* "
host="${TT7_HOST:-root@10.55.0.1}"
pause="${TT7_DUMP_PAUSE:-0}"
out="$root/backup/$label"
CHUNK_MB=64
# -n: never read stdin. The partition loop below feeds /proc/mtd on stdin, and ssh would swallow it.
ssh_() { ssh -n -o UserKnownHostsFile="$root/build/known_hosts" -o ConnectTimeout=10 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 "$host" "$@"; }

wait_for_panel() {
  local waited=0
  until ssh_ true 2>/dev/null; do
    (( waited % 30 == 0 )) && echo "   panel unreachable, waiting (${waited}s)"
    sleep 5; waited=$((waited + 5))
    (( waited < 900 )) || { echo "   gave up waiting for panel after 15 min"; exit 1; }
  done
}

mkdir -p "$out"
exec > >(tee -a "$out/dump.log") 2>&1
echo "== kernel-side dump $label from $host  $(date -Is)"
wait_for_panel
[[ -f "$out/proc-mtd.txt" ]] || ssh_ 'cat /proc/mtd' > "$out/proc-mtd.txt"
rw_mounted=$(ssh_ "mount | awk '/mtdblock/ && /[(,]rw[,)]/ {print \$1}' | sort -u")

bad=0
dumped=0
while read -r dev size _ name; do
  [[ "$dev" =~ ^mtd([0-9]+):$ ]] || continue
  n="${BASH_REMATCH[1]}"; name="${name//\"/}"; bytes=$((16#$size))
  [[ "$only" == "  " || "$only" == *" $name "* ]] || continue
  file=$(printf '%02d_%s.bin' $((n + 1)) "$name")   # 00_ is the loader-only head region
  status="$out/$file.status"
  if [[ -f "$status" ]]; then echo "$name: already done ($(cat "$status"))"; dumped=$((dumped + 1)); continue; fi
  chunks=$(( (bytes + CHUNK_MB * 1048576 - 1) / (CHUNK_MB * 1048576) ))
  have=0; [[ -f "$out/$file" ]] && have=$(( $(stat -c %s "$out/$file") / (CHUNK_MB * 1048576) ))
  truncate -s $((have * CHUNK_MB * 1048576)) "$out/$file" 2>/dev/null || : > "$out/$file"
  printf '%-10s mtdblock%-2s %6d MiB, %d chunks, resuming at %d\n' "$name" "$n" $((bytes / 1048576)) "$chunks" "$have"
  rw=0; grep -qx "/dev/mtdblock$n" <<< "$rw_mounted" && rw=1
  differ=0; start=$SECONDS
  for (( c = have; c < chunks; c++ )); do
    rd="dd if=/dev/mtdblock$n bs=1M skip=$((c * CHUNK_MB)) count=$CHUNK_MB 2>/dev/null"
    for try in 1 2 3 4 5; do
      if ssh_ "$rd" > "$out/.chunk" && panel_sum=$(ssh_ "$rd | sha256sum" | cut -d' ' -f1) && [[ -n "$panel_sum" ]]; then
        host_sum=$(sha256sum "$out/.chunk" | cut -d' ' -f1)
        if [[ "$host_sum" == "$panel_sum" ]] || (( rw )); then break; fi
        echo "   chunk $c: reads differ (try $try)"
      else
        echo "   chunk $c: transfer failed (try $try)"; wait_for_panel
      fi
      (( try < 5 )) || { echo "   chunk $c: FAILED after 5 tries"; bad=1; continue 3; }
    done
    [[ "$host_sum" == "$panel_sum" ]] || differ=$((differ + 1))
    cat "$out/.chunk" >> "$out/$file"
    (( pause > 0 )) && sleep "$pause"
  done
  rm -f "$out/.chunk"
  got=$(stat -c %s "$out/$file")
  [[ "$got" -eq "$bytes" ]] || { echo "   SHORT: $got of $bytes bytes"; bad=1; continue; }
  if (( differ == 0 )); then
    echo "   ok, every chunk read twice and agreed ($((SECONDS - start))s)" | tee /dev/stderr | sed 's/^ *//' > "$status"
  else
    echo "   MOUNTED-RW: $differ chunk(s) changed between reads, expected while mounted ($((SECONDS - start))s)" | tee /dev/stderr | sed 's/^ *//' > "$status"
  fi
  dumped=$((dumped + 1))
done < <(tail -n +2 "$out/proc-mtd.txt")

if [[ "$only" == "  " ]]; then expected=$(grep -c "^mtd" "$out/proc-mtd.txt"); else read -ra wanted <<< "$only"; expected=${#wanted[@]}; fi
[[ "$dumped" -eq "$expected" ]] || { echo "INCOMPLETE: dumped $dumped of $expected partitions"; bad=1; }

if (( bad == 0 )); then
  (cd "$out" && sha256sum ./*.bin > SHA256SUMS)
  for s in "$out"/*.status; do chmod a-w "${s%.status}"; done
fi
echo "== done $(date -Is), exit $bad. $(du -sh "$out" | cut -f1) in $out"
exit $bad
