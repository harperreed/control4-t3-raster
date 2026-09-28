#!/usr/bin/env bash
# ABOUTME: Host tests for scripts/wifi-setup.sh's config generation (--print-conf-to-stdout); no ssh, no device.
# ABOUTME: Run by `make check`. Covers quoting, hex fallbacks, env-file rules, permissions and --env precedence.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
setup="$root/scripts/wifi-setup.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fails=0

ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fails=$((fails + 1)); }

# envfile <name> <content>: a mode-600 env file in $tmp.
envfile() { printf '%s' "$2" > "$tmp/$1"; chmod 600 "$tmp/$1"; }

# expect_conf <description> <env-file> <ssid line> <psk line>
expect_conf() {
  local want got
  want=$(printf 'ctrl_interface=/var/run/wpa_supplicant\nupdate_config=0\n\nnetwork={\n\t%s\n\tscan_ssid=1\n\tkey_mgmt=WPA-PSK\n\t%s\n}\n' "$3" "$4")
  if got=$(env -u TT7_WIFI_ENV "$setup" --env "$tmp/$2" --print-conf-to-stdout 2> "$tmp/err") && [[ "$got" == "$want" ]]; then
    ok "$1"
  else
    bad "$1"; diff <(echo "$want") <(echo "$got") || true; cat "$tmp/err"
  fi
}

# expect_refused <description> <env-file> <secret that must not leak>
expect_refused() {
  if env -u TT7_WIFI_ENV "$setup" --env "$tmp/$2" --print-conf-to-stdout > "$tmp/out" 2> "$tmp/err"; then
    bad "$1 (was accepted)"
  elif grep -qF -- "$3" "$tmp/out" "$tmp/err"; then
    bad "$1 (secret leaked into output)"
  else
    ok "$1: $(head -1 "$tmp/err")"
  fi
}

envfile plain $'SSID=HomeNet\nPSK=correct horse battery\n'
expect_conf "plain SSID and passphrase with spaces are quoted" plain 'ssid="HomeNet"' 'psk="correct horse battery"'

envfile quoted $'# comment\n\nexport SSID="My Net"\nPSK=\'back\\slash#hash\'\n'
expect_conf "export, comments, surrounding quotes stripped; backslash and # kept" quoted 'ssid="My Net"' 'psk="back\slash#hash"'

envfile ssidquote $'SSID=Bob\'s "Cafe"\nPSK=password\n'
expect_conf "SSID with a double quote is written as hex" ssidquote \
  "ssid=$(printf '%s' "Bob's \"Cafe\"" | od -An -tx1 | tr -d ' \n')" 'psk="password"'

envfile utf8 $'SSID=Caf\xc3\xa9\nPSK=password\n'
expect_conf "non-ASCII SSID is written as hex" utf8 'ssid=436166c3a9' 'psk="password"'

# IEEE 802.11i H.4 test vector: passphrase "password", SSID "IEEE".
ieee=f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e
if [[ $(python3 -c "import sys; sys.path.insert(0, '$root/scripts'); import wifi_conf; print(wifi_conf.pmk('password', b'IEEE').hex())") == "$ieee" ]]; then
  ok "PBKDF2 key derivation matches the IEEE 802.11i test vector"
else
  bad "PBKDF2 key derivation vs IEEE 802.11i test vector"
fi
envfile pskquote $'SSID=IEEE\nPSK=pass"word\n'
want_hex=$(python3 -c "import sys; sys.path.insert(0, '$root/scripts'); import wifi_conf; print(wifi_conf.pmk('pass\"word', b'IEEE').hex())")
expect_conf "passphrase with a double quote is written as its derived key" pskquote 'ssid="IEEE"' "psk=$want_hex"

envfile hexpsk $'SSID=IEEE\nPSK=F42C6FC52DF0EBEF9EBB4B90B38A5F902E83FE1B135A70E23AED762E9710A12E\n'
expect_conf "64-hex PSK is used as the raw key" hexpsk 'ssid="IEEE"' "psk=$ieee"

