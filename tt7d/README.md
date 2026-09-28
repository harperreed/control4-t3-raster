# tt7d: the TT7 network display daemon

tt7d shows PNG frames that a server PUTs over HTTP on the C4-TT7's framebuffer
(SPEC.md milestones M1 and M2), streams touches and button presses back over a
WebSocket (M3, see "Input (M3)" below), and serves a local admin control panel
at `/` (M4, see "Control panel" below). When no server is talking to it, it
shows its own clock (SPEC §41.1, see "Fallback clock" below). It is one static
C binary with no threads and no libraries beyond musl, a vendored PNG codec and
a vendored TrueType rasterizer.

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
| `mqtt.conf` | MQTT settings, `KEY=VALUE` lines (see "MQTT (M5) and Home Assistant (M6)"). Written by `PUT /config/mqtt` |
| `mqtt-password` | The broker password, first line, mode 0600. Never in `mqtt.conf`, the API, or the log |
| `config-revision` | The `config_revision` counter (SPEC §35), bumped by every accepted `PUT /config/mqtt` |
| `mqtt-ha-device` | The device id last announced to Home Assistant, so a changed device id gets its old entities removed |
| `tz` | Optional. First line: the fallback clock's timezone as a POSIX TZ string (see "Fallback clock"). `--tz` beats it |
| `ntp.conf` | Optional. `server HOST` lines for ntpd; tt7-app copies it to `/etc/ntp.conf` at boot (see "Fallback clock") |

## API (`/api/v1`)

Reads need no auth, except `GET /logs`, `GET /config/mqtt` and the
`GET /events` WebSocket. `PUT /frame`, `POST /heartbeat`,
`GET /logs`, both `/config/mqtt` methods and every control panel action need
`Authorization: Bearer <token>` (`/info` lists them under `auth.required_for`).
`GET /frame/image` is also unauthenticated in v1: it returns the frame that is
already visible on the glass. Revisit this when the panel shows anything private.

| Endpoint | Returns |
|---|---|
| `GET /info` | `device_id`, `model`, `firmware_version`, `build`; `display` {`width`, `height`, `rotation`, `frame_formats`, `max_frame_bytes`, `native` {`width`, `height`, `format`, `stride`, `bits_per_pixel`}}; `capabilities` read from sysfs at request time, plus `capabilities.input` from the open input devices (see "Input (M3)"); `auth` |
| `GET /state` | `time` (UTC; the clock is wrong until something sets it), `uptime_s`, `daemon_uptime_s`, `display` {`on`, `brightness`, `frame_id`, `frame_age_s`} (what the screen shows: a frame, or the fallback clock), `power`, `network.interfaces`, `fallback` {`active`, `reason` (`no_frame_since_boot`, `server_timeout` or null), `timeout_s`, `since` (null when not active)}, `clock` {`synced`, `synced_at`, `timezone`, `format` (`24h`/`12h`)}, `frames` {`accepted`, `deduplicated`, `rejected`, `last_error`}; `mqtt` {`enabled`, `connected`, `broker` (host:port, never credentials), `client_id`, `topic_base`, `last_publish`, `last_error`, `reconnects`, `dropped`}; `input` {`last_touch`, `last_button` (ISO times or null), `event_clients`, `event_clients_dropped_slow`} |
| `GET /events` | The input event stream, a WebSocket. Needs the token (see "Input (M3)") |
| `GET /config/mqtt` | The MQTT settings in effect (see below). Needs the token |
| `PUT /config/mqtt` | Body: a JSON object of MQTT settings. Needs the token and `Content-Type: application/json`; at most 4096 bytes |
| `GET /frame` | Frame metadata: `frame_id`, `sha256`, `received_at`, `displayed_at`, `width`, `height`, `content_type`, `bytes`, `persisted`, `deduplicated`, `restored`. While the fallback clock shows: its metadata, `frame_id` `fallback-clock-<unix minute>`, `received_at` null. 404 `no_frame` before the first frame when the fallback is off |
| `GET /frame/image` | The PNG exactly as received (or as restored). While the fallback clock shows: the clock as a PNG, encoded on request. 404 `no_frame` before the first frame when the fallback is off |
| `PUT /frame` | Body: a 1280×800 PNG. Headers: `Content-Type: image/png` (required), `X-Frame-ID` (1–128 printable ASCII, no spaces; generated as `tt7d-<24 hex>` if absent), `X-Frame-SHA256` (checked if present), `X-Persist: true\|false`. Replies 200 with the frame metadata |
| `POST /heartbeat` | Needs the token; send `Content-Length: 0` (`curl -d ''`). Restarts the fallback timer and replies `{"fallback": {...}}` as in `/state`. It keeps a server frame up; during the fallback it changes nothing on screen |

