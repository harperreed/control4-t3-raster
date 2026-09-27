#!/usr/bin/env bash
# ABOUTME: Download the pinned third-party source tarballs into third_party/src and check their sha256.
# ABOUTME: Usage: scripts/fetch-sources.sh [busybox|dropbear]...  (no args = all). Safe to re-run.
#
# Pins: change a version here and nowhere else. Hashes were checked against
# busybox.net's .sha256 file and matt.ucc.asn.au's SHA256SUM.asc (2026-09-27;
# the .asc signature itself was not GPG-verified).

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="$root/third_party/src"

declare -A URL SHA
URL[busybox]="https://busybox.net/downloads/busybox-1.36.1.tar.bz2"
SHA[busybox]="b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314"
URL[dropbear]="https://matt.ucc.asn.au/dropbear/releases/dropbear-2026.94.tar.bz2"
SHA[dropbear]="e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81"

names=("$@")
[[ ${#names[@]} -gt 0 ]] || names=(busybox dropbear)
mkdir -p "$dest"

for name in "${names[@]}"; do
  [[ -n "${URL[$name]:-}" ]] || { echo "fetch-sources: unknown source '$name' (have: ${!URL[*]})" >&2; exit 2; }
  file="$dest/$(basename "${URL[$name]}")"
  if [[ ! -f "$file" ]]; then
    echo "fetch-sources: downloading ${URL[$name]}"
    curl -fsSL --retry 3 -o "$file.part" "${URL[$name]}"
    mv "$file.part" "$file"
  fi
  got=$(sha256sum "$file" | cut -d' ' -f1)
  if [[ "$got" != "${SHA[$name]}" ]]; then
    echo "fetch-sources: SHA256 MISMATCH for $file" >&2
    echo "  want ${SHA[$name]}" >&2
    echo "  got  $got" >&2
    echo "  delete the file to re-download, or fix the pin if the version changed on purpose" >&2
    exit 1
  fi
  echo "fetch-sources: ok $(basename "$file") sha256 $got"
done
