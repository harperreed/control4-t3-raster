#!/usr/bin/env bash
# ABOUTME: Push a PNG to a TT7 running tt7d: PUT /api/v1/frame with the bearer token, print the JSON reply.
# ABOUTME: Usage: tools/push-frame.sh <host[:port]> <png> [--persist] [--id ID]. Exits 1 on any non-2xx reply.

set -euo pipefail

usage() {
  cat <<'EOF'
usage: tools/push-frame.sh <host[:port]> <png> [--persist] [--id ID]

  --persist   ask tt7d to keep this frame across reboots (X-Persist: true)
  --id ID     frame id (X-Frame-ID); tt7d generates one if omitted

The token comes from $TT7_TOKEN_FILE (default ~/.config/tt7/token). Fetch it
once from the panel (tt7d creates it on first start):

  mkdir -p ~/.config/tt7
  ssh root@<panel> cat /data/tt7/tt7d/token > ~/.config/tt7/token
  chmod 600 ~/.config/tt7/token

The token is passed to curl on stdin, so it never shows up in `ps`.
EOF
}

die() { echo "push-frame: $*" >&2; exit 1; }

[[ $# -ge 1 && ( $1 == -h || $1 == --help ) ]] && { usage; exit 0; }
[[ $# -ge 2 ]] || { usage >&2; exit 2; }
host=$1
png=$2
shift 2
persist=false
id=
while [[ $# -gt 0 ]]; do
  case $1 in
    --persist) persist=true ;;
    --id) [[ $# -ge 2 ]] || die "--id needs a value"; id=$2; shift ;;
    -h | --help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
  shift
done

[[ -r $png ]] || die "cannot read $png"
token_file=${TT7_TOKEN_FILE:-$HOME/.config/tt7/token}
[[ -r $token_file ]] || die "no token file at $token_file (see --help for how to fetch it)"
token=$(tr -d ' \r\n' < "$token_file")
[[ -n $token ]] || die "$token_file is empty"

sha=$(sha256sum "$png" | cut -d' ' -f1)
args=(-sS -X PUT --max-time 60
      -H "Content-Type: image/png"
      -H "X-Frame-SHA256: $sha"
      -H "X-Persist: $persist"
      --data-binary "@$png")
[[ -n $id ]] && args+=(-H "X-Frame-ID: $id")

reply=$(mktemp)
trap 'rm -f "$reply"' EXIT
code=$(printf 'Authorization: Bearer %s\n' "$token" |
  curl "${args[@]}" -H @- -o "$reply" -w '%{http_code}' "http://$host/api/v1/frame") || die "curl failed (exit $?)"
cat "$reply"
echo
if [[ $code != 2* ]]; then
  echo "push-frame: HTTP $code" >&2
  exit 1
fi
