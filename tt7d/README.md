# tt7d: the TT7 network display daemon

tt7d shows PNG frames that a server PUTs over HTTP on the C4-TT7's framebuffer
(SPEC.md milestones M1 and M2), and serves a local admin control panel at `/`
(M4, see "Control panel" below). It is one static C binary with no threads and
no libraries beyond musl and a vendored PNG decoder.

## Design in brief

- **Display**: `/dev/fb0` (800×1280 portrait, RGB565, stride 1600 on our unit).
  Geometry and the channel layout come from `FBIOGET_VSCREENINFO`/`FSCREENINFO`
  at start. The driver's odd `grayscale`/`nonstd` values are ignored; only the
  channel bitfields describe the pixels. Packing goes through
  `probe/fbdraw.c`, the same code tt7probe uses.
- **Logical display**: 1280×800. `--rotation` (0/90/180/270) is how many
  degrees clockwise the logical image is turned to land on the native fb. At
  90, logical (x, y) goes to native (799 − y, x): the image's top-left corner
  lands at the fb's top-right. **The default of 90 is UNVERIFIED.** Nobody has
  yet photographed the panel on its dock showing which fb corner is up. Push
  `tools/make-test-frame.py` output and look: if TOP-LEFT isn't top-left, try
  `--rotation 270`.
- **Frames**: the PNG is received in full, hashed, checked (size, SHA-256,
  dimensions), decoded into RGBA, converted into a RAM back buffer, and only
  then copied to the fb with one `memcpy`. A partial or bad frame never
  touches the screen. The fb has no second page (`yres_virtual` = `yres`), so
  that copy is not synchronized with scan-out, and a frame change may tear
  for one refresh.
- **HTTP**: our own minimal HTTP/1.1 server (`http.c`, `server.c`). It runs a
  single-threaded `poll()` loop over at most 8 connections and answers one
  request per connection, always with `Connection: close`. Bodies need
  `Content-Length`: chunked requests get 411. The request head is limited to
  8 KiB. `Expect: 100-continue` works, so curl's large uploads get their auth
  and size checks before any body is sent. Each connection has a deadline
  (`--request-timeout-ms`, default 30 s) to deliver its request, and the same
  again to read its reply. A slow client never holds up the others; it only
  holds its own slot until its deadline.
- **PNG decoder**: lodepng (zlib license), vendored in `third_party/lodepng`
  with a PROVENANCE file. Why lodepng and not stb_image: it only does PNG, it
  checks CRCs and Adler-32 by default, upstream ships a fuzz target
  (`lodepng_fuzzer.cpp`), and
  `lodepng_inspect` reads the size from IHDR first, so a wrong-sized frame is
  refused before anything big is allocated. Over the network it only runs on
  requests that already passed the token check.
- **Logging**: startup lines, failed requests (status ≥ 400) and reboot requests only. The log
  is `/data/tt7/app.log`, which lives on flash, and frames arriving every few
  seconds must not turn into a stream of flash writes. `/api/v1/state` counts
  the successes.

## Files in the data dir (`--data-dir`, default `/data/tt7/tt7d`)

| File | What |
|---|---|
| `token` | Bearer token, 64 hex digits, mode 0600, created on first start. Write your own (any non-empty string) to replace it; delete it to get a new one |
| `device.json` | `{"device_id": "tt7-xxxxxx"}`, created once from `/dev/urandom` and never derived from the IP. A file that doesn't parse is left alone and the id is reported as `null` |
| `last-frame.png` | The last frame sent with `X-Persist: true`, written with temp file, fsync and rename. Shown at startup |
| `last-frame.id` | `<sha256> <frame id>` for that PNG, so a restored frame keeps its id |

## API (`/api/v1`)

Reads need no auth, except `GET /logs`. `PUT /frame`, `GET /logs` and every
control panel action need `Authorization: Bearer <token>` (`/info` lists them
under `auth.required_for`).
`GET /frame/image` is also unauthenticated in v1: it returns the frame that is
already visible on the glass. Revisit this when the panel shows anything private.

| Endpoint | Returns |
|---|---|
| `GET /info` | `device_id`, `model`, `firmware_version`, `build`; `display` {`width`, `height`, `rotation`, `frame_formats`, `max_frame_bytes`, `native` {`width`, `height`, `format`, `stride`, `bits_per_pixel`}}; `capabilities` read from sysfs at request time; `auth` |
| `GET /state` | `time` (UTC; the clock is wrong until something sets it), `uptime_s`, `daemon_uptime_s`, `display` {`on`, `brightness`, `frame_id`, `frame_age_s`}, `power`, `network.interfaces`, `frames` {`accepted`, `deduplicated`, `rejected`, `last_error`} |
| `GET /frame` | Frame metadata: `frame_id`, `sha256`, `received_at`, `displayed_at`, `width`, `height`, `content_type`, `bytes`, `persisted`, `deduplicated`, `restored`. 404 `no_frame` before the first frame |
| `GET /frame/image` | The PNG exactly as received (or as restored). 404 `no_frame` before the first frame |
| `PUT /frame` | Body: a 1280×800 PNG. Headers: `Content-Type: image/png` (required), `X-Frame-ID` (1–128 printable ASCII, no spaces; generated as `tt7d-<24 hex>` if absent), `X-Frame-SHA256` (checked if present), `X-Persist: true\|false`. Replies 200 with the frame metadata |

