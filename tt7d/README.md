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
  lands at the fb's top-right. **The default is 270**: at 90 the test frame
  was upside down on a docked TT7 (Doctor Biz, 2026-09-28), so 270 is upright.
- **Frames**: the PNG is received in full, hashed, checked (size, SHA-256,
  dimensions), decoded into RGBA, converted into a RAM back buffer, and only
  then copied to the fb with one `memcpy`. A partial or bad frame never
  touches the screen. The fb has no second page (`yres_virtual` = `yres`), so
  that copy is not synchronized with scan-out, and a frame change may tear
  for one refresh.
- **Region updates**: tt7d keeps the frame's logical RGBA. `PATCH /frame`
  (SPEC §10.1, "Region updates" below) decodes a batch of PNG rectangles,
  and only when all of them are good copies them into that RGBA, converts
  just those rectangles into the back buffer, and copies just their native
  rows to the fb.
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
| `config-revision` | The `config_revision` counter (SPEC §35), bumped by every accepted `PUT /config/mqtt` and `PUT /config/camera` |
| `camera.conf` | Camera settings, `KEY=VALUE` lines (see "Camera"). Written by `PUT /config/camera`. No picture is ever written to the data dir |
| `mqtt-ha-device` | The device id last announced to Home Assistant, so a changed device id gets its old entities removed |
| `tz` | Optional. First line: the fallback clock's timezone as a POSIX TZ string (see "Fallback clock"). `--tz` beats it |
| `ntp.conf` | Optional. `server HOST` lines for ntpd; tt7-app copies it to `/etc/ntp.conf` at boot (see "Fallback clock") |
| `update-pubkey` | Optional. An ed25519 public key as 64 hex digits (`tools/make-bundle.sh --pubkey-of KEY.pem`). Once it exists, only bundles signed with its key install; if it cannot be parsed, no bundle installs (see "Web update") |

Web update keeps its own files under `/data/tt7` (`--update-root`), not in the
data dir: `releases/` and `update/` (see "Web update (SPEC M8)").

## API (`/api/v1`)

Reads need no auth, except `GET /logs`, `GET /config/mqtt`, the
`GET /events` WebSocket and every camera route. `PUT /frame`, `PATCH /frame`, `POST /heartbeat`,
`GET /logs`, both `/config/mqtt` methods, `GET /camera/snapshot`, both
`/config/camera` methods and every control panel action need
`Authorization: Bearer <token>` (`/info` lists them under `auth.required_for`).
`GET /frame/image` is also unauthenticated in v1: it returns the frame that is
already visible on the glass. Revisit this when the panel shows anything private.

