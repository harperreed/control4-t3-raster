#!/usr/bin/env bash
# ABOUTME: Sets up the USB network link to a TT7 running our firmware (panel = 10.55.0.1, host = 10.55.0.2).
# ABOUTME: Pins NetworkManager profile "tt7-usb" to the panel's USB port path, so it survives per-boot MAC changes.
#
# When/why: the panel has a static IP and no DHCP server, and its RNDIS MAC (hence the host interface
# name) changes every boot. A profile matched on the USB port path (udev ID_PATH) auto-connects every
# time the panel shows up on that port; phone tethering on other ports is unaffected.
# Usage: sudo scripts/usb-link.sh     once per USB port (needs sudo to create the profile)
#        scripts/usb-link.sh          afterwards: just brings the link up and pings the panel

set -euo pipefail

iface=""
for net in /sys/class/net/*; do
  dev=$(readlink -f "$net/device" 2>/dev/null) || continue
  usbdev=$(dirname "$dev")                      # interface dir -> its USB device dir
  [[ -f "$usbdev/idVendor" ]] || continue
  if [[ "$(cat "$usbdev/idVendor"):$(cat "$usbdev/idProduct")" == "2207:0003" ]]; then
    iface=$(basename "$net"); break
  fi
done
[[ -n "$iface" ]] || { echo "no TT7 USB network interface (want USB 2207:0003). Is the panel booted and on micro-USB?" >&2; exit 1; }
path=$(udevadm info -q property -p "/sys/class/net/$iface" | sed -n 's/^ID_PATH=//p')
[[ -n "$path" ]] || { echo "no udev ID_PATH for $iface" >&2; exit 1; }

if [[ "$(nmcli -g match.path con show tt7-usb 2>/dev/null)" != "$path" ]]; then
  nmcli con delete tt7-usb >/dev/null 2>&1 || true
  nmcli con add type ethernet con-name tt7-usb match.path "$path" \
    ipv4.method manual ipv4.addresses 10.55.0.2/24 ipv4.never-default yes \
    ipv6.method link-local connection.autoconnect yes connection.autoconnect-priority 100 >/dev/null
  echo "created tt7-usb for USB port $path"
fi
nmcli con up tt7-usb ifname "$iface" >/dev/null
echo "tt7-usb up on $iface; panel at 10.55.0.1"
ping -c 1 -W 2 10.55.0.1 >/dev/null && echo "panel answers ping" || { echo "panel does not answer ping on 10.55.0.1" >&2; exit 1; }
