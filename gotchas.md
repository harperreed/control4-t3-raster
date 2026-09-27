# Gotchas

## Control4 panel on USB (2026-09-27)
- The panel shows up as `2207:0000` "rockchip rk3188" (serial 000fff811af4) while booted normally into the Control4 UI.
- In that state it only exposes USB mass storage with 2 LUNs (sdb/sdc), both "Media removed", 0 bytes. It exposes no ADB interface.
- The WCH `27c0:0859` "TouchScreen" on bus 1 is a separate device, not the panel.
- A web search (XDA, Control4 docs, c4forums) turned up no public ADB/root method for Control4 T3 panels. Control4's T3 "Upgrade Guide" only covers wall boxes.
- Loader/maskrom mode should show a different PID (RK3188 maskrom is `2207:310b`, per rkflashtool; verify at implementation).
- Control4's install guides say the in-wall T3 has a RESET pinhole plus an unlabeled pinhole left of the mic. Holding the unlabeled one while pressing RESET, until an icon appears, does a **factory data reset** (wipes the panel). On the tabletop, volume up plays the role of the unlabeled pinhole. That key is probably the Rockchip recovery key. Whether holding it with USB attached gets loader mode is unverified.

## C4-TT7 tabletop specifics
- Has a battery. Undocked, the reset or power button turns it off; docked, they reboot it. To abort a recovery-mode attempt: undock, then hold power.
- Community claims (Reddit r/C4diy thread 1uwrzra, which Doctor Biz pasted in a summary; UNVERIFIED because Reddit blocks our fetches): tabletop has a micro-USB recovery port like the in-wall; stock OS is Android 4.4.2; someone ran a custom Linux userspace + LVGL UI on the stock kernel; Control4 GPL kernel source is called "glassedge" (web search found nothing); tabletop and in-wall differ by a board-ID strap, so don't cross-flash images.

## Research access
- Reddit blocks WebFetch, curl, and headless agent-browser Chrome. Headless Chrome gets "blocked by network security", and old.reddit redirects to login. A headed browser can't run from Claude's shell because it has no DISPLAY. To read r/C4diy threads, a human has to paste them for now.
- agent-browser Chrome on this box needs `--args "--no-sandbox"` (AppArmor userns restriction). Only use it together with `--allowed-domains`.

## Prior art: github.com/nuvoxel/MMKeypad (read 2026-09-27, commit c95555d)
- Written by darksoldier360, the Reddit dev. Covers the full T3 jailbreak plus a custom Linux (LVGL) firmware. Every procedure in it was proven on **in-wall** 7"/10" units, none on the tabletop. Key docs: `reference/t3-control4/{README,JAILBREAK,FACTORY-IMAGE,UNIT-INVENTORY}.md`, `firmware-linux-t3/`.
- Loader mode: hold the unlabeled RECOVERY button while plugging in micro-USB (or powering on). The device then shows `2207:310b` and stays in loader mode. This is separate from Control4's factory-reset combo (recovery + RESET). Tabletop: Control4 swaps volume-up in for the unlabeled button, so volume-up is probably the recovery key (inferred, not tested). The tabletop has a battery, so power it fully off first.
- Tools: `rkdeveloptool` (`ld`, `rfi`, `rl <start> <count> <file>` at about 16 MB/s). Partition LBAs are in their README, e.g. system `0x200000 @ 0x744000`, boot `0x6000 @ 0xa000`.
- Security: `/system` ext4 has no dm-verity. `boot` has only a CRC check (no RSA key fused), so a repacked boot.img (stock kernel + own initramfs) boots.
- Their gotcha: identify a boot image by its gunzipped ramdisk, never by filename. A "boot.orig" dumped after a custom flash is not stock.
- Kernel tag for our model: `glassedge7p` = C4-TS-PORTABLE7.

## Loader mode on the C4-TT7: CONFIRMED 2026-09-27
- **Volume-up is the recovery key on the tabletop.** Power the panel fully off (undock, hold power), then hold volume-up and plug in the micro-USB. It comes up as `2207:310b` Loader. lsusb calls it "Mask ROM mode"; rkdeveloptool says Loader.
- The TT7's partition map (mtdparts) is identical to the in-wall map in the MMKeypad docs. NAND: Hynix, 8528 MB, 17465344 sectors.
- **Ubuntu's `rkdeveloptool` is the pine64 fork, and its read takes BYTES:** `rkdeveloptool read <start-sector> <num-bytes> <file>`. MMKeypad's `rl <start> <count>` counts sectors, so multiply its lengths by 512. The long command names (`read`, `list`, `read-flash-info`) replace `rl`/`ld`/`rfi`.
- The Debian udev rule leaves out 2207:310b. We added `/etc/udev/rules.d/61-rk3188-loader.rules` (GROUP=plugdev, uaccess). Without it you get "creating comm object failed".
- `scripts/backup-flash.sh <label>` makes a full read-only dump into `backup/<label>/` (git-ignored).