Control panel endpoints (`panel.c`):

| Endpoint | Auth | Does |
|---|---|---|
| `GET /hardware` | – | SPEC §20 mappings: `display` (device, native format and stride, rotation, logical size, `blank_method`), `input` (sysfs node, `/dev/input/eventN`, name, `role` touchscreen/buttons/other, known `keys`, `modalias`), `backlight` and `power_supplies` (allowlisted sysfs attributes as raw strings, `null` if missing), `thermal_zones`, `network_interfaces` (operstate, MAC, IPv4), `audio` (`cards` from `/proc/asound/cards`, `null` if unreadable; `devices` from sysfs), `video_devices` |
| `GET /system` | – | `firmware_version`, `build`, `kernel` (uname), `uptime_s`, `memory` {`total`, `free`, `available`} in kibibytes from `/proc/meminfo` (`available` is null on 3.0), `storage` for the data dir in bytes, `time` {`now`, `plausible` (false before 2024: the clock was never set), `timezone`, `synchronized`: null} |
| `GET /logs?lines=N` | token | The last N (1–2000, default 200) lines of `--log-file` and of the kernel log (`klogctl`). Non-ASCII bytes become `?`. `kernel.available` is false with an `error` when klogctl is refused |
| `PUT /display/brightness` | token | Body `{"value": N}` (raw, 0..max_brightness) or `{"value": N, "unit": "percent"}` (0..100). Writes the first backlight's `brightness` |
| `POST /display/blank` | token | Remembers the current level and writes brightness 0. Idempotent |
| `POST /display/wake` | token | Sets `bl_power` back to 0 if it isn't, and restores the remembered level if brightness is 0 (max_brightness if tt7d never saw a level, as tt7-app does) |
| `POST /display/test-pattern` | token | Shows the built-in pattern (`tools/make-test-frame.py` output, embedded at build time) through the same decode-and-present path as `PUT /frame`, not persisted. It becomes the current frame with id `test-pattern-<16 hex>`, so the preview shows it, and counts in `frames.accepted` |
| `POST /system/reboot` | token | Syncs, replies `202 {"rebooting": true, "delay": {"value": 1, "unit": "second"}}`, and a detached grandchild runs `--reboot-cmd` (default `reboot -f`) through `/bin/sh -c` one second later |

The brightness, blank and wake replies all share one shape:
`{"on", "brightness": {value, unit: "percent", available}, "brightness_raw", "max_brightness", "wake_brightness_raw", "blank_method": "backlight"}`.

```sh
T=$(cat ~/.config/tt7/token); P=<panel-ip>
curl -s -X PUT -H "Authorization: Bearer $T" -d '{"value": 60, "unit": "percent"}' http://$P/api/v1/display/brightness
curl -s -X POST -H "Authorization: Bearer $T" -d '' http://$P/api/v1/display/blank    # -d '': POST needs Content-Length
curl -s -H "Authorization: Bearer $T" "http://$P/api/v1/logs?lines=50"
```