Control panel endpoints (`panel.c`):

| Endpoint | Auth | Does |
|---|---|---|
| `GET /hardware` | – | SPEC §20 mappings: `display` (device, native format and stride, rotation, logical size, `blank_method`), `input` (sysfs node, `/dev/input/eventN`, name, `role` touchscreen/buttons/other, known `keys`, `modalias`), `backlight` and `power_supplies` (allowlisted sysfs attributes as raw strings, `null` if missing), `thermal_zones`, `network_interfaces` (operstate, MAC, IPv4), `audio` (`cards` from `/proc/asound/cards`, `null` if unreadable; `devices` from sysfs), `video_devices` |
| `GET /system` | – | `firmware_version`, `build`, `kernel` (uname), `uptime_s`, `memory` {`total`, `free`, `available`} in kibibytes from `/proc/meminfo` (`available` is null on 3.0), `storage` for the data dir in bytes, `time` {`now`, `plausible` (false before 2024: the clock was never set), `timezone`, `synchronized` (true once tt7-ntp-hook wrote its marker this boot)} |
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
| 400 | `invalid_config` (`PUT /config/mqtt`) | `field` (null for a syntax error) |
| 409 | `set_by_flag`: that setting comes from a `--mqtt-*` flag | `field` |
| 401 | `unauthorized` (and `WWW-Authenticate: Bearer`) | |
| 404 | `not_found`, `no_frame` | |
| 405 | `method_not_allowed` (and `Allow`) | |
| 408 | `request_timeout` | |
| 411 | `length_required` (chunked, or no Content-Length on PUT) | |
| 413 | `payload_too_large` | |
| 415 | `unsupported_media_type` | `supported` |
| 422 | `invalid_image` | `detail` (lodepng's reason) |
| 422 | `invalid_dimensions` | `expected` [w, h], `received` [w, h] |
| 426 | `upgrade_required`: `GET /events` without a WebSocket upgrade (with `Upgrade` and `Sec-WebSocket-Version: 13` headers) | |
| 431 | `headers_too_large` | |
| 500 | `persist_failed` (nothing changed on screen), `write_failed` (`PUT /config/mqtt`), `internal_error`, `reboot_failed` | |
| 500 | `backlight_write_failed` | `device`, `detail` |
| 503 | `no_backlight`, `too_many_clients` (all 8 event stream slots taken) | |
| 505 | `http_version_not_supported` | |

Every refused `PUT /frame` counts in `/state` `frames.rejected`, and its code
goes to `frames.last_error` (SPEC §42).

## Input (M3)

### Design

- **Devices by capability**, not by name. tt7d lists
  `<sysfs>/class/input/inputN/eventM` and opens `<--input-dir>/eventM`
  (default `/dev/input`) read-only and non-blocking. `EVIOCGBIT` says what
  it can send:
  - **touch**: `ABS_MT_POSITION_X/Y` (multitouch protocol B if it also has
    `ABS_MT_SLOT`, else A), or `ABS_X/Y` plus `BTN_TOUCH` (single touch);
  - **buttons**: `EV_KEY` with at least one key outside the `BTN_*` ranges.

  Axis ranges and the slot count come from `EVIOCGABS`. Anything else is
  ignored. Going by the modaliases in `hardware/discovery`, the TT7 should
  give `event1` gslX680 (protocol B, 11 slots) and `event0` rk29-keypad
  (keys 114, 115, 116, 143).
- **One poll loop.** The devices and the WebSocket clients sit in the same
  `poll()` as HTTP and MQTT (`server_handlers.poll_prepare/poll_service`,
  which now take several descriptors). Nothing blocks.
- **Device loss**: a read error (`ENODEV`) or end of file closes the device.
  While there is no touchscreen or no button device, tt7d looks again every
  5 s. Pointers that were down when a touchscreen vanished get their `up`.
  After `SYN_DROPPED` (the kernel's buffer overflowed), tt7d skips events up
  to the next `SYN_REPORT`.
- **Touch state machine** (`touch.c`, pure, unit-tested): protocol B slots and
  tracking ids, protocol A contact lists (matched by tracking id, else by
  order), and single touch. The pointer id is the slot (B) or a small id
  kept for the contact's life (A). On an MT device the kernel's pointer
  emulation (`ABS_X/Y`, `BTN_TOUCH`) is ignored.
- **Throttle**: at most one `move` per pointer per 16 ms. A held-back move
  goes out when its 16 ms are up, and a pointer's final position always goes
  out right before its `up`. `down` and `up` are never delayed. No gestures
  (SPEC §14).
- **Coordinates**: raw values are scaled over their `EVIOCGABS` range onto
  the native framebuffer (x across its width, y down its height), as
  tt7probe drew its dots, which landed under the finger. Then
  `render_unmap()` turns them back by `--rotation`. It is the inverse of
  `render_map()`, which draws the frames, so the rotation math lives in one
  place. `x`/`y` are logical pixels (0..1279, 0..799); `nx`/`ny` are `x/1280`
  and `y/800`.
- **Buttons**: key code → `power` (116), `volume_up` (115), `volume_down`
  (114), else `key_<code>`. Autorepeat (value 2) is dropped. Button events
  also go to MQTT `event/button`, not retained. Touch never goes to MQTT.

### The event stream: `GET /api/v1/events` (WebSocket)

RFC 6455, version 13, on the API's port. tt7d sends text frames, one JSON
object each, never fragmented. The first is `hello`:

```json
{"type":"hello","device_id":"tt7-8c9240","width":1280,"height":800,"rotation":90,"touch":true,
 "frame_id":"smoke-frame","timestamp":"2026-09-28T12:52:50.101Z","monotonic_ms":57302211}
{"type":"touch","action":"down","pointer":0,"x":639,"y":132,"nx":0.4992,"ny":0.1650,
 "frame_id":"smoke-frame","timestamp":"2026-09-28T12:52:51.065Z","monotonic_ms":57303175}
{"type":"button","button":"power","action":"press","code":116,
 "frame_id":"smoke-frame","timestamp":"2026-09-28T12:52:51.366Z","monotonic_ms":57303476}
```

`action` is `down`, `move` or `up` for touch and `press` or `release` for
buttons. `frame_id` is what the screen showed when tt7d read the event, the
same value as `/state`'s `display.frame_id`: the frame's id, the fallback
clock's `fallback-clock-<unix minute>`, or null (nothing drawn yet, or a
restored frame whose id was lost).
`timestamp` is the wall clock (wrong until something sets the clock), and
`monotonic_ms` is `CLOCK_MONOTONIC`. tt7d takes both when it reads the event.

- **Clients**: up to 8. A ninth gets 503 `too_many_clients`.
- **Slow clients never hold anything up.** Each client has a 64 KiB send
  queue on top of a 32 KiB kernel send buffer. When an event does not fit,
  tt7d drops that client with close code 1008 and counts it in `/state`
  `input.event_clients_dropped_slow`. Frames and the other clients carry on.
  The e2e test floods a client that never reads while it PUTs frames.
- **Keepalive**: tt7d pings every 30 s and drops a client that has sent
  nothing, not even a pong, for 75 s. Browsers answer pings on their own.
- **From the client**: frames must be masked (else close 1002). Pings get
  pongs, and a close gets a close with the same code. Fragmented messages
  get close 1003, frames over 1 KiB get 1009, and text or binary messages
  are read and ignored.

**Auth (the choice).** The stream shows what people touch, so it needs the
token, like every other private endpoint. Browsers cannot set an
`Authorization` header on a WebSocket, so tt7d takes **either**
`Authorization: Bearer <token>` **or** `?token=<token>` (percent-decoded).
If the header is present, it wins. Both go through the same constant-time
compare (`token_equal`). The query string never reaches the log: tt7d logs
only the path of failed requests, and the e2e test checks the log for the
token. The control panel sends `?token=` because it has to;
`tools/events.py` sends the header unless given `--query-token`. The cost:
a URL with the token in it could end up in a proxy's or a browser
extension's logs. On a LAN with plain HTTP, anyone who can sniff the URL
can sniff the header too, so nothing that was strong gets weaker. We passed
over two alternatives: the token in `Sec-WebSocket-Protocol` (more code in
every client), and short-lived tickets from a separate POST (more state in
tt7d).

```sh
TT7_TOKEN_FILE=~/.config/tt7/token tools/events.py <panel-ip>     # one JSON line per event
tools/events.py <panel-ip> --count 5 --timeout 30                 # stop after 5 events
```

`tools/events.py` needs only the Python standard library.

### `/info` and `/state`

`/info` `capabilities.input` looks like
`{"events":"/api/v1/events","touch":{"device":"gslX680","protocol":"mt_b","pointers":11,"raw":{"x":{"min":0,"max":1280},"y":{"min":0,"max":800}},"coordinates":{"width":1280,"height":800,"space":"logical"},"move_interval_ms":16},"buttons":["volume_down","volume_up","power","key_143"]}`.
`touch` is null without a touchscreen. `buttons` lists every key the button
devices report through `EVIOCGBIT`, so a code the driver claims without a
physical button behind it shows up too. The API table covers `/state` `input`.

### Testing without evdev

evdev cannot be faked without `/dev/uinput`, which needs root. So
`--input-dir` may point at a directory of **named pipes** named like the
sysfs nodes (`event0`, `event1`) that carry native `struct input_event`
records (24 bytes each on x86-64, 16 on the panel). A pipe is not an evdev
device, so `EVIOCGBIT` and `EVIOCGABS` fail on it. tt7d then reads the
capabilities from `<sysfs>/class/input/inputN/modalias` and the axis ranges
from `<input-dir>/eventM.absinfo` (lines of `<code> <min> <max>`, e.g.
`0x35 0 1280`). On the panel the ioctls work, so neither is used. A regular
file also works and is read once. `tt7d/test_input_e2e.py` uses the panel's
sysfs fixture and the ranges tt7probe recorded on the panel.

### First run on the panel: what to look for

Run `tail -f /data/tt7/app.log` as the new tt7d starts:

```text
tt7d: input: event0 "rk29-keypad": buttons (capabilities from EVIOCGBIT): 114=volume_down 115=volume_up 116=power 143=key_143
tt7d: input: event1 "gslX680": touch, multitouch protocol B, 11 pointer(s), raw x 0..1280 y 0..800 (EVIOCGABS; capabilities from EVIOCGBIT)
```

- If a line is missing, or says `cannot open`, check `ls -l /dev/input`
  (tt7-app runs `mdev -s` to make the nodes).
- The raw ranges: tt7probe recorded x 0..1280 and y 0..800 (boot-0002),
  yet the fb is 800 wide and 1280 tall. tt7d scales them as tt7probe did
  (x across the fb width). Run `tools/events.py` and touch the four corners.
  At the right `--rotation`, the corner showing the test frame's TOP-LEFT
  label reads about (0, 0). If x and y come out swapped or mirrored, the
  driver's axes do not match the fb, and tt7d needs an axis swap/flip
  option, which does not exist yet (a code change).
- Press each button once. The `code` in each event says which key is which;
  nobody has recorded that yet (docs/hardware-inventory.md, open question 4).
- `tt7probe log` still writes the raw records to
  `/data/tt7/discovery/boot-*/input-events.log`. Compare them if an event
  looks wrong.

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
blank, test pattern), Input (once unlocked: the live event stream, newest
first, and a dot on both previews where the screen was last touched; it
reconnects with backoff), Hardware (`/hardware` as a collapsible tree), System
(`/system`; the time is flagged when the year is before 2024), Logs, and
Actions (reboot, behind a confirm dialog).