## ⚠️ Loader-mode NAND reads are NOT reliable on our TT7 (found 2026-09-27)
- Three full reads of `system` (1 GiB) disagreed on **258 × 16 KiB pages (4 MiB), all in ext4 blocks marked in use**. The bad regions are whole 16 KiB pages (the NAND page size), aligned to page boundaries.
- Read size doesn't matter: a bad page (system page 104) gives **15 different versions in 15 single-page reads**, each 54–90 bits off the bitwise majority. Other pages differ by about 50% of their bits between reads. Some pages that were bad in one bulk read were stable in five later reads.
- So `backup/tt7-stock-2026-09-27/` is **not a byte-exact image** of the large partitions. `boot` (12 MiB) re-read identical once. Nothing else has been verified.
- Hypothesis (UNVERIFIED): weak Hynix MLC pages that need read-retry/ECC handling, which the kernel FTL does but the rockusb loader doesn't. Android boots and runs fine, which fits this.
- **Rule: never flash a whole partition image made from a loader dump.** A corrupted read would get written back permanently. Write only the pages you changed, after checking they read back stable.

## Probe image build (2026-09-27)
- `make` builds `build/tt7-probe-boot.img`; `make check` is the canonical check. Nothing in either touches USB.
- **zig 0.16 rejects `-Wl,--warn-common`, `-Wl,-Map,<file>` and `-Wl,--verbose`**, which BusyBox's `scripts/trylink` passes on its final link. `toolchain/arm-linux-musleabihf-cc` drops those three.
- **zig cc links in Debug mode (unstripped, with debug info) when the link line has no `-O`.** Dropbear's link line has none, so pass `LDFLAGS="-Os -s"`. Neither `zig objcopy --strip-all` nor host binutils `strip` handles static ARM ELF.
- zig's musl defines `__USE_TIME_BITS64`, so `struct input_event` is 16 bytes on ARM, matching the 3.0.36 kernel. `tt7probe.c` asserts it.
- The NAND module init insmods (`/lib/modules/rk30xxnand_ko.ko`) comes from the **stock boot ramdisk** (`rk30xxnand_ko.ko.3.0.36+`). The build copies it into `build/` only.
- Dock Ethernet is most likely an **RTL8152B USB NIC**: `r8152` is built into the stock kernel, and stock init.rc runs `rtl8152_mac`. Which USB controller it hangs off is unknown. MMKeypad's init forces the OTG port into device mode, which would cut the NIC if it sits on OTG.
- Never `cat` every attribute under `/sys/devices/platform/usb20_otg`. The stock kernel has the Synopsys `wr_reg_test` attribute, whose read handler writes a register in a loop, going by the Synopsys driver source (not checked in the glassedge source).
- rkdeveloptool (pine64 17823e9) `write <begin-sector> <file>` writes the whole file from that sector (main.cpp WL/WRITE handler, read 2026-09-27). It has no usage string, and running it without args prints "Parameter of [WL] command is invalid".

## Wi-Fi (2026-09-27)
- `make clean` keeps `build/known_hosts` (the panel's pinned host key) and `build/flash-*/` (read-backs from real flashes). It used to be `rm -rf build`.
- Wi-Fi config: `scripts/wifi-setup.sh` (env file precedence: `--env`, then `$TT7_WIFI_ENV`, then `~/.config/tt7/wifi.env`) writes `/data/tt7/wifi/wpa_supplicant.conf`. At boot `tt7-app` runs `tt7-wifi-start` in the background, which logs to `/data/tt7/wifi.log`.
- nl80211 vs wext: stock Android 4.4 on this unit runs its own wpa_supplicant with `-Dnl80211` on wlan0 (init.connectivity.rc), and the stock kernel has cfg80211 plus its wext compat layer. If nl80211 fails, `wifi-setup.sh --driver wext` makes `-D wext` stick via `/data/tt7/wifi/driver`.
- wpa_supplicant.conf quoting: a `"..."` value runs to the last quote and takes backslashes literally, but the comment stripper pairs quotes, so an embedded `"` can turn a later `#` into a comment. `wifi_conf.py` writes such SSIDs as hex and such passphrases as their derived PSK.

## Running our probe image on the TT7 (2026-09-27)
- Kernel tag at runtime is `#1-glassedge7p.2.0`, so the tabletop board ID is detected. fb0 = 800x1280 portrait RGB565 (stride 1600). Touch = Silead gslX680; buttons = rk29-keypad; Wi-Fi = OOB_RK903 (Broadcom).
- **Wi-Fi works with `wpa_supplicant -D nl80211`** (2.10, static musl). `scripts/wifi-setup.sh --start` with `TT7_WIFI_ENV=~/wifi.env` pushes the config to `/data/tt7/wifi/` and gets a DHCP lease.
- **The USB gadget resets** (instant disconnect + re-enumerate) mid-transfer and around dock events. After a reset the panel's rndis0 has no IP and the ttyGS0 shell is dead: init sets them once at boot only. Fix is in progress (watchdog in tt7-app).
- The host-side RNDIS MAC changes every panel boot, so the host interface name changes too. NetworkManager then grabs it with a DHCP profile. Re-pointing `tt7-usb` needs sudo. The zero-config way in is IPv6 link-local: `ping -6 ff02::1%<if>`, then ssh to the fe80 address. Check the host key fingerprint first.
- The panel's SSH host key persists on /data: SHA256:GRP2WC0aghQcweo+FpGu8oJnAopdTWTvayOWyxjjQZE
- The `reset` pinhole needs a paperclip. A long press on power also reboots the panel under our firmware (Doctor Biz did this 2026-09-27).
