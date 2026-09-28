#!/usr/bin/env bash
# ABOUTME: Put a wpa_supplicant.conf (from a local SSID/PSK env file) on the TT7 over ssh, optionally start Wi-Fi.
# ABOUTME: The PSK is never echoed and never in any process's argv: it travels only over ssh's stdin.
# shellcheck disable=SC2029 # remote commands embed the fixed $remote_dir on purpose

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
host="${TT7_HOST:-root@10.55.0.1}"
known_hosts="$root/build/known_hosts"
remote_dir=/data/tt7/wifi

usage() {
  cat <<EOF
usage: scripts/wifi-setup.sh [--env <file>]... [--start] [--driver nl80211|wext]
       scripts/wifi-setup.sh [--env <file>]... --print-conf-to-stdout
       scripts/wifi-setup.sh --help

Reads SSID= and PSK= from an env file and writes $remote_dir/wpa_supplicant.conf
(mode 600) on the panel at \$TT7_HOST (default root@10.55.0.1), using ssh with
-o UserKnownHostsFile=build/known_hosts.

  --env <file>            env file to read, one network per file. Repeat it for
                          several networks (home + work): the first listed is
                          preferred when both are in range. Default: \$TT7_WIFI_ENV,
                          else ~/.config/tt7/wifi.env. Refused if group or others
                          have any access to it (chmod 600 it).
  --start                 then start Wi-Fi now (probe/tt7-wifi-start.sh, the
                          same script the image runs at boot) and print
                          wlan0's IPv4 address
  --driver <name>         also save the wpa_supplicant driver to use from now
                          on ($remote_dir/driver). nl80211 is the default; try
                          wext if nl80211 fails with this Broadcom driver.
  --print-conf-to-stdout  print the generated config and exit; no ssh. For tests.
                          It contains the PSK, so don't use it on a shared screen.

Env file: KEY=VALUE lines, '#' comments, optional 'export ', optional
surrounding quotes. PSK is an 8..63 character passphrase or 64 hex digits.
Quoting in the config (see scripts/wifi_conf.py): an SSID with a double quote
or non-ASCII bytes is written as hex; a passphrase with a double quote is
written as its derived 64-hex key. Spaces and backslashes are written as is.

Before the image ships wpa_supplicant, copy the tools to the panel first:
  ssh -o UserKnownHostsFile=build/known_hosts root@10.55.0.1 mkdir -p /data/tt7/bin
  scp -O -o UserKnownHostsFile=build/known_hosts build/wifi/wpa_supplicant build/wifi/wpa_cli root@10.55.0.1:/data/tt7/bin/
EOF
}

die() { echo "wifi-setup: $*" >&2; exit 1; }

env_files=()
start=0 print_only=0 driver=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --env) [[ $# -ge 2 ]] || die "--env needs a path"; env_files+=("$2"); shift 2 ;;
    --start) start=1; shift ;;
    --driver) [[ $# -ge 2 ]] || die "--driver needs nl80211 or wext"; driver=$2; shift 2 ;;
    --print-conf-to-stdout) print_only=1; shift ;;
    -h | --help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done
[[ -z "$driver" || "$driver" == nl80211 || "$driver" == wext ]] || die "--driver must be nl80211 or wext"

# One env file per network, preferred first. No --env: $TT7_WIFI_ENV, else the default.
(( ${#env_files[@]} )) || env_files=("${TT7_WIFI_ENV:-$HOME/.config/tt7/wifi.env}")
for env_file in "${env_files[@]}"; do
  [[ -f "$env_file" ]] || die "no env file at $env_file (see --help)"
  mode=$(stat -c %a "$env_file")
  (( (8#$mode & 8#077) == 0 )) || die "$env_file is mode $mode; group/others must have no access. Run: chmod 600 $env_file"
done

# The conf lives only in this variable and on ssh's stdin. printf is a bash
# builtin, so it never shows up in any process's argv.
conf=$(python3 "$root/scripts/wifi_conf.py" "${env_files[@]}") || die "could not build a config from ${env_files[*]}"
if (( print_only )); then
  printf '%s\n' "$conf"
  exit 0
fi

ssh_opts=(-o UserKnownHostsFile="$known_hosts" -o ConnectTimeout=10)
echo "wifi-setup: writing $remote_dir/wpa_supplicant.conf on $host"
# Write to a temp file with umask 077, check it is non-empty, then rename, so
# a dropped connection never leaves a half-written config behind.
printf '%s\n' "$conf" | ssh "${ssh_opts[@]}" "$host" \
  "umask 077 && mkdir -p $remote_dir && cat > $remote_dir/wpa_supplicant.conf.tmp \
   && test -s $remote_dir/wpa_supplicant.conf.tmp \
   && chmod 600 $remote_dir/wpa_supplicant.conf.tmp \
   && mv $remote_dir/wpa_supplicant.conf.tmp $remote_dir/wpa_supplicant.conf" \
  || die "writing the config over ssh failed"

if [[ -n "$driver" ]]; then
  echo "$driver" | ssh "${ssh_opts[@]}" "$host" "cat > $remote_dir/driver" || die "saving the driver choice failed"
  echo "wifi-setup: driver set to $driver"
fi

(( start )) || { echo "wifi-setup: done (Wi-Fi starts at next boot, or re-run with --start)"; exit 0; }

echo "wifi-setup: starting Wi-Fi"
ssh "${ssh_opts[@]}" "$host" sh -s < "$root/probe/tt7-wifi-start.sh" \
  || echo "wifi-setup: start script reported a failure; log follows" >&2
# Poll on the panel (one ssh session) for up to 30 s.
if ip=$(ssh "${ssh_opts[@]}" "$host" \
     'for i in $(seq 30); do a=$(ip -4 addr show wlan0 | awk "/inet /{print \$2; exit}"); [ -n "$a" ] && { echo "$a"; exit 0; }; sleep 1; done; exit 1'); then
  echo "wifi-setup: wlan0 is up at $ip"
  exit 0
fi
echo "wifi-setup: no IPv4 on wlan0 after 30 s. Last lines of /data/tt7/wifi.log:" >&2
ssh "${ssh_opts[@]}" "$host" "tail -n 20 /data/tt7/wifi.log" >&2 || true
exit 1
