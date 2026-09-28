# Provenance: MMKeypad files vendored here

- Upstream: https://github.com/nuvoxel/MMKeypad
- Commit: `c95555d644e747db3d0fb15b12e087974e5662a7` (2026-09-22)
- License: Apache-2.0 (`LICENSE`, `NOTICE` copied verbatim from the upstream repo root)
- Upstream author credit: NuVoxel LLC ("darksoldier360")

Only the files our TT7 probe image uses are copied. Paths below are relative to
upstream's `firmware-linux-t3/`.

| Here | Upstream | Used for |
|---|---|---|
| `init/init.c` | `init/init.c` | PID 1 of the boot image |
| `init/font8x8_basic.h` | `init/font8x8_basic.h` | text in the probe's test pattern (public domain, see NOTICE) |
| `tools/mkbootimg.py` | `tools/mkbootimg.py` | unpack the stock boot image, pack ours, `compute_id` |
| `tools/mkcpio.py` | `tools/mkcpio.py` | build the root-owned newc ramdisk |
| `rootfs/etc/{group,passwd,profile,shells}` | `rootfs/etc/…` | root account, login shell list |
| `rootfs/usr/share/udhcpc/default.script` | same | applies the DHCP lease on eth0 |

Not copied: upstream's `Makefile`, `tools/build-busybox.sh`, `toolchain/` and
`dev-keys/`. Our `Makefile`, `scripts/build-busybox.sh` and `toolchain/` do the
same jobs for a Linux host and our layout; `scripts/build-busybox.sh` takes its
per-file awk `-O0` fix from upstream's `build-busybox.sh`. Upstream's `MMK_DEV=1`
developer key is never used: the image carries only the owner's own key.

## Ported, not copied: `tools/build-wpa.sh` -> `scripts/build-wpa.sh`

Upstream's `tools/build-wpa.sh` (same commit) is not vendored; our
`scripts/build-wpa.sh` reimplements it for our layout. Kept from upstream: the
recipe (wpa_supplicant 2.10 + wpa_cli, static, nl80211 through libnl-tiny,
internal TLS/crypto, no OpenSSL), the exact wpa `.config` (`DRIVER_NL80211`,
`LIBNL_TINY`, `CTRL_IFACE`, `BACKEND=file`, `TLS=internal`,
`INTERNAL_LIBTOMMATH`, `IEEE80211W`), and the `EXTRA_CFLAGS` warning
relaxations. Upstream reports this recipe working end to end on an in-wall T3
(AP6330/BCM4330, `rkwifi.oob.ko`, `-D nl80211`, 2026-07-16). Changed:

- libnl-tiny was an unpinned `git clone --depth 1` of openwrt/libnl-tiny. It is
  now pinned to commit `40493a655d8caa2ccf5206dde1e733abe2920432` and fetched as
  a hash-checked codeload tarball; wpa_supplicant 2.10 is hash-checked too
  (`scripts/fetch-sources.sh`).
- libnl-tiny is compiled without `-D_GNU_SOURCE`: `unl.c` defines it itself,
  and passing it again was the build's only warning.
- It links with `LDFLAGS="-Os -s"` instead of running a separate strip step,
  because zig links unstripped in Debug mode when the link line has no `-O`.
- It builds in a fresh `build/wpa/` every time and installs to `build/wifi/`.

Upstream's app drove Wi-Fi from C (`platform/wifi_linux.c`, not copied). Ours
is `probe/tt7-wifi-start.sh`, run by `tt7-app` and by `scripts/wifi-setup.sh`.

## Changes to upstream code

Only `init/init.c` changed. Every other file here is byte-identical to upstream
(`git diff <vendor commit> -- third_party/mmkeypad` shows each change).

`init/init.c`:

1. **ABOUTME header.** Two `ABOUTME:` comment lines at the top, per this
   repo's convention. The upstream header comment follows unchanged.
2. **Every `/data` path moved under `/data/tt7` (`TT7_DIR`).** /data is the
   unit's stock Android userdata partition, which we keep, so init may only touch
   its own subdirectory. Upstream wrote to the top of /data:

   | Upstream | Here |
   |---|---|
   | `/data/mmkinit-boot.log` (`BOOTLOG`) | `/data/tt7/init-boot.log` |
   | `/data/init.overlay`, `.bad` | `/data/tt7/init.overlay`, `.bad` |
   | `/data/nvx/init.trial` (and `mkdir /data/nvx`) | `/data/tt7/init.trial` (`mkdir /data/tt7`) |
   | `/data/mmkeypad` (app overlay), `/data/mmkeypad.bad` | `/data/tt7/app`, `/data/tt7/app.bad` |
   | `/data/mmkeypad.log` (+ `.prev`) | `/data/tt7/app.log` (+ `.prev`) |

   This also means the overlay and rollback logic cannot pick up anything stale:
   it only looks inside `/data/tt7`, which does not exist until this init creates
   it, so a leftover MMKeypad `/data/init.overlay` or `/data/mmkeypad` is ignored.
   Comments that named the old paths now name the new ones.