**The token.** The page itself and the read-only data need no token. Paste
the token (`/data/tt7/tt7d/token`) into the field and press Unlock: the page
first tries it on `GET /logs?lines=1` and keeps it only if tt7d accepts it. It
lives in `sessionStorage`, so it is gone when the tab closes. It goes out as
an `Authorization: Bearer` header, except in the event stream's WebSocket
URL (`?token=`), where browsers cannot set headers. It never appears in the
HTML or tt7d's log (the e2e tests check the log). Until then the page shows
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

## Fallback clock (SPEC §41.1)

"If it can't find the server it is expecting it should show a nice date,
clock on the screen. That is the failure mode." tt7d has no server address:
it only receives frames. So "server missing" means no frame since boot, or
no frame and no heartbeat for `--fallback-timeout` seconds (default 300,
0 turns the fallback off). Then tt7d draws its own screen: the time (Inter
Display Light, 300 px), the date below it ("Monday, 28 September 2026",
English names), and a status line `waiting for server · <ip>` (the Wi-Fi
address, else any non-loopback IPv4). Renders from the unit test:
`build/host/clockface-{24h,12h,unsynced}.png`.

The rules (`fallback.h`, unit-tested in `test_fallback.c`):

- Boot: the clock shows until the first accepted frame. A restored
  `last-frame.png` counts as a frame at boot, so it stays up for one timeout,
  then the clock replaces it.
