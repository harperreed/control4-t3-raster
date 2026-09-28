#!/usr/bin/env bash
# ABOUTME: Point a TT7 running tt7d at an MQTT broker: PUT /api/v1/config/mqtt with the bearer token.
# ABOUTME: The password is read from a file and sent in the request body on stdin, never in argv.

set -euo pipefail

usage() {
  cat <<'EOF'
usage: tools/mqtt-setup.sh <panel-host[:port]> --broker HOST[:PORT] [--user U]
                           [--password-file PATH] [--prefix P] [--no-ha]

  --broker HOST[:PORT]  broker IPv4 address, port 1883 if omitted
  --user U              broker user name (omit for an anonymous broker)
  --password-file PATH  file whose first line is the broker password; it is
                        sent to tt7d in the request body over stdin, so it never
                        shows up in `ps`. Without this flag the password on
                        the panel stays as it is.
  --prefix P            topic prefix (default on the panel: tt7)
  --no-ha               turn Home Assistant discovery off (default: on)

Also turns MQTT on. Prints tt7d's reply: the settings now in effect, with
password_set instead of the password.

The token comes from $TT7_TOKEN_FILE (default ~/.config/tt7/token); see
tools/push-frame.sh --help for how to fetch it.

Example (the broker at home):
  tools/mqtt-setup.sh 192.168.23.197 --broker 192.168.23.123:1883 \
      --user tt7 --password-file ~/.config/tt7/mqtt-password
EOF
}

die() { echo "mqtt-setup: $*" >&2; exit 1; }

# A JSON string literal. Refuses control characters rather than escape them:
# tt7d refuses them too.
json_str() {
  local s=$1
  [[ $s != *[[:cntrl:]]* ]] || die "control characters are not allowed in '$2'"
  s=${s//\\/\\\\}
  s=${s//\"/\\\"}
  printf '"%s"' "$s"
}

[[ $# -ge 1 && ( $1 == -h || $1 == --help ) ]] && { usage; exit 0; }
[[ $# -ge 1 ]] || { usage >&2; exit 2; }
panel=$1
shift
broker= user= password_file= prefix= ha=true user_set=false
while [[ $# -gt 0 ]]; do
  case $1 in
    --broker) [[ $# -ge 2 ]] || die "--broker needs a value"; broker=$2; shift ;;
    --user) [[ $# -ge 2 ]] || die "--user needs a value"; user=$2; user_set=true; shift ;;
    --password-file) [[ $# -ge 2 ]] || die "--password-file needs a value"; password_file=$2; shift ;;
    --prefix) [[ $# -ge 2 ]] || die "--prefix needs a value"; prefix=$2; shift ;;
    --no-ha) ha=false ;;
    -h | --help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
  shift
done
[[ -n $broker ]] || die "--broker is required"

host=${broker%%:*}
port=1883
[[ $broker == *:* ]] && port=${broker##*:}
[[ $port =~ ^[0-9]+$ ]] || die "bad port in --broker $broker"

body="{\"enabled\":true,\"host\":$(json_str "$host" host),\"port\":$port,\"ha_discovery\":$ha"
$user_set && body+=",\"username\":$(json_str "$user" user)"
[[ -n $prefix ]] && body+=",\"prefix\":$(json_str "$prefix" prefix)"
if [[ -n $password_file ]]; then
  [[ -r $password_file ]] || die "cannot read $password_file"
  password=
  IFS= read -r password < "$password_file" || true # no final newline is fine
  password=${password%$'\r'}
  [[ -n $password ]] || die "$password_file has no password on its first line"
  body+=",\"password\":$(json_str "$password" password)"
  unset password
fi
body+="}"

token_file=${TT7_TOKEN_FILE:-$HOME/.config/tt7/token}
[[ -r $token_file ]] || die "no token file at $token_file (see tools/push-frame.sh --help)"
token=$(tr -d ' \r\n' < "$token_file")
[[ -n $token ]] || die "$token_file is empty"

reply=$(mktemp)
trap 'rm -f "$reply"' EXIT
# Body on stdin, token header from a pipe: neither appears in curl's argv.
code=$(printf '%s' "$body" |
  curl -sS -X PUT --max-time 30 -H "Content-Type: application/json" \
    -H @<(printf 'Authorization: Bearer %s\n' "$token") \
    --data-binary @- -o "$reply" -w '%{http_code}' "http://$panel/api/v1/config/mqtt") || die "curl failed (exit $?)"
unset body
cat "$reply"
echo
if [[ $code != 2* ]]; then
  echo "mqtt-setup: HTTP $code" >&2
  exit 1
fi