`frame_age_s` counts from the last PUT of the current frame, and a
deduplicated PUT resets it (SPEC §41's `last_frame_age`). If a PUT has the same
SHA-256 as the frame on screen, tt7d doesn't redraw. It updates `frame_id` and
`received_at`, and replies with `"deduplicated": true`.

Capabilities are objects with `available` true/false, or `null` when the
hardware is there but unconfirmed. `dock_detection` is `null`: nobody has
checked whether the Mains supply means "docked". `camera` is `null` with the
video4linux nodes listed, because the inventory ties `/dev/video0` to the
nt99141 sensor but no frame has been captured.
`battery_percent` carries `"estimate": true`, since the gauge jumps between
boots (gotchas.md).

### Error codes

Every error is JSON: `{"error": "<code>", "message": "...", ...}`.

| HTTP | `error` | Extra fields |
|---|---|---|
| 400 | `bad_request`, `invalid_frame_id`, `invalid_sha256`, `invalid_persist`, `invalid_brightness`, `invalid_lines` | |
| 400 | `brightness_out_of_range` | `unit`, `min`, `max` |
| 400 | `sha256_mismatch` | `header`, `computed` |
| 401 | `unauthorized` (and `WWW-Authenticate: Bearer`) | |
| 404 | `not_found`, `no_frame` | |
| 405 | `method_not_allowed` (and `Allow`) | |
| 408 | `request_timeout` | |
| 411 | `length_required` (chunked, or no Content-Length on PUT) | |
| 413 | `payload_too_large` | |
| 415 | `unsupported_media_type` | `supported` |
| 422 | `invalid_image` | `detail` (lodepng's reason) |
| 422 | `invalid_dimensions` | `expected` [w, h], `received` [w, h] |
| 431 | `headers_too_large` | |
| 500 | `persist_failed` (nothing changed on screen), `internal_error`, `reboot_failed` | |
| 500 | `backlight_write_failed` | `device`, `detail` |
| 503 | `no_backlight` | |
| 505 | `http_version_not_supported` | |

Every refused `PUT /frame` counts in `/state` `frames.rejected`, and its code
goes to `frames.last_error` (SPEC §42).

## Control panel

`GET /` serves an admin page (SPEC §29–31), not the panel's own display UI:
`tt7d/web/index.html`, `panel.css` and `panel.js`, plain HTML/CSS/JS with no
framework and nothing fetched from elsewhere. `tt7d/embed.py` compiles them,
plus the test pattern PNG, into `build/gen/tt7d_assets.c` at build time
(about 41 KB in all). It polls `/state` every 2 s and redraws the preview
from `/frame/image?v=<frame id>:<accepted>` only when the frame changes.

Sections: Overview (preview, frame, age, brightness, power, battery as an
estimate, uptime, per-interface IPs, online badge), Display (preview,
resolution, native format and stride, rotation, brightness slider, wake,
blank, test pattern), Hardware (`/hardware` as a collapsible tree), System
(`/system`; the time is flagged when the year is before 2024), Logs, and
Actions (reboot, behind a confirm dialog).

**The token.** The page itself and the read-only data need no token. Paste
the token (`/data/tt7/tt7d/token`) into the field and press Unlock: the page
first tries it on `GET /logs?lines=1` and keeps it only if tt7d accepts it. It
lives in `sessionStorage`, so it is gone when the tab closes, and goes out
only as an `Authorization: Bearer` header. It never appears in a URL, the
HTML, or tt7d's log (the e2e test checks the log). Until then the page shows
a LOCKED badge and disables every action. A 401 on any action clears the
token and locks the page again. Lock forgets it at once.

**Headers.** Every reply carries `Cache-Control: no-store`,
`X-Content-Type-Options: nosniff` and
`Content-Security-Policy: default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; frame-ancestors 'none'`.
`frame-ancestors 'none'` goes beyond the M4 brief: `default-src` does not
cover framing, and without it another site could frame the unlocked page and
trick a click on Reboot. There are no CORS headers. The page builds all text
with `textContent`; `test_assets` fails the build if `innerHTML`, inline
script or style, or `localStorage` show up.

## Building and testing

```sh
make tt7d            # build/tt7d: static ARM EABI5 for the panel
make test-host       # unit tests (render, json, http, util, sysinfo, control, hardware, assets) with ASan/UBSan
make test-e2e        # the real daemon, host-built, on a file-backed fb (tt7d/test_e2e.py)
make check           # everything, including the boot image checks
```

Run it on the host by hand with a fake framebuffer and the panel's sysfs fixture:

```sh
make build/host/tt7d
cp -r tt7d/test/fixtures/sysfs-tt7 /tmp/tt7d-sysfs     # a writable copy for brightness/blank/wake
build/host/tt7d --listen 127.0.0.1:8765 --fb-file /tmp/fb.raw --fb-geometry 800x1280x16 \
  --fb-stride 1600 --fb-format rgb565 --data-dir /tmp/tt7d-data \
  --sysfs-root /tmp/tt7d-sysfs --proc-root tt7d/test/fixtures/proc-tt7 \
  --log-file /tmp/tt7d.log --reboot-cmd 'echo reboot requested' 2>> /tmp/tt7d.log
# control panel: http://127.0.0.1:8765/
TT7_TOKEN_FILE=/tmp/tt7d-data/token tools/push-frame.sh 127.0.0.1:8765 test-frame.png
```

`tt7d --help` lists every flag. There is no config file in v1. SPEC §35's
TOML needs a parser, which is out of scope, so tt7-app passes the flags.

## On the panel

`probe/tt7-app.sh` (the image's `/usr/bin/tt7-app`) runs discovery once per
boot, starts `tt7probe log` for raw input logging (M3 will replace it), starts
Wi-Fi, and then runs `tt7d --data-dir /data/tt7/tt7d` in a loop. If tt7d exits,
it restarts after 2 s without re-running the steps before it. `/data/tt7/bin`
comes first on its PATH, so a binary copied there replaces the image's copy.

### Fast iteration without reflashing

Once the panel runs an image with this tt7-app (or the overlay in the next
section), install a new build and restart it:

```sh
make tt7d
P=root@<panel-ip>          # Wi-Fi address, or 10.55.0.1 over USB
S="-o UserKnownHostsFile=build/known_hosts"
ssh $S $P mkdir -p /data/tt7/bin
scp -O $S build/tt7d $P:/data/tt7/bin/tt7d.new
ssh $S $P 'chmod 755 /data/tt7/bin/tt7d.new && mv /data/tt7/bin/tt7d.new /data/tt7/bin/tt7d && killall tt7d'
ssh $S $P tail -n 20 /data/tt7/app.log       # "tt7-app: starting /data/tt7/bin/tt7d"
```

`scp -O` because the panel's Dropbear has scp but no sftp server. The `mv`
replaces the file in one step, so tt7-app never starts a half-copied binary.
To go back to the image's build: `ssh $S $P 'rm /data/tt7/bin/tt7d && killall tt7d'`.

Fetch the token for `tools/push-frame.sh` once:

```sh
mkdir -p ~/.config/tt7
ssh $S $P cat /data/tt7/tt7d/token > ~/.config/tt7/token && chmod 600 ~/.config/tt7/token
python3 tools/make-test-frame.py /tmp/tf.png --label "rotation 90"
tools/push-frame.sh <panel-ip> /tmp/tf.png --persist
```

### Trying it on the image that is flashed now, without a reflash

The currently flashed image (00235b6f…) runs the old tt7-app, which draws the
test pattern, and it has no tt7d. init runs `/data/tt7/app` instead of
`/usr/bin/tt7-app` when that file is executable. It only checks at boot, and
it moves the file to `/data/tt7/app.bad` if it dies within 20 s three times in
a row. So:

```sh
ssh $S $P mkdir -p /data/tt7/bin
scp -O $S build/tt7d build/tt7probe $P:/data/tt7/bin/      # the new tt7probe has `log`
scp -O $S probe/tt7-app.sh $P:/data/tt7/app
ssh $S $P 'chmod 755 /data/tt7/app /data/tt7/bin/tt7d /data/tt7/bin/tt7probe && sync'
ssh $S $P 'nohup reboot -f > /dev/null 2>&1 &'           # detached: see gotchas.md
```

To undo it: `ssh $S $P 'rm /data/tt7/app && sync'` and reboot. This reboots the
panel, so like a flash it needs Doctor Biz's go-ahead.

## Unverified

- The rotation default (90), until someone photographs the test frame on the dock.
- Colours on the real glass. The driver reports RGB565 R11/G5/B0 and tt7d
  packs exactly that, but nobody has looked at a tt7d frame on the panel yet.
  The test frame's colour bars are there to check it.
- Whether the rk fb driver needs `FBIOPAN_DISPLAY` after a write (tt7d does
  it, as tt7probe does, and ignores failure), and how visible the tearing is.
- Decode and present time on the Cortex-A9. Not measured; expect a few hundred ms.
- Dock Ethernet (`eth0`) has never been seen, so M2's "frames over Ethernet"
  is untested. Wi-Fi and USB RNDIS are the known paths.
- `external_power` → dock and `charging` semantics on the dock.
- The control panel against the real panel: it has run only against the
  host build (unit tests, e2e, and one headless Chromium session through
  agent-browser: no console or CSP errors, unlock, test pattern, blank, wake,
  brightness, reboot confirm, no horizontal scroll at 390 px wide).
- Blank/wake on the glass. Discovery (`boot-0004-up22s`) shows both
  `/sys/class/graphics/fb0/blank` (write-only, empty on read) and
  `rk28_bl` with `bl_power`. tt7d blanks by writing backlight brightness 0,
  because it is reversible and `/state` already reads it. Whether
  brightness 0 really turns the rk28_bl backlight fully dark, and what the
  fb blank ioctl does on this LCD controller, nobody has checked. Note that
  discovery read `brightness` 127 but `actual_brightness` 67.
- `reboot -f` from tt7d's detached child on the panel (the same command as
  the working ssh `nohup reboot -f &`, but not yet run from tt7d).
- `klogctl` on the panel: tt7d runs as root there, so it should work; on
  the host it is usually refused (`dmesg_restrict`).
- The wall clock: the panel has an RTC (`hym8563`), but nothing sets the time,
  so `received_at` and `time` may read 1970 or be stale. `frame_age_s` uses
  the monotonic clock and is correct regardless.