- Any accepted frame PUT, a duplicate included, ends the fallback at once and
  restarts the timer. The frame is drawn even if it is the one that was on
  screen before the clock (the clock clears the frame store's `on_screen`).
- `POST /heartbeat` restarts the timer and nothing else. It keeps a server
  frame up, but during the fallback it does not bring the old frame back:
  that frame may be stale, and the server can re-send it (cheap, see dedup).
- The clock is never a server frame: no dedup against it, never persisted,
  not counted in `frames.accepted`. `/state`'s `display.frame_id` and
  `frame_age_s` (and the MQTT state's) describe what is on screen, so they
  show `fallback-clock-<unix minute>` and the age of that drawing.
- The panel's test pattern drawn during the fallback stays until the clock's
  words next change (at most a minute).

Redraws happen only when the words change: at the minute boundary, when the
clock gets synchronized, and when the IP changes (checked every 5 s). Each
redraw is one full-screen convert and copy through `display_draw` and
`display_present`, the path frames take, so rotation and pixel format are
shared. tt7d logs only the transitions into the fallback.

**Never a wrong time.** tt7d shows no time and no date until NTP has set the
clock in this boot. Until then the screen says "Setting clock…" and the
status line. "Synced" means the file `/run/tt7/ntp-synced` (`--ntp-marker`)
holds `synced <unix seconds> <action> ...`, and the clock reads 2026 or later.
The file lives on the ramdisk, so no marker survives a reboot. BusyBox
`ntpd -S tt7-ntp-hook` writes it (`probe/tt7-ntp-hook.sh`). Per BusyBox
1.36.1 `networking/ntpd.c` `run_script()`, ntpd runs the hook with
`step`, `stratum`, `periodic` or `unsync` as its argument and `stratum`,
`offset`, `freq_drift_ppm` and `poll_interval` in the environment. The hook
counts `step` as synced (ntpd sets its stratum to 16 just before that call),
and `stratum`/`periodic` when the stratum is below 16. `unsync` leaves the
marker: the clock was set this boot and drifts slowly. Why not `adjtimex`:
BusyBox ntpd sets `ADJ_OFFSET | ADJ_STATUS | ADJ_TIMECONST` and has
`ADJ_MAXERROR` commented out (ntpd.c), so the kernel's maxerror keeps
growing and, as far as I know the kernel's NTP code (not checked against the
3.0 source), it sets `STA_UNSYNC` again once maxerror passes 16 s.

**NTP servers.** tt7-app writes `/etc/ntp.conf` from `<data-dir>/ntp.conf`
if it exists, else `server 0.pool.ntp.org` … `3.pool.ntp.org`, and starts
`ntpd -n -S tt7-ntp-hook` once a default route exists. DNS comes from the
udhcpc script (`/usr/share/udhcpc/default.script` writes `/etc/resolv.conf`
from the lease). If the network has no DNS, use IP literals:

```sh
printf 'server 192.168.23.1\nserver 162.159.200.1\n' | ssh $S $P 'cat > /data/tt7/tt7d/ntp.conf'   # then reboot
```

**Timezone.** musl reads POSIX TZ strings, and the image ships no zoneinfo
files, so zone names like `America/Chicago` don't work. The default is
`CST6CDT,M3.2.0,M11.1.0` (America/Chicago: UTC−6, UTC−5 from the second
Sunday of March to the first Sunday of November, changing at 02:00). Set
another with `--tz` or the first line of `<data-dir>/tz`, then restart tt7d:

```sh
echo 'EST5EDT,M3.2.0,M11.1.0' | ssh $S $P 'cat > /data/tt7/tt7d/tz && killall tt7d'   # US Eastern
echo 'CET-1CEST,M3.5.0,M10.5.0/3' | ssh $S $P 'cat > /data/tt7/tt7d/tz && killall tt7d'   # Central Europe
echo 'UTC0' | ssh $S $P 'cat > /data/tt7/tt7d/tz && killall tt7d'
```

A value that isn't a POSIX TZ string (a name, then an offset) is refused:
`--tz` stops tt7d with exit 2; a bad `tz` file is logged and the default used.
`--clock-format 12` shows `2:05` with a smaller `PM`; the default is `14:05`.

**Heartbeat**, for a server that updates rarely:

```sh
curl -s -X POST -H "Authorization: Bearer $T" -d '' http://$P/api/v1/heartbeat
# {"fallback":{"active":false,"reason":null,"timeout_s":300,"since":null}}
```

**Font.** Inter 4.1 (SIL OFL 1.1), subset to printable ASCII plus `·` and
`…` by `tools/subset-fonts.sh`: two files, 34 KB together
(`third_party/fonts/inter/PROVENANCE`). stb_truetype v1.26 rasterizes them
(`third_party/stb/PROVENANCE`). The PNG encoder half of lodepng is now
compiled in too, for the preview.

## Building and testing

```sh
make tt7d            # build/tt7d: static ARM EABI5 for the panel
make test-host       # unit tests (render, json, http, util, sysinfo, control, hardware, assets, mqtt, ws, input,
                     # fallback, timesync, clockface) with ASan/UBSan; clockface also writes build/host/clockface-*.png
make test-e2e        # the real daemon, host-built, on a file-backed fb (tt7d/test_e2e.py, test_fallback_e2e.py)
make test-mqtt       # the real daemon against real amqtt brokers and a paho client (tt7d/test_mqtt_e2e.py)
make test-input      # input_event records through FIFOs; events out over the WebSocket and MQTT (tt7d/test_input_e2e.py)
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

`tt7d --help` lists every flag. The display settings have no config file in
v1 (SPEC §35's TOML needs a parser, which is out of scope), so tt7-app passes
the flags. MQTT is the exception: it has `mqtt.conf`, because it has to change
at runtime.

## MQTT (M5) and Home Assistant (M6)

### Design

- **Client**: our own MQTT 3.1.1 client. `mqtt_packet.c` encodes and decodes
  packets, and `mqtt_client.c` runs the connection. Nothing is vendored: the
  small C clients we know of (paho.mqtt.embedded-c's MQTTClient, MQTT-C)
  bring their own blocking socket calls or sync loop. The part we need is
  small: CONNECT, PUBLISH at QoS 0, SUBSCRIBE, PING and DISCONNECT. The unit
  tests check it byte for byte.
- **Never blocks the display** (SPEC §41, §50.17). The socket is
  non-blocking and sits in the same `poll()` as the HTTP server
  (`server_handlers.poll_prepare/poll_service`). TCP connect has a 10 s
  deadline, and so does CONNACK. A PINGREQ that gets no PINGRESP within half
  the keepalive drops the link. Reconnects back off 1, 2, 4 … 60 s. Outgoing
  packets wait in a 64 KiB queue. A publish that does not fit pushes out the
  oldest queued publishes, and `/state` `mqtt.dropped` counts them. While
  disconnected, publishes are dropped rather than queued; the state is
  retained and goes out again on connect.
- **Host names are not resolved**: `host` must be an IPv4 address.
  `getaddrinfo()` blocks, and a slow DNS lookup in the one loop would freeze
  the display.
- **Clean session**, QoS 0 throughout. A QoS 1 PUBLISH from the broker gets
  its PUBACK and is otherwise handled like QoS 0.
- **Clean close**: before a DISCONNECT, tt7d subscribes to its own
  availability topic, publishes `offline`, and waits (at most 2 s) to see it
  come back. amqtt 0.12.1 drops messages that are still queued when a
  DISCONNECT arrives, and this makes sure the broker handled everything first.
- **Logging**: connects, and only the first failure in a run of failures,
  because the log is on flash. The password is never logged.

### Settings

Sources, highest first: `--mqtt-KEY VALUE` flags, then `<data-dir>/mqtt.conf`,
then the defaults. `PUT /api/v1/config/mqtt` edits `mqtt.conf`. It refuses a
setting that a flag overrides (409 `set_by_flag`), since the flag would win
again at the next start. tt7-app passes no `--mqtt-*` flags, so on the panel
`mqtt.conf` is the source.

| Key (`mqtt.conf`, JSON, `--mqtt-` flag with `-` for `_`) | Default | Rule |
|---|---|---|
| `enabled` | `true` | `true`/`false`. With no `host`, MQTT stays off anyway |
| `host` | none | IPv4 address |
| `port` | `1883` | 1-65535 |
| `username` | none | |
| `password_file` | `<data-dir>/mqtt-password` | absolute path; not settable over the API |
| `prefix` | `tt7` | no `+`, `#`, spaces, empty levels, or leading or trailing `/` |
| `client_id` | the device id | letters, digits and `-_.:`, up to 64 |
| `keepalive` | `60` | 5-3600 s |
| `telemetry_interval` | `10` | 1-86400 s |
| `ha_discovery` | `true` | `true`/`false` |
| `allow_reboot_cmd` | `false` | `true`/`false`; gates `cmd/reboot` and the HA Reboot button |

An example `mqtt.conf` for the owner's broker (none of this is a built-in
default):

