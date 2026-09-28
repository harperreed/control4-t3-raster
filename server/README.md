# tt7-server: the backend that drives N tt7 panels

tt7-server makes each tt7 panel show a web page. It runs one headless Chrome
with one tab per panel, pushes what the tab paints to the panel as PNG frames
(tt7d `PUT /api/v1/frame`), and turns touches on the glass into real mouse
clicks in that tab. Any URL works: the server is a remote browser (the plan's
approved decision, docs/superpowers/plans/2026-09-28-tt7-server.md).

Milestone S1: built and tested on the host against host-built tt7d daemons.
It has not yet talked to a real panel (that is S2).

## Running it

```sh
mise install                               # go 1.26.6 (mise.toml)
make server                                # build/server/tt7-server
cp server/screens.example.toml screens.toml   # then edit url = ... per screen
build/server/tt7-server -config screens.toml  # -debug also logs every pushed frame
# admin page: http://127.0.0.1:7788/
```

`server/screens.example.toml` documents every key and is already set up for
the two panels: the tabletop tt7-4009b5 at 192.168.23.197 (token
`~/.config/tt7/token`) and the wall tt7-942093 at 192.168.23.198 (token
`~/.config/tt7/token-tt7-942093`). tt7-server refuses a token file that group
or others can read (`chmod 600`), as `scripts/wifi-setup.sh` does.

Admin API (JSON; errors are `{"error": "<code>", "message": ...}`):

| Endpoint | Does |
|---|---|
| `GET /` | The admin page: status, URL, reload, and preview for each screen |
| `GET /api/screens` | `{"screens": [...]}`: `name`, `host`, `url`, `enabled`, `reachable`, `events_connected`, `device_id`, `last_push_at`, `last_frame_id`, `frames_pushed`, `last_error`, `last_error_at` (null when unknown) |
| `PUT /api/screens/{name}/url` | Body `{"url": "https://..."}`. Saves it to screens.toml, then navigates. 400 `invalid_url`, 404 `no_such_screen`, 500 `save_failed` |
| `POST /api/screens/{name}/reload` | Reloads the page (409 `reload_failed` on a disabled screen) |
| `GET /api/screens/{name}/preview.png` | The last frame the panel accepted (404 `no_frame`) |

The API needs `Authorization: Bearer <admin token>` when `admin_token_file` is
set, and the config requires one whenever `listen` is not a loopback address.

## How it works

```text
screens.toml ──► tt7-server
                  ├ Chrome (headless, one process) ── one tab per enabled screen, 1280×800 at scale 1
                  │    Page.startScreencast (PNG) ──► screen.OnFrame (keeps only the newest)
                  ├ per screen, three goroutines:
                  │    push loop:  newest capture → skip if same SHA-256 as on the panel
                  │                → wait out 1/max_fps → PUT /api/v1/frame (X-Frame-ID, X-Frame-SHA256, X-Persist: false)
                  │                  failure: backoff 1 s → 60 s, cut short when the panel comes back
                  │    heartbeat:  POST /api/v1/heartbeat (Content-Length: 0) every heartbeat_s;
                  │                  fallback.active in the reply → re-push the frame
                  │    events:     WS /api/v1/events (Authorization: Bearer); backoff 1 s → 60 s
                  │                  hello → re-push the frame (the panel may have restarted)
                  │                  touch → Input.dispatchMouseEvent (pressed/moved/released, left, clickCount 1)
                  │                  button → logged; presence → ignored
                  └ admin HTTP (net/http)
```

- **Frame ids** are `<screen>-<8 hex per server start>-<counter>`.
- **Touches:** tt7d already sends logical 1280×800 coordinates, so x/y go to
  Chrome unchanged (clamped to the page). The first finger down drives the
  mouse until it lifts; other fingers are ignored. If the stream drops while
  a finger is down, the mouse button is released at its last position. A
  touch on a frame that is not the latest pushed is logged once per touch
  (at its first such event) and still delivered.
- **Restarts:** tt7d keeps no pushed frame across a restart (the server sends
  `X-Persist: false`, so frames never cost flash writes, SPEC §12). Every new
  event-stream connection starts with tt7d's `hello`, and the server answers
  it by re-pushing its current frame; tt7d skips the redraw if it already
  shows it. A heartbeat reply with `fallback.active: true` does the same.
- **Isolation:** each screen has its own goroutines, backoff and HTTP
  timeouts (10 s frame, 5 s heartbeat, 5 s WebSocket dial). Screencast
  frames are acknowledged at once whatever the panel is doing. A hung or
  dead panel holds up only its own loops (the e2e test checks both).
- **Shutdown:** SIGINT/SIGTERM stop the loops (closing each WebSocket), stop
  the admin server, close the tabs, then ask Chrome to quit (killed after
  5 s). If Chrome dies, tt7-server exits 1; nothing restarts it yet (S3).
