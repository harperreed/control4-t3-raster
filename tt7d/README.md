# tt7d: the TT7 network display daemon

tt7d shows PNG frames that a server PUTs over HTTP on the C4-TT7's framebuffer
(SPEC.md milestones M1 and M2). It is one static C binary with no threads and
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
- **Logging**: startup lines and failed requests (status ≥ 400) only. The log
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

## API (`/api/v1`)

Reads need no auth, except `GET /config/mqtt`. `PUT /frame` and both
`/config/mqtt` methods need `Authorization: Bearer <token>`.
`GET /frame/image` is also unauthenticated in v1: it returns the frame that is
already visible on the glass. Revisit this when the panel shows anything private.

| Endpoint | Returns |
|---|---|
| `GET /info` | `device_id`, `model`, `firmware_version`, `build`; `display` {`width`, `height`, `rotation`, `frame_formats`, `max_frame_bytes`, `native` {`width`, `height`, `format`, `stride`, `bits_per_pixel`}}; `capabilities` read from sysfs at request time; `auth` |
| `GET /state` | `time` (UTC; the clock is wrong until something sets it), `uptime_s`, `daemon_uptime_s`, `display` {`on`, `brightness`, `frame_id`, `frame_age_s`}, `power`, `network.interfaces`, `frames` {`accepted`, `deduplicated`, `rejected`, `last_error`}; `mqtt` {`enabled`, `connected`, `broker` (host:port, never credentials), `client_id`, `topic_base`, `last_publish`, `last_error`, `reconnects`, `dropped`} |
| `GET /config/mqtt` | The MQTT settings in effect (see below). Needs the token |
| `PUT /config/mqtt` | Body: a JSON object of MQTT settings. Needs the token and `Content-Type: application/json`; at most 4096 bytes |
| `GET /frame` | Frame metadata: `frame_id`, `sha256`, `received_at`, `displayed_at`, `width`, `height`, `content_type`, `bytes`, `persisted`, `deduplicated`, `restored`. 404 `no_frame` before the first frame |
| `GET /frame/image` | The PNG exactly as received (or as restored). 404 `no_frame` before the first frame |
| `PUT /frame` | Body: a 1280×800 PNG. Headers: `Content-Type: image/png` (required), `X-Frame-ID` (1–128 printable ASCII, no spaces; generated as `tt7d-<24 hex>` if absent), `X-Frame-SHA256` (checked if present), `X-Persist: true\|false`. Replies 200 with the frame metadata |

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
| 400 | `bad_request`, `invalid_frame_id`, `invalid_sha256`, `invalid_persist` | |
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
| 431 | `headers_too_large` | |
| 500 | `persist_failed` (nothing changed on screen), `write_failed` (`PUT /config/mqtt`), `internal_error` | |
| 505 | `http_version_not_supported` | |

Every refused `PUT /frame` counts in `/state` `frames.rejected`, and its code
goes to `frames.last_error` (SPEC §42).

## Building and testing

```sh
make tt7d            # build/tt7d: static ARM EABI5 for the panel
make test-host       # unit tests (render, json, http, util, sysinfo, mqtt) with ASan/UBSan
make test-e2e        # the real daemon, host-built, on a file-backed fb (tt7d/test_e2e.py)
make test-mqtt       # the real daemon against real amqtt brokers and a paho client (tt7d/test_mqtt_e2e.py)
make check           # everything, including the boot image checks
```

Run it on the host by hand with a fake framebuffer and the panel's sysfs fixture:

```sh
make build/host/tt7d
build/host/tt7d --listen 127.0.0.1:8765 --fb-file /tmp/fb.raw --fb-geometry 800x1280x16 \
  --fb-stride 1600 --fb-format rgb565 --data-dir /tmp/tt7d-data \
  --sysfs-root tt7d/test/fixtures/sysfs-tt7
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
| `state` | yes | JSON, every `telemetry_interval`, and within 1 s of a change to the frame id, brightness, battery, power or IPs. Example below |
| `sensor/<name>` | yes | plain values: `uptime_s`, `battery_percent`, `charging` (`true`/`false`), `brightness` (percent), `frame_age_s`, `wifi_ip`, `ethernet_ip`; each only when known |
| `event/boot` | no | `{"type":"boot","firmware_version":"0.1.0 (…)","uptime_s":41,"timestamp":"…"}`, once per daemon start, on the first connect |
| `event/frame` | no | `{"type":"frame","frame_id":"…","sha256":"…","deduplicated":false,"timestamp":"…"}` for each accepted `PUT /frame` |
| `event/error` | no | `{"type":"error","error":"unsupported_command","command":"wake","message":"…","timestamp":"…"}` |
| `event/button` | no | M3 publishes physical button events with `mqtt_app_event(m, "button", json)`. Touch stays off MQTT (the owner chose a WebSocket for it) |
| `cmd/brightness` | (in) | `NN%` (0-100), or a raw level `NN` (0 to `max_brightness`, which is 255 here) |
| `cmd/wake`, `cmd/blank` | (in) | anything |
| `cmd/reboot` | (in) | anything; refused with `command_disabled` unless `allow_reboot_cmd=true` |

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

**Commands and M4.** On this branch tt7d has no brightness, wake, blank or
reboot code; M4 (the control panel) adds it. MQTT reaches those operations
only through `struct mqtt_actions` in `mqtt.h`, and `main.c` passes `NULL` for
now. So every command answers `event/error` `unsupported_command`, and the
matching HA controls are not advertised. When M4 merges, it fills in the
struct in `main.c`, and `mqtt*.c` stays as it is.

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
| `sensor` brightness (%) | a backlight, and no brightness operation (this branch) |
| `number` brightness (0-100 %, slider, sends `NN%` to `cmd/brightness`) | a backlight, and `set_brightness` wired (after M4) |
| `button` wake, blank | the operation is wired (after M4) |
| `button` reboot (device class restart) | wired (after M4), and `allow_reboot_cmd=true` |

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

### Connecting the panel to the broker

After installing a new `build/tt7d` as above, with the token in
`~/.config/tt7/token`:

```sh
tools/mqtt-setup.sh <panel-ip> --broker 192.168.23.123:1883 --user <user> \
    --password-file ~/.config/tt7/mqtt-password     # omit --user/--password-file for an anonymous broker
curl -s http://<panel-ip>/api/v1/state | python3 -m json.tool | grep -A10 '"mqtt"'
```

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
- The wall clock: the panel has an RTC (`hym8563`), but nothing sets the time,
  so `received_at` and `time` may read 1970 or be stale. `frame_age_s` uses
  the monotonic clock and is correct regardless.
- MQTT has only met amqtt 0.12.1 (tests) so far, not the owner's broker at
  192.168.23.123, not mosquitto, and not the panel's network stack.
- The Home Assistant discovery payloads follow the documentation cited above
  but have not been loaded into a real Home Assistant.