```text
enabled=true
host=192.168.23.123
port=1883
username=tt7
prefix=tt7
```

`GET /api/v1/config/mqtt` returns the settings in effect plus
`config_revision`, `password_set` (never the password), `effective_client_id`,
`set_by_flags`, and `config_error`. `config_error` says why `mqtt.conf` was
not used; in that case MQTT stays off and the display runs on.

`PUT` takes a JSON object with any subset of the keys above, plus
`"password"`: a string, or `null` to delete the password file. It checks
everything first and changes nothing if anything is wrong. Then it writes
`mqtt-password` (mode 0600) and `mqtt.conf` (temp file, fsync, rename), bumps
`config_revision`, and reconnects. Before leaving the old broker it publishes
`offline` to the availability topic. If the broker changed or discovery was
turned off, it also removes the HA entities there.

The easy way, from a machine that has the token in `~/.config/tt7/token`:

```sh
printf '%s\n' 'the-broker-password' > ~/.config/tt7/mqtt-password; chmod 600 ~/.config/tt7/mqtt-password
tools/mqtt-setup.sh <panel-ip> --broker 192.168.23.123:1883 --user tt7 \
    --password-file ~/.config/tt7/mqtt-password
```

The script sends the password in the request body on curl's stdin, and the
token through a pipe. Neither shows up in `ps`.

