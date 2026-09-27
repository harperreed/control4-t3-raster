#!/usr/bin/env bash
# ABOUTME: Brings up the USB network link to a TT7 running our firmware (panel = 10.55.0.1, host = 10.55.0.2).
# ABOUTME: Finds the rndis interface of USB device 2207:0003 and binds the NetworkManager profile "tt7-usb" to it.
#
# When/why: after every panel boot. The panel has a static IP and no DHCP server, so NetworkManager
# otherwise gives up on the link and drops its addresses. The interface name can change per boot,
# so the profile is re-pointed each run. It is scoped to this one interface, so phone tethering
# (same rndis_host driver) is unaffected.
# Usage: scripts/usb-link.sh        then: ssh root@10.55.0.1
# First run needs `sudo` (creating an NM profile); it is owned by the invoking user, so later runs don't.

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

if nmcli -t -f NAME con show | grep -qx tt7-usb; then
  # Modifying a profile needs privileges; only do it when the interface name actually changed.
  current=$(nmcli -g connection.interface-name con show tt7-usb)
  [[ "$current" == "$iface" ]] || nmcli con modify tt7-usb connection.interface-name "$iface"
else
  nmcli con add type ethernet con-name tt7-usb ifname "$iface" \
    ipv4.method manual ipv4.addresses 10.55.0.2/24 ipv4.never-default yes \
    ipv6.method link-local connection.autoconnect yes \
    connection.permissions "user:${SUDO_USER:-$USER}" >/dev/null
fi
nmcli con up tt7-usb >/dev/null
echo "tt7-usb up on $iface; panel at 10.55.0.1"
ping -c 1 -W 2 10.55.0.1 >/dev/null && echo "panel answers ping" || { echo "panel does not answer ping on 10.55.0.1" >&2; exit 1; }