envfile tooshort $'SSID=x\nPSK=abc4567\n'
expect_refused "7-character passphrase is refused" tooshort abc4567
envfile nopsk $'SSID=x\n'
expect_refused "missing PSK is refused" nopsk "SSID=x"
envfile junk $'SSID=x\nhunter2hunter2\nPSK=password\n'
expect_refused "line without '=' is refused without quoting it" junk hunter2hunter2
envfile ctrl $'SSID=x\nPSK=pass\tword1\n'
expect_refused "control character in passphrase is refused" ctrl $'pass\tword1'
envfile longssid "SSID=$(printf 'a%.0s' {1..33})"$'\nPSK=password\n'
expect_refused "33-byte SSID is refused" longssid "PSK=password"

envfile open $'SSID=HomeNet\nPSK=opensesame\n'
chmod 640 "$tmp/open"
expect_refused "group-readable env file is refused" open opensesame

# Precedence: --env beats TT7_WIFI_ENV, which beats the default.
if [[ $(TT7_WIFI_ENV="$tmp/plain" HOME="$tmp/nohome" "$setup" --print-conf-to-stdout | grep "^.ssid=") == $'\tssid="HomeNet"' ]]; then
  ok "TT7_WIFI_ENV is used when --env is absent"
else
  bad "TT7_WIFI_ENV is used when --env is absent"
fi
if [[ $(TT7_WIFI_ENV="$tmp/plain" "$setup" --env "$tmp/quoted" --print-conf-to-stdout | grep "^.ssid=") == $'\tssid="My Net"' ]]; then
  ok "--env beats TT7_WIFI_ENV"
else
  bad "--env beats TT7_WIFI_ENV"
fi
# (it fails: no file there; the error names the path it looked at)
default_err=$(env -u TT7_WIFI_ENV HOME="$tmp/nohome" "$setup" --print-conf-to-stdout 2>&1 || true)
if [[ "$default_err" == *"$tmp/nohome/.config/tt7/wifi.env"* ]]; then
  ok "default is ~/.config/tt7/wifi.env"
else
  bad "default is ~/.config/tt7/wifi.env"
fi

# Several networks: one --env per network; the first listed gets the highest priority.
envfile net_a $'SSID=HomeNet\nPSK=correct horse battery\n'
envfile net_b $'SSID=WorkNet\nPSK=another password\n'
want=$(printf 'ctrl_interface=/var/run/wpa_supplicant\nupdate_config=0\n\nnetwork={\n\tssid="HomeNet"\n\tscan_ssid=1\n\tkey_mgmt=WPA-PSK\n\tpsk="correct horse battery"\n\tpriority=2\n}\n\nnetwork={\n\tssid="WorkNet"\n\tscan_ssid=1\n\tkey_mgmt=WPA-PSK\n\tpsk="another password"\n\tpriority=1\n}\n')
if got=$(env -u TT7_WIFI_ENV "$setup" --env "$tmp/net_a" --env "$tmp/net_b" --print-conf-to-stdout 2> "$tmp/err") && [[ "$got" == "$want" ]]; then
  ok "two --env files: two networks, first has the higher priority"
else
  bad "two --env files: two networks, first has the higher priority"; diff <(echo "$want") <(echo "$got") || true; cat "$tmp/err"
fi
envfile net_dup $'SSID=HomeNet\nPSK=some other secret\n'
if env -u TT7_WIFI_ENV "$setup" --env "$tmp/net_a" --env "$tmp/net_dup" --print-conf-to-stdout > "$tmp/out" 2> "$tmp/err"; then
  bad "the same SSID twice is refused (was accepted)"
elif grep -qF -- "some other secret" "$tmp/out" "$tmp/err"; then
  bad "the same SSID twice is refused (secret leaked)"
else
  ok "the same SSID twice is refused: $(head -1 "$tmp/err")"
fi
envfile net_open $'SSID=CafeNet\nPSK=cafepassword\n'; chmod 644 "$tmp/net_open"
if env -u TT7_WIFI_ENV "$setup" --env "$tmp/net_a" --env "$tmp/net_open" --print-conf-to-stdout > /dev/null 2> "$tmp/err"; then
  bad "a group-readable second env file is refused (was accepted)"
else
  ok "a group-readable second env file is refused"
fi

(( fails == 0 )) || { echo "test-wifi-setup: $fails check(s) FAILED"; exit 1; }
echo "test-wifi-setup: all checks passed"
