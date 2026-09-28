#!/usr/bin/env python3
# ABOUTME: tt7-server end-to-end test: four real host-built tt7d daemons, real headless Chrome, real test pages.
# ABOUTME: Checks pixels, touches-as-clicks, region updates vs full frames, heartbeats, restarts, admin API, isolation.
"""Usage: server/test_server_e2e.py --daemon build/host/tt7d --server build/server/tt7-server --chrome PATH
          (run by `make server-check`, through uv for the pinned Pillow that decodes PNGs here)

Nothing is mocked. Each tt7d runs on a file-backed 800x1280 RGB565 framebuffer
with the panel's sysfs fixture and FIFOs for its input devices (the harness
of tt7d/test_e2e.py and tt7d/test_input_e2e.py); touches are real
`struct input_event` records. tt7-server runs its real Chrome against a page
served here, and the expected pixels are computed here from the page's
known colours and tt7d's reported rotation, never by asking the server.

Chrome is required: if --chrome does not exist the test FAILS (it never skips).
"""
import argparse
import http.server
import json
import os
import re
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tt7d"))
import test_e2e  # noqa: E402 - the tt7d Daemon wrapper (file-backed fb, sysfs fixture)

NATIVE_W, NATIVE_H, STRIDE = test_e2e.NATIVE_W, test_e2e.NATIVE_H, test_e2e.STRIDE
EV_SYN, EV_ABS = 0, 3
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2F, 0x35, 0x36, 0x39
RAW_X_MAX, RAW_Y_MAX = 1280, 800  # gslX680's ranges as tt7probe read them on the panel
ABSINFO = "0x2f 0 10\n0x35 0 1280\n0x36 0 800\n"
FALLBACK_S = 5    # the daemons' --fallback-timeout
HEARTBEAT_S = 2   # the screens' heartbeat_s

# The counter page: a big button whose colour follows the click count. Pure colours survive RGB565 exactly.
COLOURS = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 255, 255)]
BUTTON = (240, 100, 800, 600)  # left, top, width, height in logical pixels
SAMPLE = (260, 120)            # inside the button, clear of the number
TAP = (420, 260)               # where the finger lands, inside the button
BACKGROUND_SAMPLE = (100, 750)  # outside the button: the page's black
MAGENTA = (255, 0, 255)

COUNTER_HTML = """<!doctype html><html><head><meta charset="utf-8"><style>
html, body { margin: 0; width: 1280px; height: 800px; background: #000; overflow: hidden; }
#b { position: absolute; left: %dpx; top: %dpx; width: %dpx; height: %dpx; border: 0; margin: 0; padding: 0;
     outline: none; font: bold 300px sans-serif; color: #fff; background: rgb(255, 0, 0); }
</style></head><body><button id="b">0</button><script>
var colours = %s, n = 0, b = document.getElementById("b");
var screen = new URLSearchParams(location.search).get("screen");
b.addEventListener("click", function () {
  n++;
  b.textContent = n;
  b.style.background = "rgb(" + colours[n %% colours.length].join(",") + ")";
  fetch("/clicked?screen=" + encodeURIComponent(screen) + "&n=" + n);
});
</script></body></html>""" % (*BUTTON, json.dumps(COLOURS))

# The dashboard page: a large busy area that never changes above a small button that does. A tap should
# send only the button (a region update); a full frame of this page is tens of kilobytes.
DASH_BUTTON = (1000, 650, 200, 100)  # left, top, width, height
DASH_SAMPLE = (1010, 660)            # inside the button, clear of its number
DASH_TAP = (1100, 700)
DASH_FALLBACK_S = 8                  # panel C's --fallback-timeout; its heartbeat_s is 60, so the clock does come
DASH_HTML = """<!doctype html><html><head><meta charset="utf-8"><style>
html, body { margin: 0; width: 1280px; height: 800px; background: rgb(16, 32, 48); overflow: hidden; }
#art { position: absolute; left: 0; top: 0; }
#b { position: absolute; left: %dpx; top: %dpx; width: %dpx; height: %dpx; border: 0; margin: 0; padding: 0;
     outline: none; font: bold 60px sans-serif; color: #fff; background: rgb(255, 0, 0); }
</style></head><body><canvas id="art" width="1280" height="620"></canvas><button id="b">0</button><script>
var c = document.getElementById("art").getContext("2d"), seed = 12345;
function rnd() { seed = (seed * 1103515245 + 12345) %% 2147483648; return seed / 2147483648; }
for (var y = 0; y < 620; y += 20) for (var x = 0; x < 1280; x += 20) {
  c.fillStyle = "rgb(" + [rnd() * 255 | 0, rnd() * 255 | 0, rnd() * 255 | 0].join(",") + ")";
  c.fillRect(x, y, 20, 20);
}
c.font = "18px sans-serif"; c.fillStyle = "#fff";
for (var i = 0; i < 30; i++) c.fillText("sensor " + i + ": " + (rnd() * 100).toFixed(2) + " units", 20 + (i %% 4) * 310, 20 + (i >> 2) * 60);
var colours = %s, n = 0, b = document.getElementById("b");
b.addEventListener("click", function () {
  n++;
  b.textContent = n;
  b.style.background = "rgb(" + colours[n %% colours.length].join(",") + ")";
});
</script></body></html>""" % (*DASH_BUTTON, json.dumps(COLOURS))

