# tt7-server: the backend that drives N tt7 panels

tt7-server makes each tt7 panel show a web page. It runs one headless Chrome
with one tab per panel, pushes what the tab paints to the panel (the changed
rectangles with tt7d `PATCH /api/v1/frame`, or a whole PNG with `PUT`), and
turns touches on the glass into real mouse clicks in that tab. Any URL works: the server is a remote browser (the plan's
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
| `GET /api/screens` | `{"screens": [...]}`: `name`, `host`, `url`, `enabled`, `reachable`, `events_connected`, `device_id`, `last_push_at`, `last_frame_id`, `frames_pushed`, `last_error`, `last_error_at` (null when unknown); the page: `url_status` (`loading`, `ok` or `failed`; null for a disabled screen), `url_error` (why the last load of `url` failed; null once one works); frame traffic: `bytes_sent` (bodies the panel accepted), `full_frames`, `region_frames`, `base_mismatches` (PATCHes the panel refused with 409), `last_update` (`full`/`regions`), `last_region_count`, `last_push_bytes`, `last_push_ms` (the last accepted request, send to reply) |
| `PUT /api/screens/{name}/url` | Body `{"url": "https://..."}`. Saves it to screens.toml and answers 202 with the screen's status at once; the page loads in the background (watch `url_status`). 400 `invalid_url`, 404 `no_such_screen`, 500 `save_failed` |
| `POST /api/screens/{name}/reload` | Loads the configured `url` again, whatever the tab shows now; 202 at once (409 `reload_failed` on a disabled screen) |
| `GET /api/screens/{name}/preview.png` | The last frame the panel accepted (404 `no_frame`) |

The API needs `Authorization: Bearer <admin token>` when `admin_token_file` is
set, and the config requires one whenever `listen` is not a loopback address.

`tt7-server -config screens.toml -healthcheck` does not run the server: it asks
the running one (at the config's `listen`, with its admin token) for
`GET /api/screens` and exits 0 if that answers 200, else 1. The Docker
HEALTHCHECK uses it, so the image needs no curl.

## Docker

`server/Dockerfile` builds the server static (Go 1.26.6, `CGO_ENABLED=0`) and
runs it as uid/gid 1000 next to Debian trixie's Chromium, with Noto (core,
color emoji, CJK) and Liberation fonts. `server/compose.yaml` runs it.
Verified on docker-host (192.168.200.8, Docker 27.2.1, Compose v2.29.2) on
2026-09-28 with a panel-less config; see "Verified in Docker" below.

Layout next to `server/compose.yaml` (the directory `server/config/` is
git-ignored):

```text
server/config/                  mounted read-write at /config (the directory, see below)
  screens.toml                  from screens.docker.example.toml; PUT .../url rewrites it
  tokens/                       mounted read-only at /config/tokens; mode 700
    admin.token                 the admin API token (any random string), mode 600
    tabletop.token              each panel's /data/tt7/tt7d/token, mode 600
    wall.token
```

The container runs as **uid 1000, gid 1000** (build args `TT7_UID`/`TT7_GID`
change it). The token files and `screens.toml` must be readable by that uid
and the `config/` directory writable by it; tt7-server refuses a token file
that group or others can read. On docker-host uid 1000 is harper, so files
harper creates just work.

**Mount the directory, not the file.** `PUT .../url` writes a temp file next
to `screens.toml` and renames it over the old one (atomic). A single-file bind
mount breaks that: Docker creates `/config` owned by root, so the temp file
can't be made (seen in Docker: `open /config/.screens.toml.NNN: permission
denied`), and even as root, rename(2) onto a bind-mounted file fails with
EBUSY. Both errors say to mount the directory.

First time, on the machine that runs it:

```sh
cd server
mkdir -p config/tokens && chmod 700 config/tokens
cp screens.docker.example.toml config/screens.toml      # then set host/url per screen
head -c 24 /dev/urandom | base64 > config/tokens/admin.token
cp /path/to/tabletop-token config/tokens/tabletop.token  # each panel's token
chmod 600 config/tokens/*.token
docker compose up -d --build
docker compose ps                                        # STATUS shows (healthy) after ~10 s
curl -s -H "Authorization: Bearer $(cat config/tokens/admin.token)" http://127.0.0.1:7788/api/screens | jq .
```

- **Logs** go to stdout/stderr, kept by Docker's json-file driver (10 MB × 3
  files): `docker compose logs -f`.
- **Changing a URL:** the admin page at http://127.0.0.1:7788/ (it asks for
  the admin token), or `curl -X PUT -H "Authorization: Bearer $(cat
  config/tokens/admin.token)" -d '{"url":"http://..."}'
  http://127.0.0.1:7788/api/screens/wall/url`. It lands in
  `config/screens.toml` at once. Editing `screens.toml` by hand needs
  `docker compose restart`.
- **Upgrading** (new server code, or a Chromium security update):
  `git pull && docker compose build --pull && docker compose up -d`. The
  image pins its base images by digest but installs Debian's current
  Chromium at build time (Debian drops superseded chromium packages from
  its archive, so a pinned version would stop building); `--no-cache`
  forces a fresh Chromium even when nothing else changed. Bump the digests
  in the Dockerfile to move to a newer Debian point release.
- **Admin port:** published on the host's `127.0.0.1:7788` only
  (`TT7_ADMIN_PORT=7799 docker compose up -d` picks another host port). To
  reach it from the LAN, change `127.0.0.1` in `compose.yaml`; the admin
  token guards the API either way.
- **Restarts:** `restart: unless-stopped`. tt7-server exits 1 when Chrome
  dies, and Docker starts it again. A `docker kill` or `docker stop` counts
  as a manual stop, so Docker leaves it stopped until `docker compose up -d`
  or `docker start` (Docker's restart-policy rules).
- **Watchtower** runs on docker-host; the service carries
  `com.centurylinklabs.watchtower.enable=false` because its image is a local
  build with nothing to pull.

Choices, and why:

- **Debian's `chromium`, not `chromedp/headless-shell`.** headless-shell is
  made for chromedp and smaller (about 150 MB compressed on Docker Hub
  against this image's 955 MB on disk), but on 2026-09-28 its newest tag was
  151.0.7922.109 (Docker Hub, 2026-08-11) and its Containerfile
  (github.com/chromedp/docker-headless-shell) installs no fonts, so fonts
  would be added anyway. Debian trixie's `chromium` is
  154.0.8037.57-1~deb13u1 (packages.debian.org), the same Chrome version the
  e2e test runs on the dev box, it is the full browser in new headless mode
  as tested, and trixie-security keeps it patched. The cost is disk: in the
  built image Chromium is 376 MB, Noto CJK 90 MB, the other Noto fonts
  54 MB, Liberation 4 MB. CJK is the one to drop (`fonts-noto-cjk` in the
  Dockerfile) if 90 MB matters more than Chinese/Japanese/Korean text.
- **`--no-sandbox`** (in `screens.docker.example.toml`). Tried in Docker
  without it: Chrome stops with `No usable sandbox!`, both with the compose
  settings and without `no-new-privileges`. Docker's default seccomp profile
  blocks the user namespaces Chrome's sandbox needs, and Debian's setuid
  helper (`chromium-sandbox`) is a separate package that would need
  capabilities this container drops. The headless-shell README's way to keep
  the sandbox is a custom seccomp profile (jessfraz's `chrome.json`); that
  was not tried, and docker-host also sets
  `kernel.apparmor_restrict_unprivileged_userns = 1`, which may block it
  anyway (unverified). So the container is the boundary: non-root,
  `cap_drop: ALL`, `no-new-privileges`, and it loads only your dashboards.
- **No `shm_size`.** chromedp already starts Chrome with
  `--disable-dev-shm-usage` (chromedp v0.16.0 allocate.go,
  `DefaultExecAllocatorOptions`), so Chrome keeps its shared memory in /tmp
  and Docker's 64 MB /dev/shm is never the limit.
- **`init: true`**: Docker's tini as PID 1 reaps Chrome's exited helpers (the
  headless-shell README warns of zombie processes) and passes SIGTERM on.
- **Bridge network, not `network_mode: host`.** The default bridge reaches
  the dashboards: from inside the container, `GET http://192.168.200.8:5050/`
  answered `HTTP/1.0 302 Found`. Outbound traffic to the panels leaves
  through the host's routing and NAT the same way (not tried against the
  panels: the test kept away from them). Host networking would also work
  and would drop the NAT, but it would expose the admin port on every host
  interface instead of loopback only; use it only if a panel ever has to
  reach the server first (tt7d never does: the server dials the panels).
- **`mem_limit: 1g`**: Chrome with 2 idle tabs measured about 450 MiB PSS on
  the dev box; the panel-less container used 202 MiB. Real dashboards weigh
  more, so about twice the measured figure. If Chrome hits the limit, the
  kernel kills it, the server exits, and Docker restarts it.

### Verified in Docker (docker-host, 2026-09-28)

With `-p tt7-server-ci`, host port 7799, and a config whose one screen is
192.0.2.1 (TEST-NET-1, never answers) showing a `file://` page; no panel or
broker was contacted, and everything was removed afterwards.

- The image builds (955 MB); the build context is 133 kB (`.dockerignore`
  keeps `config/` and its tokens out).
- The container runs as `uid=1000(tt7)`; `docker top` shows docker-init,
  tt7-server and Chromium, all as uid 1000.
- Chromium starts headless (`tt7-server running`, then `page loaded` for the
  `file://` page 65 ms later, with no warning from the post-load screenshot).
  A test page with Latin, Greek, Cyrillic, Chinese, Japanese, Korean and
  color emoji rendered every glyph with the image's fonts.
- Healthy about 8 s after start; the admin API answers 401 without the
  token and the screen list with it; the port is published on 127.0.0.1
  only.
- `PUT .../url` answered 202 in 15 ms; `config/screens.toml` got a new inode
  (temp file + rename in the mounted directory), kept its comment, and no
  temp file was left. After the restarts below the server loaded the saved
  URL.
- `kill -9` of Chrome's browser process: tt7-server logged `Chrome exited;
  stopping`, exited 1, and Docker restarted it (healthy again in about 6 s).
  `kill -9` of tt7-server: the same.
- A single-file mount of `screens.toml` gave `save_failed: open
  /config/.screens.toml.NNN: permission denied` (hence the error hint).

Not verified in Docker: frames reaching a real panel, the screencast inside
the container (the host e2e covers it with the same Chrome version), and
hours-long runs.

## Running at home

Run the server near the panels, on the 192.168.200.0/24 LAN (docker-host,
192.168.200.8, also serves the dashboards on :5050). From this dev box every
frame and every touch crosses the tailnet twice. On the LAN they go straight
to the panel, and the dashboard pages load from the same machine.

## How it works

```text
screens.toml ──► tt7-server
                  ├ Chrome (headless, one process) ── one tab per enabled screen, 1280×800 at scale 1
                  │    Page.startScreencast (PNG) ──► screen.OnFrame (keeps only the newest)
                  ├ per screen, three goroutines:
                  │    push loop:  newest capture → skip if same SHA-256 as on the panel
                  │                → wait out 1/max_fps (not for the first capture after a touch down/up)
                  │                → diff against the panel's pixels: small change → PATCH /api/v1/frame (regions)
                  │                  else, or refused → PUT /api/v1/frame (X-Frame-ID, X-Frame-SHA256, X-Persist: false)
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
- **Region updates** (SPEC §10.1). The push loop keeps the RGBA of the frame
  the panel is known to show (its base), with that frame's id and sha256.
  Each capture is decoded and compared with the base exactly, in 32×32
  tiles; touching changed tiles make a group, each group's bounding box is
  a rectangle, and rectangles are merged (overlapping first, then the pair
  whose union adds the least unchanged area) until at most 4 remain. No
  padding for antialiasing is needed: the compare is exact, so a pixel that
  moved by one level is a changed pixel, and its tile covers it. Each
  rectangle is encoded as a PNG (Go's `BestSpeed`: 0.5 ms and 511 bytes for
  a 256×128 button here, against 0.76 ms and 480 bytes at the default
  level) and the batch goes as one PATCH with `X-Base-Frame-ID` and the
  expected `X-Frame-SHA256`. A full PUT goes instead when the base is
  unknown (start, a `hello`, a fallback reported by a heartbeat, any push
  error), when the rectangles cover more than `region_max_fraction` of the
  screen (per screen, default 0.5; 0 turns regions off), or when the
  container would not be smaller than the PNG. Pixels identical to the base
  send nothing.
- **Refused regions.** 409 `base_mismatch` (the panel restarted, shows its
  fallback clock, or someone else PUT a frame) counts in `base_mismatches`
  and is followed at once by a full PUT, which becomes the new base. Any
  other refusal does the same. A panel that answers PATCH with 404, 405 or
  415 (a tt7d from before region updates) gets full frames until its event
  stream reconnects.
- **A touch's result goes at once.** Each touch down and each touch up
  lets one capture (within 1 s) skip the `max_fps` wait: the pressed look,
  then the result. Duplicates are still skipped, and at most two are owed
  at a time. Otherwise the rate limit holds as before.
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
  5 s). If Chrome dies, tt7-server exits 1, so a supervisor must restart it
  (Docker's restart policy does, see "Docker").
- **Loading the page** (internal/screen/nav.go): one loop per screen loads
  the configured `url` at start, on `PUT .../url` and on `reload`. Those
  API calls only kick the loop and answer 202 at once; a newer kick
  abandons a load still under way, so the tab always ends on the URL that
  was saved last (saves and kicks share one lock). A load **fails** when
  Chrome reports a network error for it (`Page.navigate`'s `errorText`,
  e.g. `net::ERR_CONNECTION_REFUSED`, `ERR_NAME_NOT_RESOLVED`), when the
  page's document comes back with HTTP 400 or more (chromedp `RunResponse`
  gives that document's response), or after 30 s. A failed load is
  retried after 5 s, 10 s, 20 s ... up to 5 min between tries, until one
  works or the URL changes; `url_status` and `url_error` say where it
  stands. Frames reach the panel only while the configured URL is loaded:
  while a load is under way or failed, screencast frames (Chrome's error
  page, a 502 page) are dropped and the panel keeps its last good frame (or
  its fallback clock takes over after tt7d's timeout). Once a load works,
  the loop takes one screenshot and offers it as a capture, since Chrome
  may have painted the page before the gate opened and a still page does
  not paint again; a screencast frame that came in first wins. A page
  that loaded and then lands on Chrome's error page by itself (CDP
  `Frame.unreachableUrl` on the main frame) counts as a failed load too.
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
  about 290-340 ms for the first tap, 130-195 ms for later ones (5 runs),
  before region updates and the touch bypass. That covers tt7d → WebSocket →
  CDP click → paint → screencast → pacer → PUT → tt7d decode → fb, all on
  localhost. A real panel adds Wi-Fi and the ARM decode time.
- With region updates and the touch bypass, on the dashboard page (a busy
  1280×620 area above a 200×100 button; a full frame is about 135 KB):
  panel C (regions) 42-98 ms and 0.8-1.4 KB per tap; panel D, the same page
  with `region_max_fraction = 0` in the same run, 155-239 ms and about
  135 KB per tap (2 runs). The counter page (a 800×600 button, 49% of the
  screen in tiles, so still regions, about 8 KB): 93-146 ms, against
  198-329 ms on main in the same box. Localhost only, with the host tt7d built with ASan; the panel's
  Wi-Fi and A9 will differ.
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
  Forget, the touch bypass), touch → mouse mapping, the page loader (URL
  changes and reloads return at once and abandon a hung load, retries back
  off 5 s → 5 min and start over after a success, frames are held while a
  load is under way or failed, the post-load screenshot never replaces a
  newer frame, an error page after a load triggers a retry), the admin API's auth
  and errors, and regions: the tile diff on known images (one change, two
  far apart, six merged into four, faint edge changes, random changes always
  covered, full frame above the fraction), the container codec, and the
  golden vector shared with tt7d (`tt7d/test/fixtures/regions-v1.*`; rewrite
  it with `go test ./internal/regions -run TestGoldenVector -update`).
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
  Two more panels show a dashboard page: C with region updates, D with
  `region_max_fraction = 0`. A tap on C must go as regions under a tenth of a
  full frame, and afterwards C's framebuffer and `/frame/image` must equal the
  page Chrome painted (decoded with pinned Pillow, rotated here). A frame PUT
  to C by someone else, then C's fallback clock (heartbeat_s 60), must each
  turn the next tap into a 409 and a full frame; a restart of C must resync
  it in full, and taps after each must be regions again. B's navigation to
  another page must be a full frame. Then B's URL goes to a host that
  accepts but never answers (the PUT answers 202 in under 1 s; a reload
  requests that configured URL again), then to a port nothing listens on
  (`url_status` failed with `ERR_CONNECTION_REFUSED`, and B's framebuffer
  and accepted-frame count stay exactly as they were: no error page is
  pushed), then a server comes up on that port and B shows its page with
  no API call. A URL answering 404 fails the same way, with nothing pushed.
- The e2e needs Chrome at `CHROME` (default agent-browser's). If it isn't
  there, the test **fails**; it never skips.

## Unverified

- Region updates on the real panels: the gain over Wi-Fi and on the A9's
  decode, and pages whose changes are large or scattered (4 rectangles merge
  them, at the cost of sending some unchanged pixels).
- Everything against real panels over Wi-Fi (S2): latency, and how the
  panels' 8-connection HTTP limit behaves next to the control panel.
- Long runs: Chrome memory growth over days, and pages that leak.
- Pages that need a real GPU, audio, or a visible window.
- Multi-touch gestures (pinch, two-finger scroll) are not mapped: one finger
  is one mouse.
- Chrome dying mid-run makes tt7-server exit 1; only a supervisor (Docker's
  `restart: unless-stopped`, see "Docker") brings it back.
- A page that fails a load of its own after it loaded (it reloads itself
  while its server is down) is caught by Chrome's error-page signal
  (`unreachableUrl`), which only unit tests exercise. A page that turns
  itself into an HTTP error page that way is not caught: only our own loads
  see the status code.
