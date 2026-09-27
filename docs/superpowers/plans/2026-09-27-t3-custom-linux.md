# T3 tabletop → custom Linux display

## Now
- Step: BLOCKED. Loader-mode NAND reads are unstable (gotchas.md), so the backup is not byte-exact
- Next (proposed, awaiting Doctor Biz): pivot to Smashing + rooted stock Android kiosk; root via a surgical page-level write of build.prop only, never a whole-partition flash
- Open: single-page build.prop write proposed; Doctor Biz answered "Not yet" (2026-09-27). Where will the Smashing server live?
- Approved: "Custom Linux + own UI" (2026-09-27), chosen over web kiosk / PC-driven display / stock MMKeypad
- Approved: "a + c" (2026-09-27): M2 = home dashboard (clock, weather, calendar, Home Assistant entities) + control panel (buttons that fire webhooks/scripts)
- Compactions: 0

## Goal
Turn a Control4 T3 7" tabletop (C4-TT7, RK3188) into a display running our own UI. Keep the stock kernel and swap the userspace, as MMKeypad does.

## Source of truth for prior art
github.com/nuvoxel/MMKeypad (Apache-2.0), commit c95555d. Read so far:
- `reference/t3-control4/README.md`: loader mode, partition map, security model (boot = CRC only, /system has no verity)
- `reference/t3-control4/JAILBREAK.md`: root adb (path A), and the note that repacked boot.img works (path B, ours)
- `firmware-linux-t3/Makefile`, `init/init.c` (outline only)

Architecture per those files: `boot.img` = stock kernel + our gzipped cpio ramdisk (static musl `init`, BusyBox, Dropbear, wpa_supplicant). `init` mounts stock `/system` (vendor .ko + firmware) and `/data` (userdata ext4). It runs the app from `/data/mmkeypad`, supports an `/data/init.overlay` with rollback, and brings up USB serial. App updates go to `/data` over SSH, so we don't reflash per change.

## Milestones

### M0: backup (in progress)
- Full dump from sector 0 to the end of flash, with SHA256SUMS. Restore path for everything below.
- Done when: every region is present at its full size and the sums are written.

### M1: prove the pipeline on the tabletop (MMKeypad image, unmodified except our SSH key)
- Tools: zig (the cross cc; add via mise, pinned in the project), python3, make.
- Get `kernel.img` + `boot.orig` from **our** `02_kernel.bin` / `03_boot.bin` (the Makefile's `STOCK` dir). How the extraction works is "verify at implementation" (see their FACTORY-IMAGE.md).
- Stage Doctor Biz's `~/.ssh/id_ed25519.pub`. **Never** use their `dev-keys/authorized_keys` (MMK_DEV=1 ships their key).
- Flash `boot` only (LBA 40960) in Loader mode, read it back, compare, reboot.
- Done when the panel boots to their UI, **touch responds in the correct orientation**, and we can SSH in over Ethernet (dock) or Wi-Fi, or at least reach the USB serial console.
- Risks, all tabletop-specific and untested upstream: the touch controller (7" in-wall = Silead GSL1680, tabletop unknown), panel orientation, the battery/charger driver, and the dock Ethernet path.
- Rollback: write `backup/tt7-stock-2026-09-27/03_boot.bin` back to LBA 40960.
- **Build gaps in the public repo (found 2026-09-27):** there is no Dropbear build script (`.gitignore` says it's vendored, but it isn't committed). `build-busybox.sh` needs a BusyBox `.config` that isn't shipped. LVGL comes from ESP-IDF `managed_components`, which isn't committed either; its version is in `firmware-idf/main/idf_component.yml`. So "unmodified MMKeypad image" really means: rebuild BusyBox (static, `-fno-strict-aliasing`, per their script notes) and Dropbear ourselves, and skip their app. The M1 ramdisk = their `init.c` + BusyBox + Dropbear + **our own tiny test app** (fill the framebuffer with colors, log touch events). That tests exactly the tabletop unknowns and needs no LVGL.
- Stock boot unpacks cleanly with their `mkbootimg.py`: kernel 3.0.36+, built 2023-01-09, page 16384, static tag `#1-glassedge.0`. Check the runtime `uname -v` (they expect a `glassedge7p`-style tag) at first boot.

### M2: our UI
- Fork `firmware-linux-t3` into this repo (keep their Apache-2.0 LICENSE/NOTICE) and replace the Control4 app with ours on the same LVGL platform layer.
- Content: home dashboard (clock, weather, calendar, HA entities) + control panel (buttons → webhooks/scripts). Detailed design gets discussed before building.
- Dev loop: `sim/` headless renderer on the PC → scp the binary to `/data` → restart the app.

## Log
- 2026-09-27: Loader mode on TT7 = hold volume-up + plug micro-USB (confirmed). Partition map matches in-wall. Ubuntu rkdeveloptool reads in bytes (see gotchas.md).