### Topics

Base: `<prefix>/<device id>`, e.g. `tt7/tt7-7f38a2`.

| Topic | Retained | Payload |
|---|---|---|
| `availability` | yes | `online` after connecting; the Last Will is `offline` |
| `state` | yes | JSON, every `telemetry_interval`, and within 1 s of a change to the frame id, brightness, battery, power or IPs; at once after an MQTT command or an HTTP display action. Example below |
| `sensor/<name>` | yes | plain values: `uptime_s`, `battery_percent`, `charging` (`true`/`false`), `brightness` (percent), `frame_age_s`, `wifi_ip`, `ethernet_ip`; each only when known |
| `event/boot` | no | `{"type":"boot","firmware_version":"0.1.0 (…)","uptime_s":41,"timestamp":"…"}`, once per daemon start, on the first connect |
| `event/frame` | no | `{"type":"frame","frame_id":"…","sha256":"…","deduplicated":false,"timestamp":"…"}` for each accepted `PUT /frame` |
| `event/error` | no | `{"type":"error","error":"command_disabled","command":"reboot","message":"…","timestamp":"…"}` |
| `event/button` | no | The same JSON as the WebSocket's button events (see "Input (M3)"), e.g. `{"type":"button","button":"power","action":"press","code":116,…}`. Touch stays off MQTT (the owner chose a WebSocket for it) |
| `cmd/brightness` | (in) | `NN%` (0-100), or a raw level `NN` (0 to `max_brightness`, which is 255 here), written as is |
| `cmd/wake`, `cmd/blank` | (in) | anything; the same backlight actions as `POST /display/wake` and `/display/blank` |
| `cmd/reboot` | (in) | anything; refused with `command_disabled` unless `allow_reboot_cmd=true`, else runs `--reboot-cmd` as `POST /system/reboot` does |

