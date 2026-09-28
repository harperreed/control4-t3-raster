# TT7 Network Display Firmware

**Status:** Draft v0.1  
**Target:** Control4 T3 7" Tabletop Touchscreen, C4-TT7 family  
**Working name:** `tt7d`

---

# 1. Objective

Repurpose the Control4 C4-TT7 as a lightweight **network-addressable display, input, and telemetry appliance**.

The TT7 does **not** render application interfaces using HTML, JavaScript, Chromium, WebKit, or a local application framework.

Instead:

```text
SERVER                           TT7
──────                           ───

application state
      ↓
render UI
      ↓
1280×800 image
      │
      ├──────── HTTP ───────────► frame endpoint
      │                           ↓
      │                         display
      │
      │                           ↑
      ◄──── HTTP/MQTT/WS ───── touch/buttons
      │
      ◄──────── MQTT ────────── sensors/state
```

The server owns:

- application state
- rendering
- layout
- typography
- interaction logic
- animations
- business logic

The TT7 owns:

- physical display
- physical input
- networking
- hardware state
- telemetry
- device management
- MQTT
- diagnostics
- firmware lifecycle

The conceptual abstraction is:

> **TT7 = remote pixels + remote input + hardware telemetry**

---

# 2. Design principles

## 2.1 Dumb display

Application-specific logic MUST NOT exist in firmware.

The firmware should not know what a:

- button
- thermostat
- music player
- calendar
- dashboard
- Home Assistant entity

is.

It knows only:

- pixels
- coordinates
- buttons
- devices
- measurements
- commands

---

## 2.2 Server-side rendering

The server MAY produce frames using anything:

- HTML/CSS
- React
- Playwright
- SVG
- Canvas
- Pillow
- ImageMagick
- native rendering
- an agent-generated image

The device receives the resulting raster image.

---

## 2.3 Self-describing

Every TT7 MUST expose enough machine-readable information for software or an agent to understand the device without prior knowledge.

Example:

```json
{
  "device_id": "tt7-7f38a2",
  "model": "C4-TT7",
  "display": {
    "width": 1280,
    "height": 800,
    "native_format": "rgb565",
    "rotation": 0
  },
  "capabilities": {
    "touch": true,
    "battery": true,
    "backlight": true,
    "wifi": true,
    "ethernet": true,
    "audio_input": true,
    "audio_output": true,
    "camera": true
  }
}
```

Capabilities MUST be discovered from actual hardware.

Unsupported or unavailable hardware MUST be explicitly represented as unavailable rather than silently omitted where practical.

---

## 2.4 Observable

Anything important happening inside the device should be inspectable through at least one of:

- local control panel
- HTTP API
- MQTT
- logs
- Prometheus metrics
- diagnostic bundle

There should be very little hidden state.

---

## 2.5 Recoverable

Experimental firmware MUST prioritize recoverability over sophistication.

Until a reliable restore procedure exists:

- preserve the original bootloader
- preserve the original kernel
- preserve device-specific/calibration partitions
- avoid unnecessary flash writes
- maintain complete stock dumps

---

# 3. Verified hardware baseline

The C4-TT7-1 family provides a **1280×800 capacitive 7" display and 720p camera**. Official Control4 documentation also identifies stereo speakers, microphone, physical volume controls, power/reset controls, Ethernet through the charging station, 2.4 GHz 802.11g/n Wi-Fi, battery operation, and PoE/DC power options.

The 7" model's published battery capacity is **3100 mAh Li-ion**.

All Linux device paths, drivers, pixel formats and kernel interfaces remain **TBD until measured on the actual target device**.

The firmware MUST discover these rather than hard-code assumptions.

---

# 4. System architecture

