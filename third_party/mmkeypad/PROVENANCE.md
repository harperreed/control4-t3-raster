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

Everything else, including network and Dropbear bring-up order, the USB
RNDIS+ACM gadget and root shell on `/dev/ttyGS0`, the NAND module insmod, the
vendor module loads, the Wi-Fi driver pick and the respawn loop, is upstream's
code unchanged.

The rest of the image also depends on one upstream convention: init insmods
`/lib/modules/rk30xxnand_ko.ko`, which upstream never shipped (their
`.gitignore` excludes `rootfs/lib/modules/`). Our build copies
`rk30xxnand_ko.ko.3.0.36+` out of this unit's stock boot ramdisk into `build/`
at build time. It is a vendor binary and is never committed.