```json
{"time":"2026-09-28T03:04:59.746Z","uptime_s":22162,"battery_percent":82,"battery_estimate":true,
 "charging":false,"external_power":false,"brightness":50,"display_on":true,"wifi_ip":"192.168.23.197",
 "ethernet_ip":null,"frame_id":"tt7d-f9975b8e7a148cff8da65975","frame_age_s":3.3,"last_touch":null}
```

`battery_percent` is the kernel gauge's reading, which jumps between boots
(gotchas.md), and `battery_estimate` is always `true` to say so. `last_touch`
is `null` for now (SPEC §24 lists it).

Commands are only taken live. A broker replays a retained message, flagged
retained, to every new subscription, so tt7d ignores commands that arrive
with that flag; otherwise a retained `cmd/reboot` would reboot the panel on
every reconnect.

**Commands and the control panel.** MQTT reaches the display only through
`struct mqtt_actions` in `mqtt.h`. `main.c` fills it with the control panel's
actions (`panel_set_brightness`, `panel_blank`, `panel_wake`, `panel_reboot`
in `panel.h`), so an MQTT command and the matching HTTP call run the same
code, and blank then wake restores the level from before the blank either
way. A failed action answers `event/error` `command_failed`.

### Home Assistant discovery

tt7d publishes one retained config per entity at
`homeassistant/<component>/<device id>/<object>/config`. All entities belong to
one device: `identifiers` [device id], `name` "TT7 <device id>", `model`
C4-TT7, `manufacturer` "Control4 (repurposed)", and `sw_version`. Each has
`availability_topic` set to the availability topic and a `unique_id` of
`<device id>_<object>`. Entities read the `state` JSON through
`value_template`.

| Entity | When |
|---|---|
| `sensor` battery ("Battery (estimate)", device class battery, %) and `binary_sensor` charging (battery_charging) | a Battery supply is present |
| `sensor` uptime and frame age (duration, s, diagnostic) | always |
| `sensor` Wi-Fi IP / Ethernet IP (diagnostic) | `wlan0` / `eth0` exists |
| `sensor` brightness (%) | a backlight, and no `set_brightness` action (not the case in this build) |
| `number` brightness (0-100 %, slider, sends `NN%` to `cmd/brightness`) | a backlight |
| `button` wake, blank | always (the actions are wired) |
| `button` reboot (device class restart) | `allow_reboot_cmd=true` |