- **Saving a URL:** BurntSushi/toml, like the other Go TOML libraries, drops
  comments when it re-encodes a file. So `PUT .../url` edits the text: it
  finds that screen's `url = ...` line and replaces only the quoted value,
  keeping the indent and any trailing comment. It then parses the result and
  requires it to equal the old config with only that URL changed, and swaps
  it in with a temp file, fsync and rename (mode kept). A `url` written as a
  multi-line string can't be rewritten and gets `save_failed`.

### Screencast, not polling (decided by experiment, 2026-09-28)

A throwaway probe on this box (Chrome 154 headless, `--no-sandbox`, chromedp)
compared `Page.startScreencast` with polling `Page.captureScreenshot`:

- Screencast worked headless. A static page gave one 1280×800 PNG frame
  (about 40 ms after the load) and then **nothing** until it changed.
- After a CDP click, the new frame arrived 10-20 ms later. The press
  itself gives one frame (the button's `:active` look) and the release
  another about 13 ms later, so the pacer pushes the first and holds the
  second until the 1/max_fps interval is up.
- A CSS animation drove it at about 60 frames/s (119 frames in the first
  2 s). The pacer samples that down to max_fps.
- Each `captureScreenshot` took 34-50 ms and **itself caused a repaint**
  (a screencast frame per call), so a poller would repaint the page on
  every poll and still add up to one poll interval of latency.

So tt7-server uses the screencast (PNG format, every frame acked at once) and
no polling. Unverified: pages that paint without the compositor noticing (none
found so far). The admin **Reload** button is the manual way out.

### Measured in the e2e test (this box, 2026-09-28)

- Touch written into a panel's input FIFO → new frame in its framebuffer:
  about 290-335 ms for the first tap, 135-195 ms for later ones (4 runs).
  That covers tt7d → WebSocket → CDP click → paint → screencast → pacer →
  PUT → tt7d decode → fb, all on localhost. A real panel adds Wi-Fi and the
  ARM decode time.
- Chrome for 2 idle screens: 14 processes, about 450 MiB PSS, 0.6% of one core.
- Panel restart → frame back: 0.3-0.7 s.

## Dependencies

| Module | Why |
|---|---|
| `github.com/chromedp/chromedp` (+ `cdproto`, `sysutil`) | Drives Chrome over CDP: launch, tabs, screencast, mouse events. Writing a CDP client by hand would be most of this program |
| `github.com/BurntSushi/toml` | Parses screens.toml, and lists undecoded keys so unknown keys are errors |
| `github.com/coder/websocket` | The client for tt7d's event stream: custom `Authorization` header on the dial, answers pings while reading, masks frames |

The other entries in go.mod (`gobwas/*`, `go-json-experiment/json`,
`golang.org/x/sys`) come in through chromedp.

## Tests

```sh
make server-check   # go vet, go test (units), then server/test_server_e2e.py
```

- **Units** (`go test ./...`): config parsing and every refusal (missing
  fields, bad host, bad URL, token file modes, duplicate names and hosts,
  unknown keys), the example config, URL write-back (comments kept,
  refusals leave the file alone), backoff, the pacer (dedup, max_fps,
  Forget), touch → mouse mapping, and the admin API's auth and errors.
- **End to end** (`server/test_server_e2e.py`, no mocks): two host-built
  tt7d daemons on file-backed 800×1280 RGB565 framebuffers with the panel's
  sysfs fixture and input FIFOs (the tt7d e2e harness), a real Chrome, and a
  page with a counter button whose colour follows the count. It checks the
  framebuffer pixels through tt7d's rotation (270), a real touch on panel A
  clicking the page (the page reports the click; A turns green, B doesn't
  change), heartbeats holding off a 5 s fallback timeout with no frames
  sent, the fallback coming back after SIGTERM, restarts of both panels
  getting their frame back, the admin API (list, URL change with pixels and
  screens.toml checked, preview bytes equal to the panel's frame), and
  panel A getting its frames while B is hung (SIGSTOP) and while B is down.
- The e2e needs Chrome at `CHROME` (default agent-browser's). If it isn't
  there, the test **fails**; it never skips.

## Unverified

- Everything against real panels over Wi-Fi (S2): latency, and how the
  panels' 8-connection HTTP limit behaves next to the control panel.
- Long runs: Chrome memory growth over days, and pages that leak.
- Pages that need a real GPU, audio, or a visible window.
- Multi-touch gestures (pinch, two-finger scroll) are not mapped: one finger
  is one mouse.
- Chrome dying mid-run makes tt7-server exit; no supervisor restarts it yet.
- A `PUT .../url` that races another for the same screen can leave the tab
  on one URL and the file on the other (saves are serialized; navigations
  are not).
