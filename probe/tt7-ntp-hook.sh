#!/bin/sh
# ABOUTME: BusyBox ntpd's -S hook on the TT7: once NTP has set the clock, write the sync marker tt7d reads.
# ABOUTME: Usage (by ntpd only): tt7-ntp-hook step|stratum|periodic|unsync, with $stratum and $offset in the env.
#
# BusyBox 1.36.1 networking/ntpd.c run_script(): ntpd spawns this without
# waiting, with argv[1] = "step" (it just stepped the clock), "stratum" (the
# stratum changed), "periodic" (every 11 min) or "unsync" (no reachable peer
# for 8 polls), and the env vars stratum, offset, freq_drift_ppm and
# poll_interval. On "step" ntpd has already reset its stratum to 16, so a
# step counts as synced whatever $stratum says; the others count when the
# stratum is below 16 (16 = unsynchronized).
#
# The marker lives in RAM (/run is on the ramdisk), so it never survives a
# reboot: tt7d shows no time until NTP succeeds again in this boot. "unsync"
# leaves the marker alone: the clock was set this boot and drifts slowly.
# The line format is what tt7d/timesync.c parses:
#   synced <unix seconds> <action> stratum=<n> offset=<seconds>

marker=${TT7_NTP_MARKER:-/run/tt7/ntp-synced}
log=${TT7_NTP_LOG:-/data/tt7/ntp.log}
action=$1

case "$action" in
    step | stratum | periodic)
        if [ "$action" = step ] || [ "${stratum:-16}" -lt 16 ]; then
            mkdir -p "$(dirname "$marker")"
            printf 'synced %s %s stratum=%s offset=%s\n' "$(date +%s)" "$action" "${stratum:-}" "${offset:-}" \
                > "$marker.tmp" && mv "$marker.tmp" "$marker"
        fi
        ;;
esac

# Rare events only (not "periodic", every 11 minutes): /data is flash.
case "$action" in
    step | unsync)
        echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) ntpd $action stratum=${stratum:-} offset=${offset:-}" >> "$log" 2> /dev/null
        ;;
esac
exit 0
