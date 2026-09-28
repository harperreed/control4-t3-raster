#!/usr/bin/env bash
# ABOUTME: Build a tt7d update bundle (ustar tar + manifest.json, optionally signed) from the current build outputs.
# ABOUTME: Upload it with the control panel's Update section or PUT /api/v1/system/update (tt7d/README.md).
#
# The bundle holds bin/tt7d, bin/tt7probe, bin/tt7-ntp-hook and app (probe/tt7-app.sh),
# plus manifest.json with each file's sha256 and mode. With --sign, manifest.sig
# holds an ed25519 signature of manifest.json's exact bytes, as 128 hex digits.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"

usage() {
  cat <<EOF
usage: tools/make-bundle.sh [options]
       tools/make-bundle.sh --pubkey-of KEY.pem

Builds build/tt7-bundle-<build>.tar from build/tt7d, build/tt7probe,
probe/tt7-ntp-hook.sh and probe/tt7-app.sh. Run \`make tt7d build/tt7probe\` first
(\`make bundle\` does both).

  --bin-dir DIR      where tt7d and tt7probe are (default build: the ARM builds;
                     the host tests use build/host)
  --build ID         release id (default: build/tt7d.version, the git describe tt7d was built with)
  --version X.Y.Z    version (default: FIRMWARE_VERSION in tt7d/main.c)
  --out FILE         output path (default build/tt7-bundle-<build>.tar)
  --sign KEY.pem     sign manifest.json with this ed25519 private key
                     (make one: openssl genpkey -algorithm ed25519 -out KEY.pem)
  --pubkey-of KEY.pem  print KEY's public key as 64 hex digits, the format of
                     <data-dir>/update-pubkey on the panel, and exit
  -h, --help
EOF
}

hex() { od -An -v -tx1 | tr -d ' \n'; }

pubkey_hex() {
  # An ed25519 SubjectPublicKeyInfo in DER is 44 bytes; the raw key is the last 32.
  openssl pkey -in "$1" -pubout -outform DER | tail -c 32 | hex
}

bin_dir="$root/build"
build=""
version=""
out=""
sign_key=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin-dir) bin_dir="$2"; shift 2 ;;
    --build) build="$2"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --sign) sign_key="$2"; shift 2 ;;
    --pubkey-of) pubkey_hex "$2"; echo; exit 0 ;;
    -h | --help) usage; exit 0 ;;
    *) echo "make-bundle: unknown option $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$build" ]]; then
  build=$(cat "$root/build/tt7d.version" 2> /dev/null || true)
  [[ -n "$build" ]] || { echo "make-bundle: no build/tt7d.version; build tt7d first or pass --build" >&2; exit 1; }
fi
if [[ -z "$version" ]]; then
  version=$(sed -n 's/^#define FIRMWARE_VERSION "\(.*\)"$/\1/p' "$root/tt7d/main.c")
fi
# The same rules tt7d applies (tt7d/bundle.c), so a bad bundle fails here first.
[[ "$build" =~ ^[A-Za-z0-9_+][A-Za-z0-9._+-]{0,63}$ ]] || { echo "make-bundle: build '$build' cannot name a release" >&2; exit 1; }
[[ "$version" =~ ^[0-9]{1,9}(\.[0-9]{1,9})*$ ]] || { echo "make-bundle: version '$version' is not dotted numbers" >&2; exit 1; }
out="${out:-$root/build/tt7-bundle-$build.tar}"

# path in the bundle, then its source
files=(
  "bin/tt7d" "$bin_dir/tt7d"
  "bin/tt7probe" "$bin_dir/tt7probe"
  "bin/tt7-ntp-hook" "$root/probe/tt7-ntp-hook.sh"
  "app" "$root/probe/tt7-app.sh"
)

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/bin"

created=$(date -u +%Y-%m-%dT%H:%M:%SZ)
entries=""
names=(manifest.json)
for ((i = 0; i < ${#files[@]}; i += 2)); do
  path="${files[i]}" src="${files[i + 1]}"
  [[ -f "$src" ]] || { echo "make-bundle: missing $src (for $path)" >&2; exit 1; }
  install -m 0755 "$src" "$stage/$path"
  sum=$(sha256sum "$stage/$path" | cut -d' ' -f1)
  entries+="${entries:+,}"$'\n'"    {\"path\": \"$path\", \"sha256\": \"$sum\", \"mode\": \"0755\"}"
  names+=("$path")
done

printf '{\n  "format": "tt7-bundle",\n  "version": "%s",\n  "build": "%s",\n  "created": "%s",\n  "files": [%s\n  ]\n}\n' \
  "$version" "$build" "$created" "$entries" > "$stage/manifest.json"

if [[ -n "$sign_key" ]]; then
  openssl pkeyutl -sign -inkey "$sign_key" -rawin -in "$stage/manifest.json" | hex > "$stage/manifest.sig"
  echo >> "$stage/manifest.sig"
  names+=(manifest.sig)
fi

# ustar, regular files only (naming files, not directories, adds no directory
# entries), root-owned, so BusyBox tar and tt7d both accept it.
mkdir -p "$(dirname "$out")"
tar --format=ustar --owner=0 --group=0 --numeric-owner -C "$stage" -cf "$out.tmp" "${names[@]}"
mv "$out.tmp" "$out"

echo "make-bundle: $out"
echo "  build $build, version $version, $(stat -c %s "$out") bytes${sign_key:+, signed (key $(pubkey_hex "$sign_key"))}"
sha256sum "$out"