```text
┌──────────────────────────────────────────────┐
│                    TT7                       │
│                                              │
│  ┌────────────────────────────────────────┐  │
│  │              Vendor kernel             │  │
│  │                                        │  │
│  │ framebuffer/display     touchscreen    │  │
│  │ backlight               buttons        │  │
│  │ Ethernet/Wi-Fi          power/battery  │  │
│  │ audio                   camera         │  │
│  └──────────────────┬─────────────────────┘  │
│                     │                        │
│  ┌──────────────────▼─────────────────────┐  │
│  │                 tt7d                   │  │
│  │                                        │  │
│  │ display         input      telemetry   │  │
│  │ device state    MQTT       HTTP API    │  │
│  │ control UI      metrics    watchdog    │  │
│  └──────┬─────────────┬────────────┬──────┘  │
│         │             │            │         │
└─────────┼─────────────┼────────────┼─────────┘
          │             │            │
         HTTP          MQTT       WebSocket
          │             │            │
          └─────────────┼────────────┘
                        │
                  external systems
```

---

# 5. Firmware architecture

Initial firmware SHOULD use:

```text
existing bootloader
        ↓
existing/vendor Linux kernel
        ↓
minimal Linux root filesystem
        ↓
tt7d
```

The initial project SHOULD NOT attempt a mainline Linux port.

The existing kernel should remain until the following have been characterized:

- LCD
- framebuffer/display controller
- touchscreen
- buttons
- backlight
- dock
- Ethernet
- Wi-Fi
- battery
- charger
- audio
- camera
- suspend/resume

A minimal Buildroot-style root filesystem is preferred if compatible with the target kernel/toolchain.

The exact implementation language for `tt7d` is deferred until the existing kernel ABI and compiler constraints are known.

Desired properties:

- single executable where practical
- few runtime dependencies
- deterministic startup
- low memory use
- cross-compilable
- ARM-compatible
- no dynamic desktop stack

---

# 6. Filesystem

Preferred layout:

```text
/
├── bin/
├── dev/
├── etc/
│   └── tt7/
│       └── config.toml
├── proc/
├── sys/
├── run/
├── tmp/
└── data/
    ├── device.json
    ├── last-frame.png
    ├── state/
    └── logs/
```

Root filesystem SHOULD eventually be read-only.

Mutable state lives under:

```text
/data
```

Temporary files live in RAM.

---

# 7. Device identity

Every device receives a persistent identifier on first boot.

Example:

```text
tt7-7f38a2
```

The identifier MUST survive:

- reboot
- configuration changes
- network changes
- firmware updates

User-facing hostname is separately configurable:

```text
conference-room-east
```

Do not use IP address as identity.

---

# 8. Network discovery

The device SHOULD expose:

```text
http://tt7-7f38a2.local/
```

using mDNS where supported.

Advertise a service similar to:

```text
_tt7._tcp
```

Metadata MAY contain:

```text
id=tt7-7f38a2
model=C4-TT7
version=0.1.0
width=1280
height=800
```

The device MUST work without mDNS when addressed directly by IP.

---

# 9. Frame protocol

## 9.1 Initial transport

HTTP is the canonical v1 frame transport.

```text
PUT /api/v1/frame
```

Mandatory v1 format:

```text
Content-Type: image/png
```

The server SHOULD send exactly the display's logical resolution.

For the C4-TT7 baseline:

```text
1280 × 800
```

---

## 9.2 Request

Example:

```http
PUT /api/v1/frame HTTP/1.1
Content-Type: image/png
X-Frame-ID: 01K8R8CY1EP54...
X-Frame-SHA256: ...
X-Persist: false

<PNG DATA>
```

`X-Frame-ID` SHOULD be globally unique.

The server MAY omit the hash.

---

## 9.3 Atomic presentation

Frame changes MUST be atomic from the user's perspective.

The implementation should behave approximately as:

```text
receive
  ↓
validate
  ↓
decode into back buffer
  ↓
convert into native format
  ↓
swap/copy to display
  ↓
acknowledge
```

A partially received frame MUST never appear on screen.

---

# 10. Frame formats

## Required

```text
image/png
```

## Optional later