3. **`mkdir /data/tt7` right after /data mounts**, before the first durable log
   line, since `BOOTLOG` now lives inside it.
4. **Persistent Dropbear host keys.** After /data mounts, init creates
   `/data/tt7/dropbear` (0700) and bind-mounts it on `/etc/dropbear`. Upstream
   left `/etc/dropbear` on the RAM ramdisk, so `dropbear -R` made new host keys
   every boot. Dropbear's `-R` path reads (or generates and writes) the key file
   in each connection's child (`svr_ensure_hostkey`, src/svr-kex.c), so the
   already-running server uses the bound dir without a restart.
5. **cache and metadata are no longer mounted.** Upstream mounted both
   read-write. Nothing in the probe image needs them, and a read-write ext4 mount
   writes to the partition (superblock, journal replay). `/system` is still
   mounted read-only for the vendor modules and firmware.
6. **Factory app path is `/usr/bin/tt7-app`** (was `/usr/bin/mmkeypad`), our
   probe launcher. The app launch log line names the paths instead of "mmkeypad".

7. **USB link watchdog (added after first boot on the TT7).** The gadget
   resets (instant disconnect and re-enumerate) mid-transfer and around dock
   events. Upstream's `usb_gadget_rndis()` gives rndis0/usb0 their address once
   at boot, so after a reset the panel had no USB IP. Now the worker loop reaps
   a 3 s `sleep` child as a tick (`usb_tick`). On each tick `usb_link_check`
   re-applies 10.55.0.1/24 to any of rndis0/usb0 that exists without that
   address or is down, and retries the ttyGS0 shell if none is running. It
   never touches `/sys/class/android_usb` (re-running the gadget setup would
   itself reset USB). Actions go to `/data/tt7/usb-watchdog.log` and kmsg; each
   distinct message is logged at most once a minute, and the log rotates once at
   256 KiB.
8. **ttyGS0 shell respawn fixes** in `spawn_serial_console`:
   - If `open("/dev/ttyGS0")` fails, the child now exits so PID 1 respawns it.
     Upstream exec'd `sh -i` anyway on init's inherited fds (the UART console).
     That shell never exits, so ttyGS0 was never respawned and stayed dead until
     reboot.
   - Every respawn after the first waits 2 s, so a port that hangs up at once
     cannot make PID 1 fork in a tight loop.
   - Respawns are logged to the rate-limited USB log, not the boot log.

9. **RNDIS transmit-stall remedy (after a second field reset, 19:15:57).**
   After one reset, rndis0 kept 10.55.0.1 and stayed up, rx_packets kept
   rising, tx_packets froze, and `ifconfig down/up` did not help. Re-enabling
   the gadget (`android0/enable` 0, 1 s, 1, 2 s, re-apply the IP) did. On each
   tick, `usb_stall_check` feeds rndis0's (else usb0's) counters to
   `usb_stall_tick()` in `init/usb_stall.c`. That is our file, not upstream:
   a pure function with host tests in `init/test_usb_stall.c`.
   - On SUSPECT (rx arrived, tx flat) it sends one UDP broadcast out of the
     interface, so a healthy link that simply got unanswerable multicast shows
     tx by the next tick.
   - On STALLED (tx flat for >= 2 ticks despite that, >= 2 packets received,
     >= 30 s since the last remedy) it runs the sequence above.
   - It logs the counters, gadget `state` and `functions`. It only acts when
     `functions` contains rndis, and only ever writes `enable`.
   - `state` is logged but not required: what it reads during a stall is
     unknown, and rx still arriving already shows the host has the device
     configured.

Everything else, including network and Dropbear bring-up order, the USB
RNDIS+ACM gadget setup (init only writes `enable` afterwards, see 9), the NAND module insmod, the
vendor module loads, the Wi-Fi driver pick and the respawn loop, is upstream's
code unchanged.

The rest of the image also depends on one upstream convention: init insmods
`/lib/modules/rk30xxnand_ko.ko`, which upstream never shipped (their
`.gitignore` excludes `rootfs/lib/modules/`). Our build copies
`rk30xxnand_ko.ko.3.0.36+` out of this unit's stock boot ramdisk into `build/`
at build time. It is a vendor binary and is never committed.