SECOND_HTML = """<!doctype html><html><head><meta charset="utf-8"><style>
html, body { margin: 0; background: rgb(255, 0, 255); font: 120px sans-serif; }
</style></head><body><p style="margin: 300px 0 0 500px">SECOND</p></body></html>"""


def wait_for(what, cond, timeout=20.0, step=0.02):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = cond()
        if value:
            return value
        time.sleep(step)
    raise AssertionError(f"timed out after {timeout:.0f} s waiting for {what}")


class TestPages:
    """The pages the screens show, plus /clicked, which the counter page calls on every click."""

    def __init__(self):
        self.clicks = {}  # screen -> highest n reported
        pages = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                u = urllib.parse.urlparse(self.path)
                q = urllib.parse.parse_qs(u.query)
                if u.path == "/counter":
                    body = COUNTER_HTML.encode()
                elif u.path == "/second":
                    body = SECOND_HTML.encode()
                elif u.path == "/dashboard":
                    body = DASH_HTML.encode()
                elif u.path == "/clicked":
                    pages.clicks[q["screen"][0]] = max(pages.clicks.get(q["screen"][0], 0), int(q["n"][0]))
                    body = b"ok"
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def url(self, path):
        return f"http://127.0.0.1:{self.port}{path}"

    def stop(self):
        self.httpd.shutdown()


class Panel(test_e2e.Daemon):
    """A tt7d with FIFOs for its buttons (event0) and touchscreen (event1), as in test_input_e2e.py."""

    def __init__(self, binary, workdir, extra_args=()):
        os.makedirs(workdir)
        super().__init__(binary, workdir, extra_args)
        self.rotation = None  # tt7d's own default (270), read back from /info
        for node in ("event0", "event1"):
            os.mkfifo(os.path.join(self.input_dir, node))
        with open(os.path.join(self.input_dir, "event1.absinfo"), "w") as f:
            f.write(ABSINFO)
        self.fds = []
        self.tracking = 100

    def start(self):
        super().start()
        # The daemon holds the read ends open, so these opens return at once.
        self.fds = [os.open(os.path.join(self.input_dir, n), os.O_WRONLY) for n in ("event0", "event1")]

    def stop(self):
        super().stop()
        for fd in self.fds:
            os.close(fd)
        self.fds = []

    def get(self, path):
        status, _, body = self.request("GET", path)
        assert status == 200, f"GET {path}: {status} {body[:200]!r}"
        return json.loads(body)

    def rot(self):
        return self.get("/api/v1/info")["display"]["rotation"]

    def pixel(self, x, y, rotation):
        """The RGB565 value on the native fb where logical (x, y) is drawn (tt7d/README.md rotation rule)."""
        if rotation == 270:
            nx, ny = y, NATIVE_H - 1 - x
        elif rotation == 90:
            nx, ny = NATIVE_W - 1 - y, x
        else:
            raise ValueError(rotation)
        with open(self.fb, "rb") as f:
            f.seek(ny * STRIDE + nx * 2)
            return struct.unpack("<H", f.read(2))[0]

    def tap(self, x, y, rotation):
        """A real one-finger tap (down, then up) at logical (x, y), written into the touch FIFO."""
        rx, ry = raw_for(x, y, rotation)
        self.tracking += 1
        recs = [rec(EV_ABS, ABS_MT_SLOT, 0), rec(EV_ABS, ABS_MT_TRACKING_ID, self.tracking),
                rec(EV_ABS, ABS_MT_POSITION_X, rx), rec(EV_ABS, ABS_MT_POSITION_Y, ry), rec(EV_SYN, 0, 0),
                rec(EV_ABS, ABS_MT_TRACKING_ID, -1), rec(EV_SYN, 0, 0)]
        os.write(self.fds[1], b"".join(recs))

    def frame_id(self):
        status, _, body = self.request("GET", "/api/v1/frame")
        return json.loads(body)["frame_id"] if status == 200 else None

    def fallback(self):
        return self.get("/api/v1/state")["fallback"]["active"]

    def fb_bytes(self):
        with open(self.fb, "rb") as f:
            return f.read()