```text
image/jpeg
application/x-rgb565
application/x-rgba8888
```

Supported formats MUST be advertised by `/api/v1/info`.

Example:

```json
{
  "frame_formats": [
    "image/png"
  ]
}
```

Raw framebuffer formats are optimization paths, not v1 requirements.

---

# 11. Frame deduplication

`tt7d` SHOULD calculate or accept a frame hash.

If an incoming frame is identical to the currently displayed frame:

- do not redraw
- update relevant receipt metadata
- respond successfully

This reduces unnecessary framebuffer work.

---

# 12. Persistent frames

The server MAY request persistence:

```text
X-Persist: true
```

When requested, the frame is saved as:

```text
/data/last-frame.png
```

On boot the device MAY immediately display this image.

Normal high-frequency frames MUST NOT automatically cause flash writes.

---

# 13. Current-screen API

Metadata:

```text
GET /api/v1/frame
```

Example:

```json
{
  "frame_id": "01K8R8CY1EP54",
  "sha256": "...",
  "received_at": "2026-09-27T20:03:31Z",
  "width": 1280,
  "height": 800,
  "content_type": "image/png",
  "persisted": false
}
```

Image:

```text
GET /api/v1/frame/image
```

The control panel uses this endpoint to show what the user should currently be seeing.

For PNG input, retaining the original accepted PNG avoids an unnecessary framebuffer screenshot/encode cycle.

---

# 14. Input model

The firmware should expose **raw interaction**, not application concepts.

## Touch

Events:

```text
down
move
up
```

Example:

```json
{
  "type": "touch",
  "action": "down",
  "pointer": 0,
  "x": 614,
  "y": 283,
  "nx": 0.480,
  "ny": 0.354,
  "frame_id": "01K8R8CY1EP54",
  "timestamp": "2026-09-27T20:04:18.183Z",
  "monotonic_ms": 9287133
}
```

Both absolute and normalized coordinates SHOULD be provided.

`frame_id` is important because the server needs to know **which visual state the user interacted with**.

The firmware SHOULD NOT initially implement:

- buttons
- gestures
- swipe
- long press
- double-click

Those semantics belong on the server.

---

# 15. Physical buttons

Physical controls SHOULD produce events.

Example:

```json
{
  "type": "button",
  "button": "volume_up",
  "action": "press",
  "timestamp": "..."
}
```

Possible discovered buttons include:

```text
power
volume_up
volume_down
```

Only actually detected buttons should be advertised.

---

# 16. Event transports

Input events should be available through two interfaces.

## MQTT

```text
tt7/<device-id>/event/touch
tt7/<device-id>/event/button
```

## WebSocket

```text
WS /api/v1/events
```

WebSocket is intended for low-latency interactive applications.

MQTT is intended for automation, monitoring, and system integration.

Both MAY be enabled simultaneously.

---

# 17. HTTP API

Base:

```text
/api/v1
```

## Information

```text
GET /info
GET /state
GET /capabilities
GET /hardware
GET /sensors
```

## Display

```text
GET  /frame
GET  /frame/image
PUT  /frame

PUT  /display/brightness
POST /display/wake
POST /display/blank
POST /display/test-pattern
```

## System

```text
GET  /system
POST /system/reboot
POST /system/shutdown
POST /system/restart-daemon
```

## Time

```text
GET /time
PUT /time
PUT /timezone
```

## Diagnostics

```text
GET /logs
GET /diagnostics
GET /metrics
```

Configuration-changing operations require authentication.

---

# 18. `/info`

`GET /api/v1/info`

is intended to answer:

> What exactly are you and what can you do?

Example:

