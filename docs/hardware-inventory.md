# C4-TT7 hardware inventory

Measured on our unit (Control4 T3 7" tabletop, serial `000fff811af4`) under our probe firmware, 2026-09-27.
Raw evidence: `hardware/discovery/boot-000N-*/` (one directory per boot, from `probe/tt7-discover.sh`).
Anything not measured is marked **unknown**. SPEC.md §3 says: discover, don't assume.

## Platform

| Item | Measured | Evidence |
|---|---|---|
| SoC | Rockchip RK3188, 4× Cortex-A9 (`Hardware: RK30board`) | `cpuinfo.txt` |
| Kernel | `3.0.36+ #1-glassedge7p.2.0` (stock Control4, built 2023-01-09). The `p` suffix means the tabletop board ID | `uname-a.txt` |
| RAM | ~855 MiB usable (`MemTotal` 875008 kB) | `free` over ssh |
| Storage | Hynix NAND, 8528 MB, 16 KiB pages, 4 MiB blocks, driven by `rk30xxnand_ko` (FTL) | `rkdeveloptool read-flash-info`, `modules.txt` |
| Partitions | `mtdparts` from the Rockchip parameter block, identical to the in-wall T3 | `mtd.txt`, `cmdline.txt`, `backup/*/parameter.txt` |
| Kernel console | `ttyFIQ0` (a UART on the board; pins not located) | `cmdline.txt` |
| RTC | `rtc_hym8563` at i2c 1-0051 | `i2c-devices.txt` |
| PMIC | `act8846` at i2c 1-005a | `i2c-devices.txt` |

## Display

| Item | Measured |
|---|---|
| Device | `/dev/fb0` |
| Native geometry | **800×1280 portrait**, virtual 800×1280 (no pan buffer), stride 1600 bytes |
| Pixel format | 16 bpp **RGB565**: red 11/5, green 5/6, blue 0/5 |
| fb memory | `smem_len` 12582912 |
| Backlight | `/sys/class/backlight/rk28_bl`, `max_brightness` 255 |
| Logical orientation (1280×800 as docked) | **rotation 270** (at 90 the test frame was upside down; Doctor Biz, 2026-09-28) |

## Input

| Device | Node | Notes |
|---|---|---|
| `gslX680` (Silead) touch, i2c 2-0040 | `event1` | Same controller family as MMKeypad's in-wall 7". Touch dots landed under the finger in native orientation (Doctor Biz: "works really great"). Absolute axis ranges were **not recorded** (no evtest in the image); tt7probe reads them via EVIOCGABS at runtime |
| `rk29-keypad` (gpio-keys) | `event0` | power = 116, volume_up = 115 (logged 2026-09-28); volume_down presumably 114. `key_143` (KEY_WAKEUP) fires alongside touches: not a button |

## Networking

| Interface | State |
|---|---|
| `wlan0` | Broadcom **RK903** (`/sys/class/rkwifi/chip = OOB_RK903`), module `rkwifi.oob.ko` from stock `/system`. **Works** with wpa_supplicant 2.10 `-D nl80211`, 2.4 GHz only |
| `rndis0` | USB gadget (`android_usb`, functions `rndis,acm`), 10.55.0.1/24. Resets under load or low power; the watchdog in init repairs the TX stall (gotchas.md) |
| `ttyGS0` | USB serial (ACM) root shell, respawned by init |
| `eth0` (dock Ethernet) | **Not seen.** The panel wasn't on a powered dock during any discovery run. Unknown |

## Power

| Item | Measured |
|---|---|
| Supplies | `ac` (Mains) and `battery` only. There is **no USB supply**, and the micro-USB from a PC does **not** charge it |
| Battery gauge | `capacity` is **unreliable** (17% → 53% across one reboot, no charging) |
| Dock/AC charging | **Unknown**: not yet observed with `ac/online = 1` |

## Audio and camera

| Item | Measured |
|---|---|
| Codec | Realtek **RT3261** at i2c 4-001c, ALSA card `RK29_RT3261`, 2 playback + 2 capture PCMs (`aif1`, `aif2`) |
| Camera | `nt99141` sensor at i2c 3-002a, `/dev/video0` |

Neither has been exercised yet (SPEC §51: no audio or camera in v1).

## Open questions

1. Dock Ethernet: which driver it uses, and whether it survives the init forcing OTG into device mode.
2. Charging behaviour on the dock's own power adapter.
3. Confirm volume_down's keycode (114 expected). The gslX680 raw range is x 0..1280, y 0..800 (EVIOCGABS via tt7d /info).
4. Why the old partitions (`kernel`, `system`) read unstably while freshly written `boot` doesn't (gotchas.md).