| Endpoint | Returns |
|---|---|
| `GET /info` | `device_id`, `model`, `firmware_version`, `build`; `display` {`width`, `height`, `rotation`, `frame_formats`, `frame_patch` {`content_type`, `version`, `max_regions`}, `max_frame_bytes`, `native` {`width`, `height`, `format`, `stride`, `bits_per_pixel`}}; `capabilities` read from sysfs at request time, plus `capabilities.input` from the open input devices (see "Input (M3)"); `auth` |
| `GET /state` | `time` (UTC; the clock is wrong until something sets it), `uptime_s`, `daemon_uptime_s`, `display` {`on`, `brightness`, `frame_id`, `frame_age_s`} (what the screen shows: a frame, or the fallback clock), `power`, `network.interfaces`, `fallback` {`active`, `reason` (`no_frame_since_boot`, `server_timeout` or null), `timeout_s`, `since` (null when not active)}, `clock` {`synced`, `synced_at`, `timezone`, `format` (`24h`/`12h`)}, `frames` {`accepted`, `region_updates` (accepted PATCHes that changed pixels), `deduplicated`, `rejected`, `last_error`}; `mqtt` {`enabled`, `connected`, `broker` (host:port, never credentials), `client_id`, `topic_base`, `last_publish`, `last_error`, `reconnects`, `dropped`}; `input` {`last_touch`, `last_button` (ISO times or null), `event_clients`, `event_clients_dropped_slow`}; `camera` (see "Camera"); `update` {`release` (the web-installed release tt7d runs from, null for the image's own build), `restart_pending`} (the rest is at `GET /system/update`) |
| `GET /events` | The input event stream, a WebSocket. Needs the token (see "Input (M3)") |
| `GET /config/mqtt` | The MQTT settings in effect (see below). Needs the token |
| `PUT /config/mqtt` | Body: a JSON object of MQTT settings. Needs the token and `Content-Type: application/json`; at most 4096 bytes |
| `GET /camera/snapshot` | A 1280×720 JPEG (quality 80) from the camera. Needs the token. 503 when the camera is off or cannot deliver (see "Camera") |
| `GET /config/camera` | The camera settings in effect. Needs the token |
| `PUT /config/camera` | Body: a JSON object of camera settings. Needs the token and `Content-Type: application/json`; at most 4096 bytes |
| `GET /frame` | Frame metadata: `frame_id`, `sha256` (see "Region updates"), `received_at`, `displayed_at`, `width`, `height`, `content_type`, `bytes` (the body of the PUT or PATCH that made the frame), `persisted`, `deduplicated`, `restored`, `updated_via` (`full` or `regions`), `regions` (null after a PUT). While the fallback clock shows: its metadata, `frame_id` `fallback-clock-<unix minute>`, `received_at` null. 404 `no_frame` before the first frame when the fallback is off |
| `GET /frame/image` | The PNG exactly as received (or as restored). After a PATCH: the composed frame, encoded as PNG on the first request and kept until the frame changes. While the fallback clock shows: the clock as a PNG, encoded on request. 404 `no_frame` before the first frame when the fallback is off |
| `PUT /frame` | Body: a 1280×800 PNG. Headers: `Content-Type: image/png` (required), `X-Frame-ID` (1–128 printable ASCII, no spaces; generated as `tt7d-<24 hex>` if absent), `X-Frame-SHA256` (checked if present), `X-Persist: true\|false`. Replies 200 with the frame metadata |
| `PATCH /frame` | Body: a region container (SPEC §10.1), `Content-Type: application/x-tt7-regions`. Headers: `X-Base-Frame-ID` (required: the frame it applies to), `X-Frame-ID`, `X-Frame-SHA256` (the resulting frame's, checked if present), `X-Persist` (only false). All regions or none; 409 `base_mismatch` unless the base is on screen. Replies 200 with the frame metadata |
| `POST /heartbeat` | Needs the token; send `Content-Length: 0` (`curl -d ''`). Restarts the fallback timer and replies `{"fallback": {...}}` as in `/state`. It keeps a server frame up; during the fallback it changes nothing on screen |

Control panel endpoints (`panel.c`; the backlight itself is `backlight.c`):

| Endpoint | Auth | Does |
|---|---|---|
| `GET /hardware` | – | SPEC §20 mappings: `display` (device, native format and stride, rotation, logical size, `blank_method`), `input` (sysfs node, `/dev/input/eventN`, name, `role` touchscreen/buttons/other, known `keys`, `modalias`), `backlight` and `power_supplies` (allowlisted sysfs attributes as raw strings, `null` if missing), `thermal_zones`, `network_interfaces` (operstate, MAC, IPv4), `audio` (`cards` from `/proc/asound/cards`, `null` if unreadable; `devices` from sysfs), `video_devices` |
| `GET /system` | – | `firmware_version`, `build`, `kernel` (uname), `uptime_s`, `memory` {`total`, `free`, `available`} in kibibytes from `/proc/meminfo` (`available` is null on 3.0), `storage` for the data dir in bytes, `time` {`now`, `plausible` (false before 2024: the clock was never set), `timezone`, `synchronized` (true once tt7-ntp-hook wrote its marker this boot)} |
| `GET /logs?lines=N` | token | The last N (1–2000, default 200) lines of `--log-file` and of the kernel log (`klogctl`). Non-ASCII bytes become `?`. `kernel.available` is false with an `error` when klogctl is refused |
| `PUT /display/brightness` | token | Body `{"value": N}` (raw, 0..max_brightness) or `{"value": N, "unit": "percent"}` (0..100). Writes the first backlight's `brightness`. A 0 (raw, or a percent that rounds to 0) is written as 1: the TT7's `rk28_bl` driver treats brightness 0 as a fallback to full bright. While blank the level is only stored: the screen stays dark and wake applies it |
| `POST /display/blank` | token | Remembers the current level and writes `bl_power` 4 (FB_BLANK_POWERDOWN), leaving `brightness` alone. A backlight without `bl_power` (not the TT7) falls back to writing brightness 0. Idempotent |
| `POST /display/wake` | token | Writes the remembered level to `brightness` if it differs (a level set while blank, or max_brightness if tt7d never saw one, as tt7-app does), then `bl_power` 0. Without `bl_power`: the level, if brightness is 0. Awake already: no change |
| `POST /display/test-pattern` | token | Shows the built-in pattern (`tools/make-test-frame.py` output, embedded at build time) through the same decode-and-present path as `PUT /frame`, not persisted. It becomes the current frame with id `test-pattern-<16 hex>`, so the preview shows it, and counts in `frames.accepted` |
| `POST /system/reboot` | token | Syncs, replies `202 {"rebooting": true, "delay": {"value": 1, "unit": "second"}}`, and a detached grandchild runs `--reboot-cmd` (default `reboot -f`) through `/bin/sh -c` one second later |

The brightness, blank and wake replies all share one shape:
`{"on", "brightness": {value, unit: "percent", available}, "brightness_raw", "max_brightness", "wake_brightness_raw", "blank_method"}`.
`blank_method` is `"bl_power"`, or `"brightness"` for a backlight without a
`bl_power` file; tt7d decides at startup. `on` is false while `bl_power` is
non-zero (or brightness is 0). `brightness` is the stored level while blank
(what wake restores), so it matches `wake_brightness_raw`; `brightness_raw`
is always the sysfs file. `/state` and MQTT `state` report `on` and
`brightness` the same way.

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
checked whether the Mains supply means "docked". `camera` comes from
camera.c: `available` (the `--camera-dev` node exists) and `enabled` apart
(see "Camera").
`battery_percent` carries `"estimate": true`, since the gauge jumps between
boots (gotchas.md).

### Error codes

Every error is JSON: `{"error": "<code>", "message": "...", ...}`.

| HTTP | `error` | Extra fields |
|---|---|---|
| 400 | `bad_request`, `invalid_frame_id`, `invalid_sha256`, `invalid_persist`, `invalid_brightness`, `invalid_lines` | |
| 400 | `brightness_out_of_range` | `unit`, `min`, `max` |
| 400 | `sha256_mismatch` | `header`, `computed` |
| 400 | `PATCH /frame`: `missing_base_frame_id`, `persist_not_supported`, `invalid_regions`, `unsupported_regions_version` | `region` (when one is at fault) |
| 400 | `too_many_regions` | `max` |
| 409 | `base_mismatch`: `X-Base-Frame-ID` is not the frame on screen (restart, another sender, the fallback clock) | `current_frame_id` |
| 422 | `region_out_of_bounds`, `regions_too_large` | `region` (out of bounds) |
| 422 | `region_size_mismatch` | `region`, `expected` [w, h], `received` [w, h] |
| 400 | `invalid_config` (`PUT /config/mqtt`, `PUT /config/camera`) | `field` (null for a syntax error) |
| 409 | `set_by_flag`: that setting comes from a `--mqtt-*` or `--camera` flag | `field` |
| 400 | Web update, the bundle refused: `invalid_bundle` (not ustar, bad checksum, truncated), `bad_member_type` (link, device, directory, FIFO, pax or GNU header), `unsafe_path` (absolute or `..`), `unknown_file` (not an allowed path), `duplicate_member`, `no_manifest`, `invalid_manifest`, `missing_file` (listed, not in the tar), `unlisted_file` (in the tar, not listed) | `member` (when one is at fault) |
| 400 | `hash_mismatch` | `member`, `expected`, `computed` |
| 403 | `signature_required`, `invalid_signature` (an `update-pubkey` is set) | `member` |
| 409 | `already_current`, `downgrade_refused` (with `--update-refuse-downgrade`), `restart_pending` (an update or rollback was just applied), `no_previous` (rollback) | `release`; `version`, `running_version` |
| 401 | `unauthorized` (and `WWW-Authenticate: Bearer`) | |
| 404 | `not_found`, `no_frame` | |
| 405 | `method_not_allowed` (and `Allow`) | |
| 408 | `request_timeout` | |
| 411 | `length_required` (chunked, or no Content-Length on PUT, POST or PATCH) | |
| 413 | `payload_too_large` | `max_bytes` (web update: 4 MiB) |
| 415 | `unsupported_media_type` | `supported` |
| 422 | `invalid_image` | `detail` (lodepng's reason); `region` for a PATCH |
| 422 | `invalid_dimensions` | `expected` [w, h], `received` [w, h] |
| 426 | `upgrade_required`: `GET /events` without a WebSocket upgrade (with `Upgrade` and `Sec-WebSocket-Version: 13` headers) | |
| 431 | `headers_too_large` | |
| 500 | `persist_failed` (nothing changed on screen), `write_failed` (`PUT /config/mqtt`), `internal_error`, `reboot_failed` | |
| 500 | `backlight_write_failed` | `device`, `detail` |
| 500 | `install_failed`, `rollback_failed` (web update; `message` says which step), `invalid_pubkey` | |
| 503 | `no_backlight`, `too_many_clients` (all 8 event stream slots taken) | |
| 503 | `camera_disabled`, `camera_unavailable` (no device), `camera_busy` (worker restarting, or 4 requests already waiting), `camera_failed` (the worker reported an error or exited), `camera_timeout` (the worker went silent and was killed) | |
| 503 | `insufficient_memory` (web update: checked before the body is read) | `available_bytes`, `needed_bytes` |
| 507 | `insufficient_storage` (web update: /data would drop below `--update-min-free-bytes`) | `free_bytes`, `needed_bytes` |
| 505 | `http_version_not_supported` | |

Every refused `PUT /frame` or `PATCH /frame` counts in `/state`
`frames.rejected`, and its code goes to `frames.last_error` (SPEC §42).

### Region updates (`PATCH /frame`, SPEC §10.1)

The body is `"TT7R"`, version 1, a reserved 0 byte, a u16 count (0..16), one
12-byte record per region (x, y, w, h as u16, png_len as u32; big-endian,
logical coordinates), then the PNGs in record order. SPEC §10.1 has the
table and every refusal.

- **Base.** `X-Base-Frame-ID` must be the frame on screen. A restarted tt7d
  has only its restored frame (if any, under its persisted id), the
  fallback clock covers the frame, and a PUT from anyone else replaces it:
  each answers 409 `base_mismatch` with `current_frame_id`. The sender then
  PUTs the whole frame.
- **All or nothing.** Every region is decoded (lodepng) and size-checked
  before a pixel changes; then they are copied into the kept logical RGBA
  in order (later wins), and only their rectangles are converted and copied
  to the fb, through the same rotation code as a PUT.
- **`sha256`** after a PATCH is `sha256(<base sha256 as 64 lowercase hex> +
  <body>)`, not a hash of the pixels: hashing 4 MB of RGBA on every tap would
  cost more than the patch saves. Both sides compute it; the server sends it
  as `X-Frame-SHA256`. An empty batch keeps the hash, changes the id, and
  replies `deduplicated: true`.
- **Not persisted.** `X-Persist: true` gets 400 `persist_not_supported`.
- The golden request body `test/fixtures/regions-v1.bin` (with
  `regions-v1.txt`) is checked by `test_regions.c` and by the server's Go
  tests.

```sh
# one 200x100 region at (1000, 650) onto the frame "f1"; tools/ has no helper, this is the layout by hand
python3 - <<'PY' > /tmp/patch.bin
import struct, sys
png = open("button.png", "rb").read()   # exactly 200x100
sys.stdout.buffer.write(b"TT7R" + bytes([1, 0]) + struct.pack(">HHHHHI", 1, 1000, 650, 200, 100, len(png)) + png)
PY
curl -s -X PATCH -H "Authorization: Bearer $T" -H "Content-Type: application/x-tt7-regions" \
     -H "X-Base-Frame-ID: f1" -H "X-Frame-ID: f2" --data-binary @/tmp/patch.bin http://$P/api/v1/frame
```

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

Sections: Overview (preview, frame, age, fallback clock state and NTP sync,
brightness, power, battery as an
estimate, uptime, per-interface IPs, online badge), Display (preview,
resolution, native format and stride, rotation, brightness slider, wake,
blank, test pattern), Input (once unlocked: the live event stream, newest
first, and a dot on both previews where the screen was last touched; it
reconnects with backoff), Hardware (`/hardware` as a collapsible tree), System
(`/system`; the time is flagged when the year is before 2024), Logs, Update
(`web/update.js`: upload a bundle with a progress bar, the running, current and
previous release, trial, history, rollback behind a confirm dialog; see "Web
update"), and Actions (reboot, behind a confirm dialog).

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
make test-camera     # the real daemon and camera worker on NV12 frames from a FIFO (tt7d/test_camera_e2e.py)
make test-update     # web update: bundles from tools/make-bundle.sh into the real daemon (tt7d/test_update_e2e.py),
                     # and tt7-app.sh's release selection under BusyBox sh (probe/test_tt7_app.py)
make bundle          # build/tt7-bundle-<build>.tar from the ARM builds, for the Update section
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
| `cmd/brightness` | (in) | `NN%` (0-100), or a raw level `NN` (0 to `max_brightness`, which is 255 here), written as is, except that 0 becomes 1 and a level sent while blank waits for wake (as `PUT /display/brightness`) |
| `cmd/wake`, `cmd/blank` | (in) | anything; the same backlight actions as `POST /display/wake` and `/display/blank` |
| `cmd/reboot` | (in) | anything; refused with `command_disabled` unless `allow_reboot_cmd=true`, else runs `--reboot-cmd` as `POST /system/reboot` does |

```json
{"time":"2026-09-28T03:04:59.746Z","uptime_s":22162,"battery_percent":82,"battery_estimate":true,
 "charging":false,"external_power":false,"brightness":50,"display_on":true,"wifi_ip":"192.168.23.197",
 "ethernet_ip":null,"frame_id":"tt7d-f9975b8e7a148cff8da65975","frame_age_s":3.3,"last_touch":null}
```

`battery_percent` is the kernel gauge's reading, which jumps between boots
(gotchas.md), and `battery_estimate` is always `true` to say so. `last_touch`
is the wall time of the last touch (as `/state`'s `input.last_touch`), null
before the first. A touch alone does not publish the state; the next
change or telemetry interval carries it.

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
| `camera` camera (`topic` `<base>/camera/image`, raw JPEG) | the camera is on (see "Camera") |
| `binary_sensor` presence (device class occupancy, `state_topic` `<base>/presence`, ON/OFF) | the camera and presence are on |

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
The camera entities (read 2026-09-28): `/integrations/camera.mqtt/` (`topic`
carries raw image bytes unless `image_encoding` is `b64`; Home Assistant's
`mqtt/camera.py` subscribes with `disable_encoding=True`), and the
`occupancy` device class on `/integrations/binary_sensor/` ("on means
occupied (detected)").

## Camera

The camera is an owner-approved extension to SPEC §51 ("3. we should do a,
and b", 2026-09-27): (a) snapshots, (b) presence that wakes the display. It
uses cam/'s capture, JPEG and motion code, which `tt7cam snap` proved on the
panel (gotchas.md). **It is off by default.**

### Design

- **A worker process.** tt7d forks a child (`camera_worker.c`) that alone
  opens `/dev/video0` and `/dev/ion`. The two talk over a socketpair
  (`camera_proto.h`: an 8-byte type/length header, then the payload), which
  tt7d reads from its poll loop without ever blocking. Why a process and
  not a thread: the risk is the kernel driver itself (a DQBUF that never
  returns, a crash in the ion or IPP path), and a thread stuck in the driver
  cannot be killed without killing tt7d. A process can be, and a crash in it
  leaves the display and the HTTP server running. Its only cost is a fork
  and one copy of each JPEG over the socket. The child closes every
  descriptor it inherited except its socket, and `PR_SET_PDEATHSIG` stops it
  if tt7d dies.
- **Only the worker touches the camera.** On-request mode (presence off)
  opens the camera for each snapshot: `settle_frames` frames for
  auto-exposure (29 by default: tt7cam snap's 30 frames with the last kept),
  then one kept frame, then `cam_close`. Presence mode opens it once and keeps
  one streaming session. Every `presence_interval_ms` it queues a buffer,
  takes the frame, scores it with motion.c (32×18 grid means of the luma,
  which is the downsampling) and sends a tick. A snapshot request while
  presence runs gets the frame just scored (at most one interval old) and
  never opens the device a second time.
- **Supervision.** A worker that sends nothing for `worker_timeout_s` (in
  presence mode it ticks every interval; in on-request mode the clock runs
  only while a snapshot is asked for) gets SIGTERM. The worker's handler
  makes its capture loop end through `cam_close`: STREAMOFF, then the ion
  buffer is unmapped and freed. 1 s later it gets SIGKILL, and tt7d reaps it
  with `waitpid(WNOHANG)` and logs each step. After SIGKILL the kernel
  releases the video and ion descriptors on exit. Whether this driver
  does a STREAMOFF in its release path is unverified. A worker stuck in
  uninterruptible sleep cannot be reaped; tt7d then says so once in the log
  and `last_error`, and starts no second worker while the first holds the
  device. Failed workers restart after 1 s, doubling to 60 s.
- **Snapshots stay in memory.** The latest JPEG lives in tt7d's memory and
  is served again for `snapshot_max_age_s` (2 s), which also rate-limits
  captures. Turning the camera off frees it. No picture is ever written to
  /data. A snapshot request whose capture has not finished waits in the
  camera module (the server's `take_over` hook, as the WebSocket does) and is
  answered when the JPEG arrives. At most 4 requests wait at once.
- **Presence and the display** (`presence.c`). On arrival, with
  `presence_wake` on and the display blank, presence calls the same
  `panel_wake` as `POST /display/wake`. With `presence_idle_blank_s` > 0 it
  blanks again that many seconds after presence ends, but only a display
  that presence itself woke. A display someone woke or blanked by hand in
  the meantime is left alone. 0 (the default) never blanks. When the
  presence worker stops or dies, presence reports gone.

### Settings (`<data-dir>/camera.conf`)

`KEY=VALUE` lines like mqtt.conf, written by `PUT /api/v1/config/camera`
(whose JSON uses the same names, except `enabled` for `camera`). `--camera
on|off` overrides `camera` and then locks it (`PUT` gets 409 `set_by_flag`).

| Key (JSON) | Default | Meaning |
|---|---|---|
| `camera` (`enabled`) | off | the camera at all |
| `presence` | off | presence detection (a streaming worker) |
| `presence_wake` | off | presence wakes a blank display |
| `presence_idle_blank_s` | 0 | blank a display presence woke after this long without presence; 0 = never |
| `presence_interval_ms` | 500 | time between presence frames (100-10000) |
| `presence_threshold` | 8 | motion score (mean luma deviation) that turns presence on; off below half, after 3 quiet frames |
| `snapshot_interval` | 0 | seconds between snapshots published to MQTT; 0 = only on `cmd/snapshot` |
| `snapshot_max_age_s` | 2 | a snapshot younger than this is served from memory |
| `settle_frames` | 29 | frames dropped for auto-exposure before an on-request snapshot's frame |
| `worker_timeout_s` | 10 | silence before the worker is killed (2-600) |

Changing `camera`, `presence`, the interval, threshold or settle frames
restarts the worker (SIGTERM, a clean exit, a new one). Each accepted PUT
bumps the device-wide `config_revision`.

```sh
T=$(cat ~/.config/tt7/token); P=<panel-ip>
curl -s -X PUT -H "Authorization: Bearer $T" -H 'Content-Type: application/json' \
     -d '{"enabled": true}' http://$P/api/v1/config/camera
curl -s -H "Authorization: Bearer $T" -o /tmp/tt7-snap.jpg -D - http://$P/api/v1/camera/snapshot
# HTTP/1.1 200 OK, Content-Type: image/jpeg, X-Captured-At: 2026-09-28T15:02:03.243Z
curl -s -X PUT -H "Authorization: Bearer $T" -H 'Content-Type: application/json' \
     -d '{"presence": true, "presence_wake": true, "presence_idle_blank_s": 300}' http://$P/api/v1/config/camera
```

### Reported

- `/info` `capabilities.camera`: `{"available": true, "enabled": false,
  "presence": false, "device": "/dev/video0", "test_source": false,
  "video4linux_devices": ["video0"], "snapshot": {"path":
  "/api/v1/camera/snapshot", "format": "image/jpeg", "width": 1280,
  "height": 720, "quality": 80}, "config": "/api/v1/config/camera"}`.
  `available` means the device node exists; `enabled` is the setting.
- `/state` `camera`: `{"enabled", "presence_enabled", "present" (null until
  the detector has scored a frame, or while presence is off),
  "presence_changed_at", "last_snapshot_at", "worker" ("off", "running",
  "stopping", "restarting"), "worker_pid", "worker_restarts",
  "frames_scored", "last_error"}`.
- WebSocket `GET /api/v1/events`: `{"type": "presence", "present": true,
  "score": 23.41, "timestamp": "...", "monotonic_ms": ...}` on each change.
- MQTT, under `tt7/<device id>/`: `presence` (retained `ON`/`OFF`; an empty
  retained payload clears it while presence is off), `camera/image` (a raw
  JPEG, not retained) after `cmd/snapshot` or every `snapshot_interval`
  seconds, and `event/error` `{"type": "error", "error": "camera_disabled" |
  "camera_busy", "command": "snapshot", ...}` when a `cmd/snapshot` cannot be
  served. Home Assistant gets a `camera` entity while the camera is on and an
  occupancy `binary_sensor` while presence is on; both are removed (empty
  retained configs) when turned off. HA's MQTT camera does not ask for
  pictures, so it shows one only after `cmd/snapshot` or with
  `snapshot_interval` set. tt7d's MQTT send queue is 512 KiB so a snapshot
  fits; a bigger JPEG is dropped and logged.
- The control panel's Camera section: the switches (with the token), "Take
  snapshot" (an authenticated fetch, shown as a `data:` URL, which the
  existing `img-src 'self' data:` allows, so the CSP did not change), and the
  presence state. The picture leaves the page when it is locked.

### Testing without the camera

`--camera-fake-source PATH` (**test only**, never set on the panel) makes the
worker read 1280×720 NV12 frames from a file (looped) or a FIFO instead of
the camera. Everything after the frame source is the real code.
`test_camera_e2e.py` feeds a FIFO from a thread: an empty room, or a bright
block for "someone". A read from the fake source ignores signals the way a
stuck driver call would, so pausing the feeder makes a genuinely stuck worker
that only SIGKILL ends. tt7d logs `camera: TEST MODE` when the flag is set,
and `/info` shows `test_source: true`.

### Trying it on the panel

With a new `build/tt7d` installed (see "Fast iteration") and the token in
`~/.config/tt7/token`:

1. `curl -s http://<panel-ip>/api/v1/info | python3 -m json.tool | grep -A12 '"camera"'`:
   `available` true, `enabled` false. `GET /camera/snapshot` gives 503
   `camera_disabled`.
2. Enable (above), then `ssh $S $P tail -f /data/tt7/app.log`: `camera: worker
   N started (snapshots on request, /dev/video0)`. Take a snapshot with the
   curl above. It should take about 2-3 s (30 frames), then come from memory
   within 2 s. Look at `/tmp/tt7-snap.jpg`, then delete it (gotchas.md
   privacy rule). On failure the worker writes capture.c's step log to
   app.log. `dmesg` noise like "Format is Invalidate" and "get cif ldo
   failed!" is normal for this driver. Anything with `BUG`, `Oops` or
   `rk29_vipmem` is not: stop there.
3. `ssh $S $P 'grep -i -E "vipmem|camera" /proc/iomem'` before and after a
   snapshot in on-request mode. `rk_camera_vb` is our buffer while the driver
   maps it (tt7cam probe greps for the same regions); it should be gone
   afterwards, because the worker closes the camera each time.
4. Presence: PUT `{"presence": true}`. `/state` `camera.frames_scored` should
   climb about 2 per second and `present` should become false. Walk in front
   of the panel: `present` true; `tools/events.py` (or the panel's Input
   section) shows the presence event; MQTT `tt7/<id>/presence` goes `ON`.
   Blank the display, walk away and back: with `presence_wake` on it lights
   up. Leave presence running for a while and watch dmesg and the log: a
   `gave no sign of life` line means the streaming session stalled (see
   "Unverified").
5. Kill test, with presence on: `ssh $S $P kill -STOP <worker pid>` (from
   `/state camera.worker_pid`): after `worker_timeout_s` the log shows SIGTERM,
   SIGKILL, `reaped (killed by signal 9)`, and a new worker. Frames and the
   control panel keep working throughout.
6. Off: PUT `{"enabled": false}`. The worker exits with a clean STREAMOFF (no
   `reaped` line), and `/state` `camera.last_snapshot_at` becomes null.

## Web update (SPEC M8)

Upload a bundle of tt7d and its helpers from the control panel's Update
section (or `PUT /api/v1/system/update`). The panel installs it beside the
running build, restarts into it, and goes back by itself if it does not stay
up. The boot image, `/system`, init and its `/data/tt7/init.overlay` are never
touched: a bundle only writes under `/data/tt7/releases` and `/data/tt7/update`.
Updating the boot image over the web is deferred.

### The bundle

`make bundle` (or `tools/make-bundle.sh`) writes `build/tt7-bundle-<build>.tar`:
a ustar tar of regular files only (BusyBox tar extracts it; the e2e test checks
that) holding `bin/tt7d`, `bin/tt7probe`, `bin/tt7-ntp-hook`, `app`
(`probe/tt7-app.sh`) and `manifest.json`:

```json
{"format": "tt7-bundle", "version": "0.1.0", "build": "d31d215", "created": "2026-09-28T14:55:23Z",
 "files": [{"path": "bin/tt7d", "sha256": "…", "mode": "0755"}, …]}
```

`build` (from `build/tt7d.version`, the `git describe` tt7d was built with) is
the release id and directory name. tt7d refuses anything else in the tar:
links, devices, directories, absolute or `..` paths, unknown names, a file the
manifest lists but the tar lacks or the other way round, and any file whose
SHA-256 differs. `bin/tt7d` and `app` are required. Modes are `0755` or `0644`.

**Signing (optional).** Put a public key on the panel and from then on only
bundles signed with it install; unsigned ones get 403 `signature_required`:

```sh
openssl genpkey -algorithm ed25519 -out ~/.config/tt7/update-key.pem    # keep it off the panel
tools/make-bundle.sh --pubkey-of ~/.config/tt7/update-key.pem > /tmp/update-pubkey
scp -O $S /tmp/update-pubkey $P:/data/tt7/tt7d/update-pubkey
tools/make-bundle.sh --sign ~/.config/tt7/update-key.pem                # adds manifest.sig
```

`manifest.sig` is 128 hex digits: ed25519 (RFC 8032) over `manifest.json`'s
exact bytes, which in turn pin every file's SHA-256. tt7d checks it with
vendored TweetNaCl (`third_party/tweetnacl/PROVENANCE`: public domain, one
file, SHA-512 included; checked against the RFC 8032 test vectors). Without a
key, the admin token plus the per-file SHA-256 is all the protection there is:
the hashes catch a damaged upload, not a malicious one from someone who has
the token.

### API

| Endpoint | Auth | Does |
|---|---|---|
| `GET /system/update` | – | `running` {`release` (null: the image's own build), `version`, `build`}, `current` and `previous` ({`release`, `version`, `build`, `created`, `usable`} or null), `trial` ({`release`, `starts`} or null), `releases` (dirs on the panel), `restart_pending`, `history` (last 20 events, oldest first: {`time`, `event`, `release`, `detail`}), `last_result` (the newest event), `last_error` (the last refused update request since tt7d started, in RAM only: {`time`, `error`, `status`}), `policy` {`max_bytes`, `signature_required`, `refuse_downgrade`, `keep_releases`, `min_free_bytes`, `confirm_after_s`} |
| `PUT /system/update` | token | Body: the tar, `Content-Type: application/x-tar`, at most 4 MiB. Before reading the body tt7d checks the token, type, size and free RAM (MemFree + Buffers + Cached must cover 3 × the body + 16 MiB). It holds the body in RAM, verifies everything there, and only then writes to /data. Replies `202 {"release", "version", "previous", "signed", "install_ms", "restarting": true, "delay"}`, then exits with status 75 a second later |
| `POST /system/update/rollback` | token | Swaps `current` and `previous` (the one rolled back to runs under trial), `202 {"release", "previous", "restarting": true, "delay"}`, exits 75. 409 `no_previous` if there is none |

```sh
T=$(cat ~/.config/tt7/token)
curl -s -X PUT -H "Authorization: Bearer $T" -H 'Content-Type: application/x-tar' \
     --data-binary @build/tt7-bundle-$(cat build/tt7d.version).tar http://$P/api/v1/system/update
curl -s http://$P/api/v1/system/update | python3 -m json.tool
```

Flags: `--update-root` (default `/data/tt7`), `--update-confirm-after S`
(default 30), `--update-min-free-bytes N` (default 8 MiB left free after the
install, else 507), `--update-refuse-downgrade` (off by default, since this is
a dev device: refuses a bundle whose `version` is below the running tt7d's).

### On disk

```
/data/tt7/releases/<id>/        app, bin/tt7d, bin/tt7probe, bin/tt7-ntp-hook, manifest.json
/data/tt7/update/current        "<id>": what tt7-app runs. Absent: the image's own build
/data/tt7/update/previous       "<id>": what current replaced; the rollback target
/data/tt7/update/trial          "<id> <starts>": not confirmed yet
/data/tt7/update/history        one line per installed / confirmed / rollback_requested / rolled_back / pruned
```

Install: the files go into `releases/.incoming-<id>/` (each written, `fchmod`ed
and `fsync`ed, then both directories `fsync`ed), which is renamed to
`releases/<id>/`. Then `update/trial`, `update/current` and `update/previous`
are each replaced by write-temp, fsync, rename, and `update/` is fsynced. The
trial goes first: a power cut before `current` changes leaves a trial for a
release that is not current, which tt7-app drops. At most 3 release
directories stay; the oldest others go, never `current` or `previous`.
Refused uploads write nothing to flash.

On the host a 450 KB bundle installs in about 25 ms (`install_ms`). On the
panel this is not measured yet; `install_ms` in the reply and the history line
will say. The HTTP loop is blocked for that long.

### Restart and rollback

After the 202, tt7d stops the camera worker if one runs (SIGTERM, so it
closes the camera; SIGKILL after 1 s) and reaps it, so the next tt7d finds
the camera free. Then it exits with status 75. tt7-app sees 75 and execs its entry
script again (`/data/tt7/app`, or `/usr/bin/tt7-app` if there is none), which
runs `select_release` and execs the chosen release's own `app`. No reboot: a
bundle changes nothing that init or the kernel use, and a reboot costs about
40 s and once hung for 5 minutes (gotchas.md). The price: background helpers
started at boot keep running the binaries they started with (ntpd keeps its
hook path, the `tt7probe log` logger its tt7probe) until the next reboot. A new
tt7d and `app` take effect at once; a new tt7probe or tt7-ntp-hook at the next
boot.

**Healthy** means tt7d's poll loop, the one that answers every request
including `/api/v1/info`, has kept running for `--update-confirm-after` (30 s)
since it bound its port. tt7d then deletes `update/trial`, if the trial names
the release it runs from (`TT7_RELEASE`, set by tt7-app), and logs
`confirmed`. A tt7d from another release never confirms it.

The chain, all in `probe/tt7-app.sh` (tested under BusyBox sh by
`probe/test_tt7_app.py`):

1. **New release**, under trial. Each start counts: every start of the entry
   script and every tt7d restart. tt7-app also stops a tt7d that has not
   confirmed within 90 s (`TT7_TRIAL_DEADLINE`), which counts as a failed
   start. After 2 starts (`TT7_TRIAL_MAX`) without a confirm, it rolls back.
2. **Previous release**, under a trial of its own (`update/previous` is
   cleared, so it cannot bounce back).
3. **The image's own build**: no release. PATH is `/data/tt7/bin`, then the
   image's directories.

**Why not init's own rollback.** init.c (`third_party/mmkeypad/init/init.c`)
decides once at boot whether to run `/data/tt7/app` or `/usr/bin/tt7-app`. If
that app exits within 20 s three times in a row, init renames `/data/tt7/app`
to `/data/tt7/app.bad` and runs the image's `/usr/bin/tt7-app` for the rest of
the boot. It never goes back to a previous app, only to the factory one, and
it only sees tt7-app exiting, which it never does on its own: tt7d crashes
happen inside tt7-app's loop. So the chain lives in tt7-app, and web updates
never rewrite `/data/tt7/app`. `TT7_TRIAL_MAX` is 2 so that a release whose
`app` dies before it even starts tt7d is rolled back on the third start,
before init's third strike quarantines `/data/tt7/app`. init's separate
`init.overlay` trial (3 unconfirmed boots, confirm after 30 s) is not used.

A release that was confirmed and later crashes is only restarted, never
rolled back. A release whose `app` hangs without exiting is caught at the next
boot, not before.

### Putting it on the panel that runs today

The panel runs main's tt7-app as `/data/tt7/app` and main's tt7d from
`/data/tt7/bin`, on a boot image built from an earlier commit. None of those
know about releases, and main's tt7d has no update endpoint, so the first step
is one ssh copy (no reflash):

```sh
make tt7d bundle
P=root@<panel-ip>; S="-o UserKnownHostsFile=build/known_hosts"
scp -O $S probe/tt7-app.sh $P:/data/tt7/app.new
scp -O $S build/tt7d $P:/data/tt7/bin/tt7d.new
ssh $S $P 'chmod 755 /data/tt7/app.new /data/tt7/bin/tt7d.new && mv /data/tt7/app.new /data/tt7/app && mv /data/tt7/bin/tt7d.new /data/tt7/bin/tt7d && sync'
ssh $S $P 'nohup reboot -f > /dev/null 2>&1 &'      # init reads /data/tt7/app at boot; needs Doctor Biz's go-ahead
```

After the reboot `app.log` says "running the image's own build (no release is
current)". Then upload `build/tt7-bundle-<build>.tar` in the Update section.
From then on every update goes through the web. Keep `/data/tt7/bin/tt7d`: it
is the end of the chain, and the image's own `/usr/bin/tt7d` on this panel
predates the update endpoint. If init ever quarantines `/data/tt7/app`, this
image's `/usr/bin/tt7-app` runs and ignores releases until `/data/tt7/app` is
put back. An image built from this commit has the same selection logic in
`/usr/bin/tt7-app`, so a reflash removes that gap but is not needed.

## On the panel

`probe/tt7-app.sh` (the image's `/usr/bin/tt7-app`) starts, all in the
background: ntpd (once a default route exists), Wi-Fi, discovery (once per
boot), and `tt7probe log` for raw input logging (evdev allows several readers,
so it runs beside tt7d's own input handling and keeps the raw records to
compare tt7d's events against). Then it runs `tt7d --data-dir /data/tt7/tt7d`
in a loop, so the fallback clock is up within seconds of boot instead of after
discovery (SPEC §38). If tt7d exits,
it restarts after 2 s without re-running the steps before it. PATH is the
current release's `bin` (see "Web update"), then `/data/tt7/bin`, then the
image's directories: a binary copied to `/data/tt7/bin` replaces the image's
copy, but not a web-installed release's.

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
Once a web-installed release is current, its `bin/tt7d` comes first on PATH
and this copy is not used: upload a bundle instead, or remove
`/data/tt7/update/current` and restart tt7-app to go back to it. Note that
`killall tt7d` while a release is under trial counts as a failed start.
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

- All of "Web update" on the panel: nothing of it has run there. On the host:
  the daemon took real bundles over HTTP (e2e), tt7-app's selection functions
  ran under BusyBox 1.37 sh (the Ubuntu host's, not the panel's 1.36.1), and
  one headless Chromium session through agent-browser uploaded a bundle in the
  Update section, saw the confirm, and a refused file (no console or CSP
  errors, fits 390 px). The whole tt7-app loop (background tt7d, the 90 s
  watchdog, `exec` of the entry script after exit 75) has not run anywhere:
  only its decision functions are tested. Install time and fsync cost on the
  panel's NAND are not measured. Whether `rename()` and `fsync()` on the
  panel's /data (ext4 on the rk30xxnand FTL) survive a power cut as ext4
  promises is not tested. TweetNaCl is checked with the RFC vectors on the
  host only; the ARM build compiles it but has not verified a signature.
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
- Blank/wake on the glass: measured on the wall panel (2026-09-28).
  Writing `brightness` 0 to `rk28_bl` does NOT blank: the screen goes super
  bright (the driver treats 0 specially). `bl_power` 4 turns the backlight
  fully off and `bl_power` 0 brings it back at the previous level, so tt7d
  blanks with `bl_power` and never writes brightness 0. The scale is not
  linear either: writing 13 reads back `actual_brightness` 19. Still
  unchecked: whether writing `brightness` while `bl_power` is 4 lights the
  screen (tt7d never does it; a level set while blank waits for wake), and
  what the fb blank ioctl (`/sys/class/graphics/fb0/blank`) does on this LCD
  controller.
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
- The camera in tt7d has not run on the panel. Only tt7cam's `probe` and `snap`
  captured there (30 back-to-back grabs in one session). Unverified:
  - The streaming session in presence mode: grabbing one frame every 500 ms
    with no buffer queued in between. tt7cam `motion` does this, but was not
    run on the panel. The driver may stall, keep a stale frame in its
    vipmem, or need the stream restarted each time. If it does, the worker
    timeout will show it (`gave no sign of life`), and the fix is STREAMOFF
    and STREAMON around each grab, at the cost of the auto-exposure settling.
  - The kernel side of SIGKILL: after a killed worker, whether the RK CIF
    driver's release path stops the stream and returns its buffer. A
    SIGTERM stop goes through `cam_close` and is fine.
  - JPEG encode time for 1280×720 on the Cortex-A9 (it runs in the worker,
    so it never holds up the display), and the JPEG size of a real room at
    quality 80 against the 512 KiB MQTT queue.
  - `presence_threshold` 8 and the 32×18 grid on real lighting (auto-exposure
    steps, a TV, sunlight moving).
  - The HA camera and occupancy entities in a real Home Assistant.