```json
{
  "device_id": "tt7-7f38a2",
  "name": "Conference Room East",
  "model": "C4-TT7",
  "firmware_version": "0.1.0",

  "display": {
    "width": 1280,
    "height": 800,
    "rotation": 0,
    "frame_formats": [
      "image/png"
    ]
  },

  "capabilities": {
    "touch": true,
    "buttons": [
      "power",
      "volume_up",
      "volume_down"
    ],
    "brightness": true,
    "battery": true,
    "dock_detection": true,
    "ethernet": true,
    "wifi": true,
    "camera": true,
    "audio_input": true,
    "audio_output": true
  }
}
```

---

# 19. `/state`

`GET /api/v1/state`

answers:

> What is happening right now?

Example:

```json
{
  "time": "2026-09-27T15:15:21-05:00",
  "uptime_s": 37102,

  "display": {
    "on": true,
    "brightness": 72,
    "frame_id": "01K8R8..."
  },

  "power": {
    "source": "dock",
    "battery_percent": 91,
    "charging": true
  },

  "network": {
    "ethernet": {
      "connected": true,
      "ip": "10.0.1.184"
    },

    "wifi": {
      "connected": false
    }
  },

  "mqtt": {
    "connected": true
  }
}
```

Unavailable values should be represented explicitly:

```json
{
  "battery_temperature": null
}
```

rather than fabricated.

---

# 20. Hardware inventory

`GET /api/v1/hardware`

is deliberately low-level.

It SHOULD expose discovered mappings such as:

```json
{
  "display": {
    "device": "/dev/fb0",
    "driver": "...",
    "pixel_format": "..."
  },

  "input": [
    {
      "device": "/dev/input/event0",
      "role": "touchscreen",
      "driver": "..."
    }
  ],

  "power_supplies": [],
  "thermal_zones": [],
  "network_interfaces": [],
  "audio_devices": [],
  "video_devices": []
}
```

This endpoint is especially important during bring-up and reverse engineering.

---

# 21. Sensor abstraction

`tt7d` maintains a registry of discovered sensors.

Conceptually:

```text
sensor
  id
  class
  value
  unit
  source
  updated_at
  available
```

Example:

```json
{
  "id": "battery.percent",
  "class": "battery",
  "value": 91,
  "unit": "%",
  "source": "/sys/class/power_supply/...",
  "available": true
}
```

Potential sensors include, **only if physically exposed by the hardware/kernel**:

- battery percentage
- battery voltage
- battery current
- charge state
- dock state
- power source
- CPU temperature
- thermal zones
- CPU utilization
- RAM usage
- storage usage
- Ethernet link
- network RX/TX
- Wi-Fi RSSI
- Wi-Fi SSID
- screen state
- brightness
- last-touch timestamp
- frame age
- camera availability
- microphone availability
- speaker availability

Additional discovered hardware should fit this same registry rather than require architectural changes.

---

# 22. MQTT

MQTT is a first-class interface.

Default namespace:

```text
tt7/<device-id>
```

Example:

```text
tt7/tt7-7f38a2
```

---

# 23. MQTT availability

Topic:

```text
tt7/<id>/availability
```

Payload:

```text
online
```

MQTT Last Will:

```text
offline
```

Availability messages MUST be retained.

---

# 24. MQTT state

Primary state topic:

```text
tt7/<id>/state
```

Retained JSON:

```json
{
  "uptime": 38291,
  "battery_percent": 91,
  "charging": true,
  "docked": true,
  "brightness": 72,
  "wifi_rssi": -61,
  "ethernet": true,
  "frame_id": "01K8R8...",
  "last_touch": "2026-09-27T20:14:18Z"
}
```

This aggregate state reduces MQTT topic explosion and is easy for agents and applications to consume.

---

# 25. MQTT sensor topics

Individual sensor topics MAY additionally be published:

```text
tt7/<id>/sensor/battery_percent
tt7/<id>/sensor/cpu_temperature
tt7/<id>/sensor/wifi_rssi
```

These are primarily useful for simple automation systems.

---

# 26. MQTT events

Events MUST NOT be retained.

Examples:

```text
tt7/<id>/event/touch
tt7/<id>/event/button
tt7/<id>/event/boot
tt7/<id>/event/error
```

