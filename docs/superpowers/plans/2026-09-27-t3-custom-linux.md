# T3 tabletop → custom Linux display

## Now
- Step: probe image flashed and running (boot #2); Wi-Fi up at 192.168.23.197 via standalone wpa_supplicant. Subagent building USB-reset watchdog + `flash-boot.sh --net`
- Next: (1) Doctor Biz's go-ahead to flash the new image over Wi-Fi; (2) resumable kernel-side full dump (scripts/dump-via-ssh.sh) once the panel is docked/charging
- Approved: "perfect" (2026-09-27): build the first boot.img (MMKeypad init + BusyBox + Dropbear + discovery probe + test pattern). BUILD ONLY, flashing needs a separate go-ahead
- Approved: "Yes, flash it (Recommended)" (2026-09-27): flash build/tt7-probe-boot.img (sha256 aa232eaf16b85571…) to boot
- Approved: "2 gooo" (2026-09-27): flash the next image (USB watchdog + Wi-Fi at boot) over the network after review
- Open: Language for tt7d (C via zig cc recommended; Go>=1.24/Rust>=1.64 require kernel >=3.2, the panel has 3.0.36)
- Approved: "Custom Linux + own UI" (2026-09-27), chosen over web kiosk / PC-driven display / stock MMKeypad
- Approved: "a + c" (2026-09-27): dashboard + control panel content, now served by the server side of SPEC.md
- Approved: "i figired out a much better solution" + pasted SPEC.md (2026-09-27): tt7d dumb network display supersedes the Smashing/rooted-Android kiosk
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
- 2026-09-27: Probe image booted on TT7 (glassedge7p, fb 800x1280 RGB565, gslX680 touch). USB gadget resets killed the first kernel dump; Wi-Fi (nl80211) works. Kernel-side reads: misc/boot/recovery/backup stable, `kernel` mtd1 differed between two reads (to investigate).
- 2026-09-27: Stock `boot` dump verified byte-exact (embedded Rockchip SHA1 id matches). Large-partition loader dumps are NOT reliable (gotchas.md). Plan: take a clean full dump from our own Linux through the kernel mtd driver once it boots.
- 2026-09-27: Loader mode on TT7 = hold volume-up + plug micro-USB (confirmed). Partition map matches in-wall. Ubuntu rkdeveloptool reads in bytes (see gotchas.md).
- 2026-09-27: Built `build/tt7-probe-boot.img` on branch `m0-probe-image` (stock kernel + MMKeypad init + BusyBox + Dropbear + tt7probe). `make check` green. `scripts/flash-boot.sh` written, not run. Nothing written to the device.