Every connect publishes every entity: the real config if the entity is
available, or an empty retained payload if not, which removes it. So
capabilities that disappear and `ha_discovery=false` both clean up after
themselves. tt7d also listens on `homeassistant/status` and announces again
when Home Assistant says `online`.

Sources for the field names (read 2026-09-27):
https://www.home-assistant.io/integrations/mqtt/ (discovery topic, empty
payload removes, device block, availability, birth message),
`/integrations/sensor.mqtt/` (a JSON `null` through `value_template` renders
`None`, which makes a numeric sensor `unknown`),
`/integrations/binary_sensor.mqtt/`, `/integrations/number.mqtt/`,
`/integrations/button.mqtt/`, and the `battery_charging` and `restart` device
classes on `/integrations/binary_sensor/` and `/integrations/button/`.

## On the panel

`probe/tt7-app.sh` (the image's `/usr/bin/tt7-app`) starts, all in the
background: ntpd (once a default route exists), Wi-Fi, discovery (once per
boot), and `tt7probe log` for raw input logging (evdev allows several readers,
so it runs beside tt7d's own input handling and keeps the raw records to
compare tt7d's events against). Then it runs `tt7d --data-dir /data/tt7/tt7d`
in a loop, so the fallback clock is up within seconds of boot instead of after
discovery (SPEC §38). If tt7d exits,
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
scp -O $S probe/tt7-ntp-hook.sh $P:/data/tt7/bin/tt7-ntp-hook   # images before the fallback clock lack it
scp -O $S probe/tt7-app.sh $P:/data/tt7/app
ssh $S $P 'chmod 755 /data/tt7/app /data/tt7/bin/tt7d /data/tt7/bin/tt7probe /data/tt7/bin/tt7-ntp-hook && sync'
ssh $S $P 'busybox ntpd --help 2>&1 | head -1'           # the image's BusyBox must have ntpd
ssh $S $P 'nohup reboot -f > /dev/null 2>&1 &'           # detached: see gotchas.md
```

The new tt7-app only takes effect after a reboot (init reads `/data/tt7/app`
at boot); replacing only `tt7d` needs no reboot (see above), but then nothing
starts ntpd and the clock stays on "Setting clock…".

To undo it: `ssh $S $P 'rm /data/tt7/app && sync'` and reboot. This reboots the
panel, so like a flash it needs Doctor Biz's go-ahead.

### Connecting the panel to the broker

After installing a new `build/tt7d` as above, with the token in
`~/.config/tt7/token`:

```sh
tools/mqtt-setup.sh <panel-ip> --broker 192.168.23.123:1883 --user <user> \
    --password-file ~/.config/tt7/mqtt-password     # omit --user/--password-file for an anonymous broker
curl -s http://<panel-ip>/api/v1/state | python3 -m json.tool | grep -A10 '"mqtt"'
```

## Unverified

- All of "Input (M3)" on the real panel. It has run only on the host,
  against FIFOs carrying records the test wrote. No event sequence recorded
  on the panel was replayed: the touch unit tests use sequences written from
  the kernel's multi-touch-protocol documentation and gslX680's advertised
  capabilities. Open: whether gslX680 speaks protocol B as documented, the
  touch axes against the fb (see "First run" above), which key code is
  which button, and whether KEY_WAKEUP (143) is a real button.
- The event stream in a browser: one headless Chromium session through
  agent-browser, against the host build, showed the live list and the dot.
  Safari and Firefox are untried. The CSP's `default-src 'self'` has to
  allow the same-origin `ws:` URL, which current browsers do (CSP Level 3).

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
- The wall clock: the panel has an RTC (`hym8563`), but nothing reads or
  writes it. ntpd now sets the system clock once the network is up; before
  that, `received_at` and `time` may read 1970 or 2011. `frame_age_s` uses the
  monotonic clock and is correct regardless.
- The fallback clock on the panel: ntpd, the hook and the marker have run
  only on the host (the hook under dash, not BusyBox ash). Whether the
  flashed image's BusyBox has ntpd is unchecked (`busybox ntpd --help` over
  ssh would tell); the build config has had it on. How long a redraw and the
  preview PNG take on the Cortex-A9 is not measured. How the clock looks on
  the glass in RGB565 (anti-aliased edges, the dark grey background) has not
  been seen.
- Starting ntpd only once a default route exists (a USB-only panel never gets
  one, so it never syncs and never shows a time: by design, but untested).
- MQTT has only met amqtt 0.12.1 (tests) so far, not the owner's broker at
  192.168.23.123, not mosquitto, and not the panel's network stack.
- The Home Assistant discovery payloads follow the documentation cited above
  but have not been loaded into a real Home Assistant.