---

# 27. MQTT commands

Examples:

```text
tt7/<id>/cmd/brightness
tt7/<id>/cmd/wake
tt7/<id>/cmd/blank
tt7/<id>/cmd/reboot
```

Commands MUST correspond to authenticated operations available through the HTTP API.

Dangerous commands should be capability/configuration gated.

---

# 28. Home Assistant

The daemon SHOULD implement Home Assistant MQTT Discovery.

The TT7 appears as one Home Assistant device.

Potential entities:

### Sensors

- battery
- CPU temperature
- Wi-Fi signal
- uptime
- current frame age
- memory use

### Binary sensors

- docked
- charging
- Ethernet connected
- display active

### Controls

- brightness
- volume if supported

### Buttons

- wake
- reboot

Entities MUST only be created when the associated capability is available.

---

# 29. Local control panel

The device serves a lightweight management interface:

```text
http://tt7-7f38a2.local/
```

The control panel is administrative.

It is **not** the UI displayed by the TT7 itself.

No large JavaScript framework should be required.

Simple HTML/CSS plus minimal JavaScript is sufficient.

---

# 30. Control panel — Overview

Example:

```text
TT7 — Conference Room East                 ● ONLINE

┌──────────────────────────────────────────┐
│                                          │
│             CURRENT SCREEN               │
│                                          │
│          [1280×800 preview]              │
│                                          │
└──────────────────────────────────────────┘

Frame       01K8R8CY...
Age         3 seconds
Brightness  72%

Power       Docked
Battery     91% charging
Uptime      4d 03h

Ethernet    10.0.1.184
Wi-Fi       disconnected
MQTT        connected

[Wake] [Blank] [Test Pattern] [Reboot]
```

The preview should represent the last successfully accepted frame.

---

# 31. Control panel sections

## Overview

- current screen
- online state
- current frame
- major telemetry
- common actions

## Display

- screen preview
- frame ID
- frame age
- resolution
- pixel format
- rotation
- brightness
- wake
- blank
- test patterns

## Hardware

- framebuffer
- input devices
- audio devices
- camera
- power devices
- thermal zones
- physical buttons
- detected drivers

## Network

- hostname
- Ethernet state
- Wi-Fi state
- IP addresses
- MAC addresses
- DNS
- default gateway
- Wi-Fi RSSI
- RX/TX counters

## MQTT

- configured broker
- connection status
- client ID
- namespace
- last publish
- last receive
- recent MQTT activity

## System

- firmware version
- kernel version
- uptime
- CPU use
- RAM
- storage
- current time
- timezone
- NTP state

## Logs / Debug

- recent `tt7d` log
- kernel log tail
- boot information
- discovered hardware
- error history
- diagnostic bundle download

---

# 32. Time

Normal operation:

```text
NTP → system clock
```

The device MUST expose:

- current time
- timezone
- synchronization status

The control panel should support:

- set timezone
- enable/disable NTP
- manually set time

Manual time setting is primarily diagnostic.

---

# 33. Diagnostics bundle

The user should be able to download:

```text
tt7-diagnostics-<device>-<timestamp>.tar.gz
```

Containing, where available:

```text
info.json
state.json
hardware.json
sensors.json
config-redacted.toml
tt7d.log
dmesg.txt
proc-cmdline.txt
mounts.txt
network.txt
input-devices.txt
audio-devices.txt
video-devices.txt
```

Secrets MUST be removed.

---

# 34. Prometheus metrics

Expose:

```text
GET /metrics
```

Example:

```text
tt7_uptime_seconds 357231
tt7_battery_percent 91
tt7_wifi_rssi_dbm -61
tt7_frames_received_total 183991
tt7_frames_rejected_total 2
tt7_frame_decode_seconds 0.012
tt7_touch_events_total 4821
```

Metrics are an observability feature, not the canonical control API.

---

# 35. Configuration

Example:

