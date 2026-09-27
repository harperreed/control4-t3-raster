#!/bin/sh
# ABOUTME: Start Wi-Fi on the TT7: wpa_supplicant on wlan0 with /data/tt7/wifi/wpa_supplicant.conf, then udhcpc.
# ABOUTME: Run by tt7-app at boot and streamed over ssh by scripts/wifi-setup.sh --start. Logs to /data/tt7/wifi.log.
#
# Safe to run again: a running wpa_supplicant is told to re-read the config,
# and a running udhcpc for wlan0 is left alone.
# Driver: $TT7_WIFI_DRIVER, else the first line of /data/tt7/wifi/driver, else
# nl80211. Use "wext" if nl80211 does not work with this Broadcom driver.

PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH
dir=/data/tt7/wifi
conf=$dir/wpa_supplicant.conf
ctrl=/var/run/wpa_supplicant
dhcp_pid=/var/run/udhcpc-wlan0.pid

exec >> /data/tt7/wifi.log 2>&1
echo "== tt7-wifi-start at uptime $(cut -d' ' -f1 /proc/uptime)s"

[ -f "$conf" ] || { echo "no $conf; nothing to do"; exit 1; }

driver=${TT7_WIFI_DRIVER:-}
[ -n "$driver" ] || driver=$(head -n 1 "$dir/driver" 2>/dev/null)
[ -n "$driver" ] || driver=nl80211

# The image ships the tools in /usr/sbin; before a reflash they can be copied
# to /data/tt7/bin instead.
bin=
for d in /usr/sbin /data/tt7/bin; do
    if [ -x "$d/wpa_supplicant" ]; then bin=$d; break; fi
done
[ -n "$bin" ] || { echo "no wpa_supplicant in /usr/sbin or /data/tt7/bin"; exit 1; }

if pidof wpa_supplicant > /dev/null; then
    echo "wpa_supplicant already running; asking it to re-read $conf"
    "$bin/wpa_cli" -p "$ctrl" -i wlan0 reconfigure
else
    echo "starting $bin/wpa_supplicant -D $driver on wlan0"
    mkdir -p "$ctrl"
    "$bin/wpa_supplicant" -B -D "$driver" -i wlan0 -c "$conf" -C "$ctrl" \
        || { echo "wpa_supplicant failed to start (exit $?)"; exit 1; }
fi

if [ -f "$dhcp_pid" ] && kill -0 "$(cat "$dhcp_pid")" 2> /dev/null; then
    echo "udhcpc already running for wlan0"
else
    # -b: go to the background if no lease comes at once and keep trying,
    # since association may still be in progress.
    udhcpc -i wlan0 -b -p "$dhcp_pid" -s /usr/share/udhcpc/default.script
fi
echo "done: $(ip -4 addr show wlan0 | grep inet || echo 'no IPv4 on wlan0 yet')"
