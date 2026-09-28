# control4-t3-raster

Turn an obsolete **Control4 T3 tabletop touchscreen** (C4-TT7) into a network display you drive from anywhere.

The panel runs its own stock kernel with a tiny Linux userspace of ours. A small daemon, `tt7d`, puts any PNG you send on the screen and reports what the hardware is doing. The server owns the meaning; the panel owns the pixels.

```text
your server ──PUT /api/v1/frame (PNG)──► tt7d ──► 7" 1280×800 screen
            ◄──── MQTT state / HA discovery ────
```

> **Status: working prototype, not polished.** Tested on one C4-TT7. Read [Safety](#safety-and-recovery) before touching yours.

## What works

- **Frames:** push a 1280×800 PNG over HTTP, and it's on the glass in about 0.2 s. A bad or partial frame never reaches the screen. Duplicate frames are skipped, and `X-Persist` keeps a frame across reboots.
- **Control panel:** `http://<panel>/` shows a live preview, telemetry, hardware, network and logs, with brightness, blank, wake, test-pattern and reboot controls. Actions need a token.
- **MQTT + Home Assistant:** availability with a last will, retained state, sensors, commands, and HA discovery for the hardware the panel really has. The broker is configurable at runtime.
- **Self-describing API:** `/api/v1/info`, `/state` and `/hardware` report what the panel has, discovered from the hardware and never hardcoded.
- **Wi-Fi** (2.4 GHz) at boot, **SSH** (key only), and a **USB network link** with a watchdog that repairs the Rockchip gadget's TX stall.
- **Flashing over the network**, with read-back verification and a restore of the stock boot image.

Planned, in [SPEC.md](SPEC.md) and the [plan](docs/superpowers/plans/2026-09-27-t3-custom-linux.md): touch events over WebSocket, a fallback clock when the server goes quiet, web firmware updates, and camera snapshots plus presence detection.

## Hardware

Rockchip RK3188 (4× Cortex-A9), about 850 MB RAM, 8.5 GB NAND, an 800×1280 RGB565 panel (shown as 1280×800), Silead gslX680 touch, Broadcom Wi-Fi, an RT3261 audio codec and an NT99141 camera. Everything measured on the unit is in [docs/hardware-inventory.md](docs/hardware-inventory.md), with the raw evidence in `hardware/discovery/`.

## How it works

The T3's boot partition holds an ordinary Android boot image, and its only integrity check is a CRC, with no signing key burned into the chip. So we keep **the unit's own kernel** and swap in our ramdisk:

- `init` (adapted from MMKeypad) mounts stock `/system` read-only for the vendor drivers, brings up the network, SSH and USB, and supervises the app.
- `tt7d` is a single static C daemon (musl, cross-built with `zig cc`) for HTTP, frames, the control panel and MQTT. The kernel is 3.0.36, older than Go and Rust support, hence C.
- App updates go to `/data/tt7/`, and `init` rolls back an app that keeps crashing.
- `tt7-server` ([server/README.md](server/README.md)) runs on your machine, not the panel: one headless Chrome shows a URL per panel, pushes what it paints as frames, and turns touches into clicks. Host-tested only so far.

## Quick start

You need Linux, `mise` (it installs the pinned zig), `python3`, `make`, `rkdeveloptool`, `curl`, and a micro-USB cable.

```sh
mise install
make            # build/tt7-probe-boot.img (first run downloads BusyBox, Dropbear, wpa_supplicant)
make check      # the canonical check: unit, image and end-to-end tests
```

### 1. Back up first

Power the panel fully off, **hold volume-up and plug in micro-USB**. It enters Rockchip Loader mode (`2207:310b`). Then:

```sh
scripts/backup-flash.sh my-unit
```

> ⚠️ Loader-mode reads of this NAND are **not reliable** for old partitions (see [gotchas.md](gotchas.md)). The boot partition has a built-in SHA1, and the flash tools check it before any restore. For a trustworthy full dump, boot our image and use `scripts/dump-via-ssh.sh`.

### 2. Flash and boot

```sh
scripts/flash-boot.sh build/tt7-probe-boot.img     # asks you to type 'write', verifies read-back
rkdeveloptool reboot
sudo scripts/usb-link.sh                           # once per USB port; the panel is 10.55.0.1
```

### 3. Wi-Fi, frames, MQTT

```sh
TT7_WIFI_ENV=~/wifi.env scripts/wifi-setup.sh --start             # SSID=…, PSK=… (mode 600)
ssh root@<panel> cat /data/tt7/tt7d/token > ~/.config/tt7/token
python3 tools/make-test-frame.py frame.png
TT7_TOKEN_FILE=~/.config/tt7/token tools/push-frame.sh <panel> frame.png --persist
tools/mqtt-setup.sh <panel> --broker 192.168.1.10:1883             # broker must be an IPv4 address
```

Later updates go over Wi-Fi: `scripts/flash-boot.sh --net root@<panel> build/tt7-probe-boot.img`.

## API at a glance

| Endpoint | Auth | What |
|---|---|---|
| `PUT /api/v1/frame` | token | PNG in, on screen (`X-Frame-ID`, `X-Frame-SHA256`, `X-Persist`) |
| `GET /api/v1/frame`, `/frame/image` | | current frame metadata / PNG |
| `GET /api/v1/info`, `/state`, `/hardware`, `/system` | | what the panel is and what it's doing |
| `PUT /api/v1/display/brightness`, `POST /display/{blank,wake,test-pattern}` | token | display control |
| `POST /api/v1/system/reboot`, `GET /api/v1/logs` | token | admin |
| `GET/PUT /api/v1/config/mqtt` | token | broker settings (the password is never returned) |

MQTT topics live under `tt7/<device-id>/`: `availability`, `state`, `sensor/*`, `event/*`, `cmd/{brightness,wake,blank,reboot}`. The reboot command is off unless enabled. Details are in [tt7d/README.md](tt7d/README.md).

## Safety and recovery

- **Loader mode lives in the chip's boot ROM** (volume-up + USB), so a bad boot image can always be replaced: `scripts/flash-boot.sh --restore` puts back the stock boot from your backup, after checking its embedded SHA1.
- We never write the bootloader, the kernel partition or `/system`. Only `boot` and files under `/data/tt7/` change.
- Never flash a whole partition made from a Loader-mode dump. Reads can come back corrupted, and flashing one would make the damage permanent.
- The panel has a battery, and nothing shuts it down cleanly when it's low. The micro-USB from a PC **does not charge it**, so use the dock with its own power adapter.
- The battery percentage under our firmware is a poor estimate. The voltage is more honest.

## Credits

- **[MMKeypad](https://github.com/nuvoxel/MMKeypad)** by darksoldier360 (Apache-2.0) did the hard part first: the T3 jailbreak, loader access and the custom-Linux approach. Our `init` and boot-image tools come from it; changes are listed in `third_party/mmkeypad/PROVENANCE.md`.
- **[lodepng](https://github.com/lvandeve/lodepng)** (zlib license) decodes the PNGs.
- BusyBox, Dropbear, wpa_supplicant and libnl-tiny are downloaded and built by the scripts, under their own licenses.

## License

Our code is under the [MIT license](LICENSE). Third-party code keeps its own license: see `third_party/*/`. BusyBox is GPL-2.0, so **if you distribute a built image, you take on GPL obligations** for it.

Control4 is a trademark of its owner. This project has no connection to Control4; it's for people giving old panels a second life.