```toml
device_id = "tt7-7f38a2"
name = "Conference Room East"
hostname = "conference-room-east"

[display]
rotation = 0
brightness = 80

[http]
listen = "0.0.0.0:80"

[mqtt]
enabled = true
broker = "mqtt://10.0.1.20:1883"
prefix = "tt7"
username = "tt7"
password_file = "/data/mqtt-password"

[time]
timezone = "America/Chicago"
ntp = true

[telemetry]
interval_seconds = 10

[debug]
ssh = false
```

Configuration should have a monotonically increasing:

```text
config_revision
```

so tools can detect changes.

---

# 36. Security

The system is expected to live on a trusted LAN, but must not assume every LAN client is trusted.

At minimum:

- management mutations require authentication
- frame writes require authentication
- diagnostic downloads require authentication
- MQTT credentials are supported
- credentials never appear in diagnostics
- MQTT TLS SHOULD be supported if practical
- control panel sessions SHOULD be authenticated
- production SSH SHOULD default to disabled

Read-only status endpoints MAY optionally be exposed without authentication.

---

# 37. Watchdog

`tt7d` is appliance infrastructure and should recover automatically.

The system should monitor:

- daemon health
- frame decoder
- MQTT connection
- networking
- display path

Crashes should restart `tt7d`.

Repeated failures should be visible through logs and boot/error counters rather than silently cycling forever.

---

# 38. Boot behavior

Target sequence:

```text
kernel boot
    ↓
filesystem mounted
    ↓
network initialized
    ↓
tt7d starts
    ↓
restore persisted frame if available
    ↓
HTTP starts
    ↓
MQTT connects
    ↓
availability = online
    ↓
server supplies fresh frame
```

The display SHOULD become useful without waiting for nonessential services.

---

# 39. Server behavior

The renderer is a separate system.

A minimal server loop looks like:

```text
state changes
    ↓
render 1280×800
    ↓
PNG
    ↓
PUT /api/v1/frame
```

Possible renderer:

```text
React
  ↓
headless Chromium / Playwright
  ↓
screenshot
  ↓
PNG
```

but no renderer technology is prescribed.

---

# 40. Interaction cycle

Interactive operation:

```text
SERVER                              TT7

render state A
      │
      │──── frame A ───────────────► display
      │
      │
      │◄──── touch(x,y,frame=A) ──── user touches
      │
update application state
      │
render state B
      │
      │──── frame B ───────────────► display
```

The TT7 remains unaware of what the interaction means.

---

# 41. Offline behavior

If the renderer disappears:

- retain the last valid frame until the fallback timeout expires
- then show the fallback clock screen (§41.1)
- continue serving control panel/API
- continue publishing local telemetry when MQTT remains available
- do not blank the display unless configured

The device SHOULD expose:

```text
last_frame_age
```

so stale content can be detected.

## 41.1 Fallback clock screen

(Added 2026-09-27, Doctor Biz: "if it can't find the server it is expecting it should show a nice date, clock on the screen. that is the failure mode.")

The device has no server address; it only receives frames. "Server missing" therefore means:

- no frame has been accepted since boot, or
- the newest accepted frame **or heartbeat** is older than `fallback_timeout` (default 300 s, configurable, 0 disables)

When the condition holds, `tt7d` renders a fallback screen itself: a large clock, the date, and a small status line (for example `waiting for server · <ip>`). The first accepted frame replaces it immediately.

Servers that update rarely keep the display with `POST /api/v1/heartbeat`. Re-sending the current frame also counts, and deduplication keeps that cheap.

The clock MUST NOT show a time until the system clock is synchronized (NTP). Until then the fallback shows the status line and a "setting clock" notice. Timezone is configurable.

The fallback screen is the one deliberate exception to §2.1: a failure-mode display, not application UI.

---

# 42. Error behavior

An invalid frame:

- MUST NOT replace the current valid frame
- MUST return an actionable HTTP error
- MUST increment failure telemetry
- SHOULD log a reason

