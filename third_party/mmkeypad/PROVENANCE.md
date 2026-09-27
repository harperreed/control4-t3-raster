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

None yet: the files above are byte-identical to the upstream commit.