def png_rgb(png):
    """A PNG's pixels as RGB bytes (Pillow; alpha dropped: the panel shows none)."""
    import io
    return Image.open(io.BytesIO(png)).convert("RGB").tobytes()


def fb_from_png(png):
    """The RGB565 framebuffer tt7d should hold for a logical PNG at rotation 270: logical (x, y) lands on
    native (y, 1279 - x), which is the image turned 90 degrees counter-clockwise (Pillow's ROTATE_90)."""
    import io
    rot = Image.open(io.BytesIO(png)).convert("RGB").transpose(Image.Transpose.ROTATE_90).tobytes()
    out = bytearray(len(rot) // 3 * 2)
    for i in range(len(rot) // 3):
        r, g, b = rot[3 * i], rot[3 * i + 1], rot[3 * i + 2]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out[2 * i], out[2 * i + 1] = v & 0xFF, v >> 8
    return bytes(out)


def rec(etype, code, value):
    """One struct input_event in the host's native layout."""
    return struct.pack("@llHHi", 0, 0, etype, code, value)


def raw_for(x, y, rotation):
    """The raw touch position that tt7d maps nearest logical (x, y). tt7d scales raw x across the
    native fb width and raw y down its height (like tt7probe's dots), then undoes the rotation:
    at 270 logical (x, y) is native (y, 1279 - x); at 90 it is native (799 - y, x) (tt7d/README.md)."""
    nx, ny = (y, NATIVE_H - 1 - x) if rotation == 270 else (NATIVE_W - 1 - y, x)
    rx = min(range(RAW_X_MAX + 1), key=lambda r: abs(r * (NATIVE_W - 1) // RAW_X_MAX - nx))
    ry = min(range(RAW_Y_MAX + 1), key=lambda r: abs(r * (NATIVE_H - 1) // RAW_Y_MAX - ny))
    return rx, ry


def rgb565(c):
    r, g, b = c
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def http_json(port, method, path, doc=None, token=None):
    import http.client
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=40)
    headers = {"Content-Type": "application/json"} if doc is not None else {}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    try:
        conn.request(method, path, body=json.dumps(doc).encode() if doc is not None else None, headers=headers)
        resp = conn.getresponse()
        return resp.status, resp.read()
    finally:
        conn.close()


def free_port():
    return test_e2e.free_port()


class Server:
    def __init__(self, binary, workdir, config):
        self.binary, self.config = binary, config
        self.log_path = os.path.join(workdir, "tt7-server.log")
        self.proc = None

    def start(self):
        log = open(self.log_path, "ab")
        self.proc = subprocess.Popen([self.binary, "-config", self.config], stdout=log, stderr=subprocess.STDOUT)
        log.close()

    def log(self):
        with open(self.log_path, "rb") as f:
            return f.read().decode("utf-8", "replace")

    def descendants(self):
        """PIDs of every process under the server (Chrome and its helpers)."""
        parents = {}
        for p in os.listdir("/proc"):
            if p.isdigit():
                try:
                    with open(f"/proc/{p}/stat") as f:
                        fields = f.read().rsplit(")", 1)[1].split()
                    parents.setdefault(int(fields[1]), []).append(int(p))
                except OSError:
                    pass
        out, todo = [], [self.proc.pid]
        while todo:
            kids = parents.get(todo.pop(), [])
            out += kids
            todo += kids
        return out

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                return self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
                raise AssertionError("tt7-server did not exit within 20 s of SIGTERM")
        return self.proc.returncode if self.proc else None


def chrome_usage(pids, window=5.0):
    """Total PSS (MiB) of the processes, and their CPU use (% of one core) over an idle window."""
    def cpu_ticks():
        total = 0
        for p in pids:
            try:
                with open(f"/proc/{p}/stat") as f:
                    fields = f.read().rsplit(")", 1)[1].split()
                total += int(fields[11]) + int(fields[12])
            except OSError:
                pass
        return total
    t0, c0 = time.monotonic(), cpu_ticks()
    time.sleep(window)
    t1, c1 = time.monotonic(), cpu_ticks()
    hz = os.sysconf("SC_CLK_TCK")
    pss_kib = 0
    for p in pids:
        try:
            with open(f"/proc/{p}/smaps_rollup") as f:
                m = re.search(r"^Pss:\s+(\d+) kB", f.read(), re.M)
                pss_kib += int(m.group(1)) if m else 0
        except OSError:
            pass
    return pss_kib / 1024, 100.0 * (c1 - c0) / hz / (t1 - t0)


def screen_status(admin_port, name):
    return {s["name"]: s for s in json.loads(http_json(admin_port, "GET", "/api/screens")[1])["screens"]}[name]


def tap_and_measure(panel, admin_port, name, n, rot):
    """Tap the dashboard button; return (ms to the new colour on the fb, bytes sent for the tap, status after)."""
    before = screen_status(admin_port, name)
    t0 = time.monotonic()
    panel.tap(*DASH_TAP, rot)
    wait_for(f"colour {n} on panel {name}", lambda: panel.pixel(*DASH_SAMPLE, rot) == rgb565(COLOURS[n % len(COLOURS)]),
             timeout=10, step=0.005)
    ms = (time.monotonic() - t0) * 1000
    time.sleep(0.4)  # let the tap's last capture (the release) go out too
    after = screen_status(admin_port, name)
    return ms, after["bytes_sent"] - before["bytes_sent"], before, after


def dirty_rect_steps(steps, info, c, dd, rot, admin_port, pages):
    """Region updates on panel C against full frames on panel D, both showing the dashboard page."""
    steps.append("7a. C and D show the dashboard, each from one full frame")
    for p, name in ((c, "c"), (dd, "d")):
        wait_for(f"the dashboard on {name}", lambda p=p: p.pixel(*DASH_SAMPLE, rot) == rgb565(COLOURS[0]), timeout=20)
        wait_for(f"{name}'s status", lambda name=name: screen_status(admin_port, name)["full_frames"] >= 1)
    full_bytes = len(http_json(admin_port, "GET", "/api/screens/c/preview.png")[1])  # what one full frame costs

    steps.append("7b. a tap on C sends only the button (regions), much smaller than a full frame; D sends full frames")
    c_ms, c_bytes, d_ms, d_bytes = [], [], [], []
    for n in (1, 2, 3, 4):
        ms, sent, before, after = tap_and_measure(c, admin_port, "c", n, rot)
        assert after["region_frames"] > before["region_frames"] and after["full_frames"] == before["full_frames"], \
            f"tap {n} on C was not sent as regions: {before} -> {after}"
        assert after["last_update"] == "regions" and 1 <= after["last_region_count"] <= 4, after
        assert sent * 10 < full_bytes, f"tap {n} on C cost {sent} bytes, a full frame is {full_bytes}"
        c_ms.append(ms)
        c_bytes.append(sent)
    fb_c = c.fb_bytes()  # read at once: C's fallback clock comes DASH_FALLBACK_S after the last frame
    image_c = c.request("GET", "/api/v1/frame/image")[2]
    preview_c = http_json(admin_port, "GET", "/api/screens/c/preview.png")[1]
    meta_c = c.get("/api/v1/frame")
    assert meta_c["updated_via"] == "regions" and meta_c["regions"] >= 1, meta_c
    for n in (1, 2, 3, 4):
        ms, sent, before, after = tap_and_measure(dd, admin_port, "d", n, rot)
        assert after["region_frames"] == 0 and after["full_frames"] > before["full_frames"], after
        d_ms.append(ms)
        d_bytes.append(sent)
    info.append(f"dashboard full frame: {full_bytes} bytes")
    info.append("tap -> panel fb, regions (C): " + ", ".join(f"{x:.0f} ms" for x in c_ms)
                + "; bytes per tap: " + ", ".join(str(x) for x in c_bytes))
    info.append("tap -> panel fb, full frames (D): " + ", ".join(f"{x:.0f} ms" for x in d_ms)
                + "; bytes per tap: " + ", ".join(str(x) for x in d_bytes))

    steps.append("7c. after region updates C's framebuffer and /frame/image are exactly the page Chrome painted")
    assert png_rgb(image_c) == png_rgb(preview_c), "C's composed /frame/image differs from the page"
    assert fb_c == fb_from_png(preview_c), "C's framebuffer differs from the page, rotated"

    steps.append("7d. another sender PUTs a frame to C: the next region update gets 409 and a full frame resyncs")
    other = test_e2e.png_bytes(1280, 800, test_e2e.random_image(99))
    assert c.put_frame(other, **{"X-Frame-ID": "someone-else"})[0] == 200
    before = screen_status(admin_port, "c")
    c.tap(*DASH_TAP, rot)
    wait_for("C back on the page", lambda: c.pixel(*DASH_SAMPLE, rot) == rgb565(COLOURS[5 % len(COLOURS)]), timeout=10)
    wait_for("C's full resync", lambda: screen_status(admin_port, "c")["full_frames"] > before["full_frames"])
    after = screen_status(admin_port, "c")
    assert after["base_mismatches"] == before["base_mismatches"] + 1, after
    assert c.fb_bytes() == fb_from_png(http_json(admin_port, "GET", "/api/screens/c/preview.png")[1])
    ms, sent, before, after = tap_and_measure(c, admin_port, "c", 6, rot)
    assert after["last_update"] == "regions", f"the tap after the resync was not regions: {after}"

    steps.append(f"7e. C's fallback clock takes over ({DASH_FALLBACK_S} s, no heartbeat); a tap gets 409 and a full frame")
    wait_for("C's fallback clock", c.fallback, timeout=DASH_FALLBACK_S + 4, step=0.25)
    before = screen_status(admin_port, "c")
    c.tap(*DASH_TAP, rot)
    wait_for("C back on the page", lambda: not c.fallback() and c.pixel(*DASH_SAMPLE, rot) == rgb565(COLOURS[7 % len(COLOURS)]),
             timeout=10)
    wait_for("C's full resync", lambda: screen_status(admin_port, "c")["full_frames"] > before["full_frames"])
    after = screen_status(admin_port, "c")
    assert after["base_mismatches"] == before["base_mismatches"] + 1, after
    assert c.get("/api/v1/frame")["updated_via"] == "full"

    steps.append("7f. C restarts: the server resyncs it with a full frame, then taps are regions again")
    c.stop()
    before = screen_status(admin_port, "c")
    c.start()
    wait_for("C's frame after the restart", lambda: (c.frame_id() or "").startswith("c-")
             and c.pixel(*DASH_SAMPLE, rot) == rgb565(COLOURS[7 % len(COLOURS)]), timeout=15)
    assert c.get("/api/v1/frame")["updated_via"] == "full"
    assert screen_status(admin_port, "c")["full_frames"] > before["full_frames"]
    ms, sent, before, after = tap_and_measure(c, admin_port, "c", 8, rot)
    assert after["last_update"] == "regions" and sent * 10 < full_bytes, after


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--daemon", required=True, help="host-built tt7d")
    ap.add_argument("--server", required=True, help="the tt7-server binary")
    ap.add_argument("--chrome", required=True, help="the Chrome binary tt7-server should run")
    args = ap.parse_args()
    if not os.access(args.chrome, os.X_OK):
        print(f"FAIL test_server_e2e: Chrome not found at {args.chrome} (the e2e needs a real Chrome; "
              "set CHROME=... for make server-check)", file=sys.stderr)
        return 1
    daemon_bin, server_bin = os.path.abspath(args.daemon), os.path.abspath(args.server)

    steps, info = [], []
    with tempfile.TemporaryDirectory(prefix="tt7-server-e2e-") as workdir:
        pages = TestPages()
        flags = ["--fallback-timeout", str(FALLBACK_S), "--ntp-marker", os.path.join(workdir, "ntp-synced")]
        a = Panel(daemon_bin, os.path.join(workdir, "a"), flags)
        b = Panel(daemon_bin, os.path.join(workdir, "b"), flags)
        c_flags = ["--fallback-timeout", str(DASH_FALLBACK_S), "--ntp-marker", os.path.join(workdir, "ntp-synced")]
        c = Panel(daemon_bin, os.path.join(workdir, "c"), c_flags)  # dashboard, region updates
        dd = Panel(daemon_bin, os.path.join(workdir, "d"), flags)   # dashboard, full frames only (the "before")
        config = os.path.join(workdir, "screens.toml")
        server = Server(server_bin, workdir, config)
        admin_port = free_port()
        try:
            a.start()
            b.start()
            c.start()
            dd.start()
            for d in (a, b, c, dd):
                mode = stat.S_IMODE(os.stat(os.path.join(d.data, "token")).st_mode)
                assert mode & 0o077 == 0, f"tt7d wrote its token with mode {mode:o}"
            with open(config, "w") as f:
                f.write(f"""# tt7-server e2e config
listen = "127.0.0.1:{admin_port}"
chrome_path = "{args.chrome}"
chrome_flags = ["--no-sandbox"]  # AppArmor userns on this box (gotchas.md)

# panel A
[[screen]]
name = "a"
host = "127.0.0.1:{a.port}"
token_file = "{a.data}/token"
url = "{pages.url('/counter?screen=a')}"
heartbeat_s = {HEARTBEAT_S}

# panel B, the one whose URL the admin API changes
[[screen]]
name = "b"
host = "127.0.0.1:{b.port}"
token_file = "{b.data}/token"
url = "{pages.url('/counter?screen=b')}"   # the counter, for now
heartbeat_s = {HEARTBEAT_S}

# panel C: the dashboard, sent as regions; heartbeats rare, so its fallback clock comes and the server finds out
[[screen]]
name = "c"
host = "127.0.0.1:{c.port}"
token_file = "{c.data}/token"
url = "{pages.url('/dashboard?screen=c')}"
heartbeat_s = 60

# panel D: the same dashboard, full frames only
[[screen]]
name = "d"
host = "127.0.0.1:{dd.port}"
token_file = "{dd.data}/token"
url = "{pages.url('/dashboard?screen=d')}"
heartbeat_s = {HEARTBEAT_S}
region_max_fraction = 0
""")
            rot = a.rot()
            assert rot == b.rot() == 270, f"tt7d's default rotation is {rot}, the test expects 270"
            red, green = rgb565(COLOURS[0]), rgb565(COLOURS[1])
            server.start()

            steps.append("1. both framebuffers show the counter page from Chrome (button red, background black)")
            for d in (a, b):
                wait_for(f"the red button on panel {d.port}", lambda d=d: d.pixel(*SAMPLE, rot) == red)
                assert d.pixel(*BACKGROUND_SAMPLE, rot) == 0, hex(d.pixel(*BACKGROUND_SAMPLE, rot))
                fid = d.frame_id()
                assert fid and fid.startswith(("a-", "b-")), f"frame id {fid!r} is not tt7-server's"
                meta = d.get("/api/v1/frame")
                assert (meta["width"], meta["height"], meta["persisted"]) == (1280, 800, False), meta

            dirty_rect_steps(steps, info, c, dd, rot, admin_port, pages)

            steps.append("5a. GET /api/screens reports both screens, reachable, with their device ids")
            wait_for("both event streams connected", lambda: all(
                s["events_connected"] for s in json.loads(http_json(admin_port, "GET", "/api/screens")[1])["screens"]))
            status, body = http_json(admin_port, "GET", "/api/screens")
            screens = {s["name"]: s for s in json.loads(body)["screens"]}
            for name, d, path in (("a", a, "/counter?screen=a"), ("b", b, "/counter?screen=b")):
                s = screens[name]
                assert s["host"] == f"127.0.0.1:{d.port}" and s["url"] == pages.url(path), s
                assert s["enabled"] and s["reachable"] and s["frames_pushed"] >= 1, s
                assert s["device_id"] == d.get("/api/v1/info")["device_id"], s
                assert s["last_frame_id"] == d.frame_id(), (s, d.frame_id())
                assert re.fullmatch(r"\d{4}-\d\d-\d\dT[\d:.]+Z", s["last_push_at"]), s

            steps.append("5c. preview.png is byte-identical to the frame the panel holds")
            for name, d in (("a", a), ("b", b)):
                status, png = http_json(admin_port, "GET", f"/api/screens/{name}/preview.png")
                _, _, held = d.request("GET", "/api/v1/frame/image")
                assert status == 200 and png == held, f"{name}: preview {len(png)} bytes, panel {len(held)}"

            steps.append("2. a touch on panel A clicks the page: counter 1, green on A; B untouched")
            b_frame = b.frame_id()
            t0 = time.monotonic()
            a.tap(*TAP, rot)
            wait_for("the green button on panel A", lambda: a.pixel(*SAMPLE, rot) == green, timeout=10, step=0.005)
            latency_ms = (time.monotonic() - t0) * 1000
            wait_for("the page to report click 1", lambda: pages.clicks.get("a") == 1)
            time.sleep(1)
            assert pages.clicks == {"a": 1}, pages.clicks
            assert b.frame_id() == b_frame and b.pixel(*SAMPLE, rot) == red, "panel B changed"
            lat = [latency_ms]
            for n in (2, 3):  # a few more, for the latency figure
                t0 = time.monotonic()
                a.tap(*TAP, rot)
                wait_for(f"colour {n} on A", lambda n=n: a.pixel(*SAMPLE, rot) == rgb565(COLOURS[n]), timeout=10,
                         step=0.005)
                lat.append((time.monotonic() - t0) * 1000)
                time.sleep(0.5)
            s_a = screen_status(admin_port, "a")
            info.append("touch -> new frame on the panel fb: " + ", ".join(f"{x:.0f} ms" for x in lat)
                        + f" (counter page; last update {s_a['last_update']}, {s_a['last_push_bytes']} bytes)")
            clicks_a = 3

            steps.append(f"3a. heartbeats (every {HEARTBEAT_S} s) keep fallback.active false past the "
                         f"{FALLBACK_S} s timeout, with no frames sent")
            accepted = [d.get("/api/v1/state")["frames"]["accepted"] for d in (a, b)]
            deadline = time.monotonic() + FALLBACK_S + 4
            while time.monotonic() < deadline:
                assert not a.fallback() and not b.fallback(), "a panel fell back to its clock"
                time.sleep(0.25)
            assert [d.get("/api/v1/state")["frames"]["accepted"] for d in (a, b)] == accepted, \
                "frames were pushed during the heartbeat window, so it did not test heartbeats alone"

            pids = server.descendants()
            pss, cpu = chrome_usage(pids)
            info.append(f"Chrome for 4 idle screens: {len(pids)} processes, {pss:.0f} MiB PSS, {cpu:.1f}% of one core")

            steps.append("6a. panel B hung (SIGSTOP): panel A still gets its frame at once")
            os.kill(b.proc.pid, signal.SIGSTOP)
            try:
                time.sleep(HEARTBEAT_S + 1)  # let B's heartbeat get stuck
                t0 = time.monotonic()
                a.tap(*TAP, rot)
                clicks_a += 1
                wait_for("A's next colour while B hangs",
                         lambda: a.pixel(*SAMPLE, rot) == rgb565(COLOURS[clicks_a % len(COLOURS)]), timeout=3)
                info.append(f"touch -> frame on A while B hangs: {(time.monotonic() - t0) * 1000:.0f} ms")
            finally:
                os.kill(b.proc.pid, signal.SIGCONT)

            steps.append("6b. panel B stopped (unreachable): panel A still gets its frames; the server says B is down")
            b.stop()
            wait_for("B reported unreachable", lambda: not {s["name"]: s for s in json.loads(
                http_json(admin_port, "GET", "/api/screens")[1])["screens"]}["b"]["reachable"], timeout=15)
            a.tap(*TAP, rot)
            clicks_a += 1
            wait_for("A's next colour while B is down",
                     lambda: a.pixel(*SAMPLE, rot) == rgb565(COLOURS[clicks_a % len(COLOURS)]), timeout=3)

            steps.append("4. restarted panels get the frame re-pushed with no page change (B, then A)")
            b.start()
            t0 = time.monotonic()
            wait_for("B's frame re-pushed", lambda: (b.frame_id() or "").startswith("b-")
                     and b.pixel(*SAMPLE, rot) == red, timeout=70)
            info.append(f"panel B back after being down: frame re-pushed {time.monotonic() - t0:.1f} s after it "
                        "started (includes the event stream's reconnect backoff)")
            a.stop()
            a.start()  # tt7d persists no frame the server sent (X-Persist: false), so only a re-push brings it back
            t0 = time.monotonic()
            wait_for("A's frame re-pushed", lambda: (a.frame_id() or "").startswith("a-")
                     and a.pixel(*SAMPLE, rot) == rgb565(COLOURS[clicks_a % len(COLOURS)]), timeout=15)
            info.append(f"panel A restart: frame re-pushed {time.monotonic() - t0:.1f} s after it answered")
            assert a.get("/api/v1/frame")["restored"] is False
            assert pages.clicks == {"a": clicks_a}, f"the page reloaded or got stray clicks: {pages.clicks}"

            steps.append("5b. PUT url moves B to the second page (magenta on B's fb) and saves it to screens.toml")
            second = pages.url("/second")
            status, body = http_json(admin_port, "PUT", "/api/screens/b/url", {"url": second})
            assert status == 200 and json.loads(body)["url"] == second, (status, body)
            wait_for("magenta on panel B", lambda: b.pixel(*SAMPLE, rot) == rgb565(MAGENTA), timeout=10)
            s_b = screen_status(admin_port, "b")
            assert s_b["last_update"] == "full", f"a navigation should be a full frame: {s_b}"
            assert b.get("/api/v1/frame")["updated_via"] == "full"
            assert a.pixel(*SAMPLE, rot) == rgb565(COLOURS[clicks_a % len(COLOURS)]), "panel A changed"
            with open(config) as f:
                saved = f.read()
            assert f'url = "{second}"   # the counter, for now' in saved, saved
            assert "# panel B, the one whose URL the admin API changes" in saved, saved
            assert f'url = "{pages.url("/counter?screen=a")}"' in saved, saved
            status, body = http_json(admin_port, "GET", "/api/screens")
            assert {s["name"]: s for s in json.loads(body)["screens"]}["b"]["url"] == second
            time.sleep(0.5)
            status, png = http_json(admin_port, "GET", "/api/screens/b/preview.png")
            assert status == 200 and png == b.request("GET", "/api/v1/frame/image")[2], "B's preview != panel"

            steps.append("5d. admin API refusals: unknown screen 404, bad URL 400 (file untouched), reload works")
            assert http_json(admin_port, "PUT", "/api/screens/zzz/url", {"url": second})[0] == 404
            status, body = http_json(admin_port, "PUT", "/api/screens/a/url", {"url": "javascript:alert(1)"})
            assert status == 400 and json.loads(body)["error"] == "invalid_url", body
            with open(config) as f:
                assert f.read() == saved, "a refused URL changed screens.toml"
            assert http_json(admin_port, "POST", "/api/screens/b/reload")[0] == 200
            status, body = http_json(admin_port, "GET", "/")
            assert status == 200 and b"tt7-server" in body

            steps.append("3b. SIGTERM: the server exits 0, Chrome goes, and both panels fall back to the clock")
            pids = server.descendants()
            assert pids, "no Chrome processes under the server"
            rc = server.stop()
            assert rc == 0, f"tt7-server exited {rc}"
            left = [p for p in pids if os.path.exists(f"/proc/{p}") and "chrome" in open(f"/proc/{p}/cmdline").read()]
            assert not left, f"Chrome processes left after shutdown: {left}"
            wait_for("both panels on the fallback clock", lambda: a.fallback() and b.fallback(),
                     timeout=FALLBACK_S + 5, step=0.25)

            log = server.log()
            steps.append("no panel token reaches the server log")
            for d in (a, b):
                assert d.token() not in log, "a panel token reached the server log"
        except Exception as e:  # noqa: BLE001 - say which step failed, with the logs
            print(f"FAIL test_server_e2e: {steps[-1] if steps else 'start'}: {type(e).__name__}: {e}",
                  file=sys.stderr)
            server.stop()
            for path in (server.log_path, a.log_path, b.log_path, c.log_path, dd.log_path):
                if os.path.exists(path):
                    with open(path, "rb") as f:
                        tail = f.read().decode("utf-8", "replace").splitlines()[-30:]
                    print(f"---- {path} (last 30 lines) ----", *tail, sep="\n", file=sys.stderr)
            return 1
        finally:
            server.stop()
            for d in (a, b, c, dd):
                if d.proc:
                    try:
                        os.kill(d.proc.pid, signal.SIGCONT)
                    except OSError:
                        pass
                d.stop()
            pages.stop()
        for s in steps:
            print(f"  ok   server: {s}")
        for i in info:
            print(f"  info server: {i}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