Example:

```json
{
  "error": "invalid_dimensions",
  "expected": [1280, 800],
  "received": [1024, 768]
}
```

Errors should use stable machine-readable codes.

---

# 43. API conventions

The API should optimize for humans, automation, and agents.

Requirements:

- stable JSON field names
- ISO-8601 timestamps
- explicit units
- explicit `null` for unavailable values
- stable error codes
- idempotent setters where reasonable
- versioned endpoints
- capability discovery
- no undocumented magic values

Example:

Bad:

```json
{"temp": 48}
```

Good:

```json
{
  "cpu_temperature": {
    "value": 48.0,
    "unit": "celsius",
    "available": true
  }
}
```

---

# 44. Hardware discovery phase

Before implementing the appliance image, characterize the target unit.

Collect:

```sh
uname -a
cat /proc/cpuinfo
cat /proc/cmdline
cat /proc/partitions
cat /proc/mtd
mount
df -h

find /sys/class -maxdepth 2
ls -la /dev/fb*
ls -la /dev/input
ls -la /dev/video*
ls -la /dev/snd

cat /proc/bus/input/devices

ip addr
ip link

dmesg
```

Where available:

```sh
fbset
evtest
aplay -l
arecord -l
v4l2-ctl --all
```

Create:

```text
docs/hardware-inventory.md
```

from the results.

---

# 45. Recovery milestone

Before persistent firmware replacement:

1. Obtain complete stock storage dump.
2. Hash it.
3. Repeat dump and compare hashes where practical.
4. Document partition map.
5. Identify bootloader.
6. Identify kernel image.
7. Identify recovery path.
8. Prove a failed userspace image can be recovered.
9. Preserve all device-specific partitions.

No production firmware milestone is complete until recovery has been demonstrated.

---

# 46. Development modes

## Development image

May include:

- SSH
- shell tools
- `strace`
- `evtest`
- framebuffer utilities
- ALSA tools
- V4L tools
- verbose logging

## Production image

Remove unnecessary tooling.

Production defaults:

```text
SSH:              off
debug API:        authenticated
root filesystem:  read-only
watchdog:          on
MQTT:              on if configured
control UI:        on
```

---

# 47. Update mechanism

Firmware updates are required eventually but are not necessary for framebuffer MVP.

Desired properties:

- authenticated update
- integrity verification
- version reporting
- failed-update recovery
- no modification of factory backup/recovery material

An A/B root filesystem is desirable if storage layout permits it.

Do not design an A/B layout until actual flash capacity and partition structure are known.

---

# 48. Repository

Suggested repository:

```text
tt7/
├── README.md
├── SPEC.md
│
├── docs/
│   ├── hardware-inventory.md
│   ├── boot.md
│   ├── recovery.md
│   ├── partitions.md
│   ├── framebuffer.md
│   ├── input.md
│   └── mqtt.md
│
├── firmware/
│   ├── buildroot/
│   ├── overlay/
│   └── configs/
│
├── tt7d/
│   ├── api/
│   ├── display/
│   ├── input/
│   ├── hardware/
│   ├── mqtt/
│   ├── telemetry/
│   └── web/
│
├── tools/
│   ├── discover-hardware.sh
│   ├── dump-device.sh
│   ├── framebuffer-test.sh
│   └── push-frame.sh
│
├── protocol/
│   ├── openapi.yaml
│   └── mqtt.md
│
└── test/
    ├── fixtures/
    └── integration/
```

---

# 49. Implementation milestones

## M0 — Recovery

**Goal:** We cannot permanently brick the test unit through normal development mistakes.

Deliver:

- stock dump
- hashes
- partition documentation
- documented recovery process
- recovery proven experimentally

---

## M1 — Pixels

**Goal:** Display arbitrary server-generated pixels.

Deliver:

```text
test.png
   ↓
push-frame
   ↓
TT7 framebuffer
```

Acceptance:

