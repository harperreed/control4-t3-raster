#!/usr/bin/env bash
# ABOUTME: Assemble the ramdisk tree in build/rootfs from built binaries, probe scripts and stock pieces.
# ABOUTME: Run via `make rootfs`; mkcpio.py then packs it with every entry owned by root.
#
# Inputs (all produced by other make targets):
#   build/init, build/tt7probe, build/busybox/{busybox,busybox.links},
#   build/dropbear/dropbearmulti, build/wifi/{wpa_supplicant,wpa_cli},
#   build/stock/ramdisk/rk30xxnand_ko.ko.3.0.36+
#   SSH_PUBKEY (env): the one key allowed to log in as root.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
b="$root/build"
rfs="$b/rootfs"
key="${SSH_PUBKEY:?SSH_PUBKEY unset: run through make}"

# Exactly one ed25519 public key line, nothing else.
if [[ ! -f "$key" ]] || [[ $(wc -l < "$key") -ne 1 ]] || ! grep -qE '^ssh-ed25519 AAAA[0-9A-Za-z+/=]+( .*)?$' "$key"; then
  echo "stage-rootfs: $key is not a single ssh-ed25519 public key line" >&2
  exit 1
fi

rm -rf "$rfs"
mkdir -p "$rfs"
cp -a "$root/third_party/mmkeypad/rootfs/." "$rfs/"

install -D -m 0755 "$b/init" "$rfs/init"

# BusyBox and its applet links (busybox.links lists absolute paths).
install -D -m 0755 "$b/busybox/busybox" "$rfs/bin/busybox"
while read -r applet; do
  [[ "$applet" == /bin/busybox ]] && continue
  mkdir -p "$rfs$(dirname "$applet")"
  ln -sfn /bin/busybox "$rfs$applet"
done < "$b/busybox/busybox.links"

# Dropbear multi-binary; init runs /usr/sbin/dropbear, scp -O needs scp on PATH.
install -D -m 0755 "$b/dropbear/dropbearmulti" "$rfs/usr/sbin/dropbearmulti"
ln -sfn dropbearmulti "$rfs/usr/sbin/dropbear"
ln -sfn ../sbin/dropbearmulti "$rfs/usr/bin/dropbearkey"
ln -sfn ../sbin/dropbearmulti "$rfs/usr/bin/scp"

# The probe: init launches /usr/bin/tt7-app.
install -D -m 0755 "$b/tt7probe" "$rfs/usr/bin/tt7probe"
install -D -m 0755 "$root/probe/tt7-app.sh" "$rfs/usr/bin/tt7-app"
install -D -m 0755 "$root/probe/tt7-discover.sh" "$rfs/usr/bin/tt7-discover"
install -D -m 0755 "$root/probe/tt7-wifi-start.sh" "$rfs/usr/bin/tt7-wifi-start"

# Wi-Fi userspace. init loads the driver; tt7-wifi-start runs these.
install -D -m 0755 "$b/wifi/wpa_supplicant" "$rfs/usr/sbin/wpa_supplicant"
install -D -m 0755 "$b/wifi/wpa_cli" "$rfs/usr/sbin/wpa_cli"

# The NAND driver from this unit's own stock ramdisk. init insmods it by this
# name; loading it is what makes the kernel create the mtd partitions.
install -D -m 0644 "$b/stock/ramdisk/rk30xxnand_ko.ko.3.0.36+" "$rfs/lib/modules/rk30xxnand_ko.ko"

# Root's key. Dropbear refuses keys in group/world-writable paths.
install -d -m 0700 "$rfs/root" "$rfs/root/.ssh"
install -m 0600 "$key" "$rfs/root/.ssh/authorized_keys"

# Host umask may leave group/world write bits; nothing in the image needs them.
chmod -R go-w "$rfs"

echo "stage-rootfs: $(find "$rfs" | wc -l) entries in $rfs"