- arbitrary 1280×800 PNG appears correctly
- orientation correct
- colors correct
- repeatable after reboot
- invalid image cannot corrupt current display

---

## M2 — Network display

Deliver:

```text
PUT /api/v1/frame
GET /api/v1/frame
GET /api/v1/frame/image
GET /api/v1/info
GET /api/v1/state
```

Acceptance:

- device receives frames over Ethernet
- frame IDs work
- deduplication works
- current-screen preview works

Wi-Fi follows after Ethernet if Ethernet is easier during development.

---

## M3 — Input

Deliver:

- touchscreen discovery
- coordinate calibration
- physical button discovery
- WebSocket event stream

Acceptance:

```text
touch physical screen
        ↓
server receives correct x/y + frame_id
```

---

## M4 — Control panel

Deliver:

- overview
- screen preview
- system information
- hardware information
- networking
- time configuration
- logs
- reboot/shutdown
- diagnostics bundle

---

## M5 — MQTT

Deliver:

- availability/LWT
- aggregate state
- sensor publication
- touch/button events
- commands
- MQTT diagnostics

Acceptance:

- reconnects automatically
- state is retained correctly
- events are not retained
- broker loss does not affect display operation

---

## M6 — Home Assistant

Deliver:

- MQTT Discovery
- device entity
- available telemetry entities
- display controls
- lifecycle cleanup

---

## M7 — Appliance firmware

Deliver:

- minimal rootfs
- deterministic boot
- read-only root
- persistent `/data`
- watchdog
- secure defaults
- production logging

---

## M8 — Update/recovery

Deliver:

- authenticated firmware update
- integrity verification
- failed-update recovery
- version history

---

# 50. MVP definition

The MVP is complete when all of the following work:

1. TT7 boots custom userspace.
2. Ethernet obtains an IP address.
3. Device is discoverable.
4. Server can push a PNG.
5. PNG appears correctly on screen.
6. Control panel displays the current PNG.
7. Touch coordinates are delivered to server.
8. Physical buttons produce events where supported.
9. `/info` describes real hardware capabilities.
10. `/state` describes current runtime state.
11. Device publishes MQTT availability.
12. Device publishes discovered telemetry.
13. Home Assistant discovers the TT7.
14. Time/timezone can be inspected and changed.
15. Device can reboot remotely.
16. Device survives loss of server.
17. Device survives loss of MQTT.
18. Device survives loss/recovery of network.
19. Last good frame remains displayed through transient failures.
20. Stock device firmware can still be restored.

---

# 51. Explicit non-goals for v1

Do **not** build:

- Chromium
- WebKit
- local HTML rendering
- React on device
- application widgets
- local dashboard framework
- server application logic
- gesture recognizer
- remote desktop protocol
- video playback
- camera streaming
- microphone streaming
- audio conferencing
- delta-frame protocol
- animation protocol
- mainline Linux port

These can be considered after the core appliance is stable.

---

# 52. Future optimizations

Only optimize after measuring.

Potential later additions:

### Raw frames

```text
RGB565
```

avoids PNG decode.

### Dirty rectangles

```text
x
y
width
height
pixels
```

allow partial updates.

### Compression

Raw buffers could use:

```text
zstd
lz4
```

### Frame queues

Useful for limited animation.

### Server synchronization

Server could coordinate many TT7s simultaneously.

### Media transport

Camera/audio can become separate protocols without affecting the framebuffer architecture.

---

# 53. Core invariant

The most important architectural invariant is:

> **The device exposes hardware. The server provides meaning.**

A new server application should require **zero firmware modification**.

A new firmware release should not need to know anything about the server's UI.

If the TT7 can:

```text
receive pixels
display pixels
report input
report hardware
accept management commands
stay alive
```

then it is doing its job.

---

# 54. One-sentence product definition

**TT7 firmware turns an obsolete Control4 touchscreen into a remotely rendered, bidirectional, observable network display appliance.**
