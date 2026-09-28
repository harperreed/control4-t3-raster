#!/usr/bin/env python3
# ABOUTME: End-to-end test: runs the real host-built tt7d on a file-backed 800x1280 RGB565 framebuffer.
# ABOUTME: Pushes frames over real HTTP, checks the framebuffer against an independent Python rotation, and drives the control panel.
"""Usage: tt7d/test_e2e.py --daemon build/host/tt7d   (run by `make test-e2e`)

Nothing here is mocked: the daemon is the same source as the panel build,
compiled for the host, listening on a free loopback port, with a temporary
data dir and a writable copy of the sysfs fixture from the real panel (the
brightness, blank and wake tests read the backlight files it writes). The expected
framebuffer contents are computed here, in Python, from the logical RGBA
pixels, never by asking the daemon.
"""
import argparse
import hashlib
import http.client
import importlib.util
import json
import os
import random
import re
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FIXTURE = os.path.join(HERE, "test", "fixtures", "sysfs-tt7")
PROC_FIXTURE = os.path.join(HERE, "test", "fixtures", "proc-tt7")
CSP = ("default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; "
       "frame-ancestors 'none'")

# The panel's real framebuffer (hardware/discovery/*/fb-ioctl.txt).
NATIVE_W, NATIVE_H, STRIDE = 800, 1280, 1600
LOGICAL_W, LOGICAL_H = 1280, 800
FB_BYTES = STRIDE * NATIVE_H
TIMEOUT_MS = 1500

_spec = importlib.util.spec_from_file_location("make_test_frame", os.path.join(ROOT, "tools", "make-test-frame.py"))
make_test_frame = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(make_test_frame)
png_bytes = make_test_frame.png_bytes


def expected_fb(pixels, channels=4):
    """RGB565 little-endian bytes for the logical image rotated 90 degrees clockwise:
    logical (x, y) lands on native (LOGICAL_H - 1 - y, x)."""
    out = bytearray(FB_BYTES)
    for y in range(LOGICAL_H):
        nx2 = (LOGICAL_H - 1 - y) * 2
        row = y * LOGICAL_W * channels
        for x in range(LOGICAL_W):
            i = row + x * channels
            v = ((pixels[i] >> 3) << 11) | ((pixels[i + 1] >> 2) << 5) | (pixels[i + 2] >> 3)
            o = x * STRIDE + nx2
            out[o] = v & 0xFF
            out[o + 1] = v >> 8
    return bytes(out)


def random_image(seed, channels=4, w=LOGICAL_W, h=LOGICAL_H):
    return random.Random(seed).randbytes(w * h * channels)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Daemon:
    def __init__(self, binary, workdir, extra_args=()):
        self.binary = binary
        self.extra_args = list(extra_args)
        self.fb = os.path.join(workdir, "fb.raw")
        self.data = os.path.join(workdir, "data")
        self.log_path = os.path.join(workdir, "tt7d.log")
        self.sysfs = os.path.join(workdir, "sysfs")
        self.backlight = os.path.join(self.sysfs, "class", "backlight", "rk28_bl")
        self.reboot_marker = os.path.join(workdir, "rebooted")
        # Never the host's /dev/input: tests that want input put FIFOs here (test_input_e2e.py).
        self.input_dir = os.path.join(workdir, "input")
        os.makedirs(self.input_dir, exist_ok=True)
        shutil.copytree(FIXTURE, self.sysfs)
        self.port = free_port()
        self.proc = None
        self.rotation = "90"  # None: pass no --rotation, so tt7d's own default applies
        with open(self.fb, "wb") as f:
            f.write(bytes(FB_BYTES))

    def start(self):
        log = open(self.log_path, "ab")
        self.proc = subprocess.Popen(
            [self.binary, "--listen", f"127.0.0.1:{self.port}", "--fb-file", self.fb,
             "--fb-geometry", f"{NATIVE_W}x{NATIVE_H}x16", "--fb-stride", str(STRIDE), "--fb-format", "rgb565",
             *(["--rotation", self.rotation] if self.rotation is not None else []),
             "--data-dir", self.data, "--sysfs-root", self.sysfs,
             "--proc-root", PROC_FIXTURE, "--log-file", self.log_path, "--input-dir", self.input_dir,
             "--reboot-cmd", "touch " + shlex.quote(self.reboot_marker),
             # Never the host's own /dev/video0 (test_camera_e2e.py overrides this).
             "--camera-dev", os.path.join(os.path.dirname(self.data), "no-video0"),
             "--request-timeout-ms", str(TIMEOUT_MS)] + self.extra_args,
            stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise AssertionError(f"tt7d exited with {self.proc.returncode}")
            try:
                status, _, _ = self.request("GET", "/api/v1/info", timeout=1)
                if status == 200:
                    return
            except OSError:
                pass
            time.sleep(0.05)
        raise AssertionError("tt7d did not answer /api/v1/info within 15 s")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            self.proc.wait(timeout=10)
        self.proc = None

    def token(self):
        with open(os.path.join(self.data, "token")) as f:
            return f.read().strip()

    def request(self, method, path, body=None, headers=None, timeout=20):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        try:
            conn.request(method, path, body=body, headers=headers or {})
            resp = conn.getresponse()
            return resp.status, dict((k.lower(), v) for k, v in resp.getheaders()), resp.read()
        finally:
            conn.close()

    def put_frame(self, png, token=True, **extra):
        headers = {"Content-Type": "image/png"}
        if token is True:
            headers["Authorization"] = f"Bearer {self.token()}"
        elif token:
            headers["Authorization"] = token
        headers.update(extra)
        return self.request("PUT", "/api/v1/frame", body=png, headers=headers)

    def api(self, method, path, doc=None, token=True, body=None):
        """A control panel request: JSON body, Bearer token unless token is False (or a string to send)."""
        headers = {}
        if token:
            headers["Authorization"] = "Bearer " + (self.token() if token is True else token)
        if doc is not None:
            body = json.dumps(doc).encode()
            headers["Content-Type"] = "application/json"
        return self.request(method, path, body=body, headers=headers)

    def bl(self, attr):
        with open(os.path.join(self.backlight, attr)) as f:
            return f.read().strip()

    def fb_bytes(self):
        with open(self.fb, "rb") as f:
            return f.read()


def jbody(status, body, want_status):
    assert status == want_status, f"HTTP {status}, want {want_status}: {body[:300]!r}"
    return json.loads(body)


def check_error(result, want_status, code):
    status, headers, body = result
    doc = jbody(status, body, want_status)
    assert headers.get("content-type", "").startswith("application/json"), headers
    assert doc.get("error") == code, f"error {doc.get('error')!r}, want {code!r}: {doc}"
    return doc


# ---- tests ------------------------------------------------------------------

def test_info_and_empty_state(d):
    info = jbody(*d.request("GET", "/api/v1/info")[::2], 200)
    assert re.fullmatch(r"tt7-[0-9a-f]{6}", info["device_id"]), info["device_id"]
    with open(os.path.join(d.data, "device.json")) as f:
        assert json.load(f)["device_id"] == info["device_id"]
    assert info["model"] == "C4-TT7"
    disp = info["display"]
    assert (disp["width"], disp["height"], disp["rotation"]) == (1280, 800, 90), disp
    assert disp["frame_formats"] == ["image/png"]
    assert disp["frame_patch"] == {"content_type": "application/x-tt7-regions", "version": 1, "max_regions": 16}
    assert disp["native"] == {"width": 800, "height": 1280, "format": "rgb565", "stride": 1600,
                              "bits_per_pixel": 16}, disp["native"]
    caps = info["capabilities"]
    assert caps["touch"] == {"available": True, "device": "gslX680"}, caps["touch"]
    assert caps["ethernet"] == {"available": False, "interface": None}, caps["ethernet"]
    # The fixture has a video0 in sysfs, but the daemon's --camera-dev does not exist, and the camera is off.
    assert caps["camera"]["available"] is False and caps["camera"]["enabled"] is False, caps["camera"]
    assert caps["camera"]["video4linux_devices"] == ["video0"], caps["camera"]
    assert os.stat(os.path.join(d.data, "token")).st_mode & 0o777 == 0o600

    state = jbody(*d.request("GET", "/api/v1/state")[::2], 200)
    assert state["display"]["frame_id"] is None and state["display"]["frame_age_s"] is None, state["display"]
    assert state["display"]["brightness"] == {"value": 50, "unit": "percent", "available": True}
    assert state["power"]["battery_percent"]["estimate"] is True
    assert state["frames"] == {"accepted": 0, "region_updates": 0, "deduplicated": 0, "rejected": 0,
                               "last_error": None}, state["frames"]
    assert state["fallback"] == {"active": False, "reason": None, "timeout_s": 0, "since": None}, state["fallback"]
    assert state["clock"]["timezone"] == "CST6CDT,M3.2.0,M11.1.0" and state["clock"]["format"] == "24h", state["clock"]
    assert isinstance(state["uptime_s"], (int, float)) and re.fullmatch(r"\d{4}-\d\d-\d\dT.*Z", state["time"])

    check_error(d.request("GET", "/api/v1/frame"), 404, "no_frame")
    check_error(d.request("GET", "/api/v1/frame/image"), 404, "no_frame")
    assert d.fb_bytes() == bytes(FB_BYTES), "framebuffer touched before any frame"


def test_put_frame(d):
    pixels = random_image(1)
    png = png_bytes(LOGICAL_W, LOGICAL_H, pixels)
    status, _, body = d.put_frame(png, **{"X-Frame-ID": "e2e-1", "X-Frame-SHA256": hashlib.sha256(png).hexdigest()})
    doc = jbody(status, body, 200)
    assert doc["frame_id"] == "e2e-1" and doc["deduplicated"] is False and doc["persisted"] is False, doc
    assert doc["sha256"] == hashlib.sha256(png).hexdigest() and doc["width"] == 1280 and doc["height"] == 800
    assert d.fb_bytes() == expected_fb(pixels), "framebuffer != independently rotated RGB565"

    meta = jbody(*d.request("GET", "/api/v1/frame")[::2], 200)
    assert meta["frame_id"] == "e2e-1" and meta["content_type"] == "image/png", meta
    status, headers, image = d.request("GET", "/api/v1/frame/image")
    assert status == 200 and headers["content-type"] == "image/png" and image == png, "frame/image != original PNG"
    return png


def test_rgb_png_via_push_frame_tool(d, workdir):
    pixels = random_image(2, channels=3)
    path = os.path.join(workdir, "rgb.png")
    with open(path, "wb") as f:
        f.write(png_bytes(LOGICAL_W, LOGICAL_H, pixels, channels=3))
    token_file = os.path.join(workdir, "token")
    with open(token_file, "w") as f:
        f.write(d.token() + "\n")
    out = subprocess.run([os.path.join(ROOT, "tools", "push-frame.sh"), f"127.0.0.1:{d.port}", path, "--id", "tool-1"],
                         env={**os.environ, "TT7_TOKEN_FILE": token_file}, capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, f"push-frame.sh failed: {out.stdout} {out.stderr}"
    assert json.loads(out.stdout)["frame_id"] == "tool-1", out.stdout
    assert d.fb_bytes() == expected_fb(pixels, channels=3), "RGB (colour type 2) PNG drawn wrong"

    # The tool reports HTTP errors with a non-zero exit and still prints the JSON.
    with open(token_file, "w") as f:
        f.write("wrong-token\n")
    out = subprocess.run([os.path.join(ROOT, "tools", "push-frame.sh"), f"127.0.0.1:{d.port}", path],
                         env={**os.environ, "TT7_TOKEN_FILE": token_file}, capture_output=True, text=True, timeout=60)
    assert out.returncode == 1 and json.loads(out.stdout)["error"] == "unauthorized", out


def unfiltered_png_pixels(png):
    """Pixels of a PNG made by png_bytes (8-bit, filter 0 on every row): enough to check the tool's output."""
    pos, idat, width = 8, b"", 0
    while pos < len(png):
        length, kind = struct.unpack(">I4s", png[pos:pos + 8])
        data = png[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, _, _, color_type = struct.unpack(">IIBB", data[:10])
            channels = {2: 3, 6: 4}[color_type]
        elif kind == b"IDAT":
            idat += data
        pos += 12 + length
    raw, stride = zlib.decompress(idat), width * channels
    assert all(raw[y * (stride + 1)] == 0 for y in range(len(raw) // (stride + 1)))
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(len(raw) // (stride + 1))), channels


def test_make_test_frame_tool(d, workdir):
    path = os.path.join(workdir, "test-frame.png")
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "make-test-frame.py"), path, "--label", "e2e"],
                         capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, out.stderr
    with open(path, "rb") as f:
        png = f.read()
    jbody(*d.put_frame(png, **{"X-Frame-ID": "test-frame"})[::2], 200)
    pixels, channels = unfiltered_png_pixels(png)
    assert d.fb_bytes() == expected_fb(pixels, channels), "test frame drawn wrong"
    # Logical (2, 2), inside the red TOP-LEFT block (inside the 1-px border),
    # lands at native (797, 2): the top-right of the portrait framebuffer.
    o = 2 * STRIDE + (NATIVE_W - 1 - 2) * 2
    assert d.fb_bytes()[o:o + 2] == b"\x00\xf8", d.fb_bytes()[o:o + 2].hex()


def test_rejections_leave_display_alone(d):
    before = d.fb_bytes()
    small = png_bytes(1024, 768, random_image(3, w=1024, h=768))
    doc = check_error(d.put_frame(small), 422, "invalid_dimensions")
    assert doc["expected"] == [1280, 800] and doc["received"] == [1024, 768], doc

    good = png_bytes(LOGICAL_W, LOGICAL_H, random_image(4))
    check_error(d.put_frame(good, token=False), 401, "unauthorized")
    check_error(d.put_frame(good, token="Bearer nope"), 401, "unauthorized")
    check_error(d.put_frame(good, token="Basic " + d.token()), 401, "unauthorized")
    check_error(d.put_frame(os.urandom(5000)), 422, "invalid_image")
    check_error(d.put_frame(good[: len(good) // 2]), 422, "invalid_image")
    check_error(d.put_frame(good, **{"Content-Type": "image/jpeg"}), 415, "unsupported_media_type")
    doc = check_error(d.put_frame(good, **{"X-Frame-SHA256": "0" * 64}), 400, "sha256_mismatch")
    assert doc["computed"] == hashlib.sha256(good).hexdigest(), doc
    check_error(d.put_frame(good, **{"X-Frame-SHA256": "xyz"}), 400, "invalid_sha256")
    check_error(d.put_frame(good, **{"X-Frame-ID": "has space"}), 400, "invalid_frame_id")
    check_error(d.put_frame(good, **{"X-Persist": "maybe"}), 400, "invalid_persist")
    assert d.fb_bytes() == before, "a rejected frame changed the framebuffer"
    assert not os.path.exists(os.path.join(d.data, "last-frame.png")), "a rejected frame was persisted"

    # 11 rejections here plus the wrong-token push in the tool test before this one.
    state = jbody(*d.request("GET", "/api/v1/state")[::2], 200)
    assert state["frames"]["rejected"] == 12 and state["frames"]["last_error"] == "invalid_persist", state["frames"]


def test_oversized_is_refused_before_the_body(d):
    s = socket.create_connection(("127.0.0.1", d.port), timeout=5)
    s.sendall((f"PUT /api/v1/frame HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer {d.token()}\r\n"
               "Content-Type: image/png\r\nContent-Length: 9000000\r\nExpect: 100-continue\r\n\r\n").encode())
    reply = s.recv(65536).decode("latin-1")
    s.close()
    assert reply.startswith("HTTP/1.1 413 "), reply[:200]
    assert json.loads(reply.split("\r\n\r\n", 1)[1])["error"] == "payload_too_large", reply


def test_expect_continue_upload(d):
    """curl sends Expect: 100-continue for big bodies; tt7d must answer 100 and then take the body."""
    pixels = random_image(5)
    png = png_bytes(LOGICAL_W, LOGICAL_H, pixels)
    s = socket.create_connection(("127.0.0.1", d.port), timeout=5)
    s.sendall((f"PUT /api/v1/frame HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer {d.token()}\r\n"
               f"Content-Type: image/png\r\nContent-Length: {len(png)}\r\nExpect: 100-continue\r\n\r\n").encode())
    interim = s.recv(64).decode("latin-1")
    assert interim == "HTTP/1.1 100 Continue\r\n\r\n", interim
    s.sendall(png)
    reply = b""
    while chunk := s.recv(65536):
        reply += chunk
    s.close()
    assert reply.startswith(b"HTTP/1.1 200 "), reply[:200]
    assert d.fb_bytes() == expected_fb(pixels)
    return png, pixels


def test_dedup(d, png):
    first = jbody(*d.request("GET", "/api/v1/frame")[::2], 200)
    # Scribble on the framebuffer: a skipped redraw leaves the scribble there.
    with open(d.fb, "r+b") as f:
        f.write(b"\xAA" * 64)
    time.sleep(0.01)
    doc = jbody(*d.put_frame(png, **{"X-Frame-ID": "e2e-dup"})[::2], 200)
    assert doc["deduplicated"] is True and doc["frame_id"] == "e2e-dup", doc
    assert doc["received_at"] > first["received_at"], (doc["received_at"], first["received_at"])
    assert d.fb_bytes()[:64] == b"\xAA" * 64, "a duplicate frame was redrawn"
    state = jbody(*d.request("GET", "/api/v1/state")[::2], 200)
    assert state["frames"]["deduplicated"] == 1 and state["display"]["frame_id"] == "e2e-dup", state
    assert state["display"]["frame_age_s"] < 5


def test_persist_and_restart(d):
    pixels = random_image(6)
    png = png_bytes(LOGICAL_W, LOGICAL_H, pixels)
    doc = jbody(*d.put_frame(png, **{"X-Frame-ID": "keep-me", "X-Persist": "true"})[::2], 200)
    assert doc["persisted"] is True, doc
    with open(os.path.join(d.data, "last-frame.png"), "rb") as f:
        assert f.read() == png, "last-frame.png != the PNG sent"
    assert not os.path.exists(os.path.join(d.data, "last-frame.png.tmp"))

    # A later frame without X-Persist does not overwrite the persisted one.
    other = png_bytes(LOGICAL_W, LOGICAL_H, random_image(7))
    assert jbody(*d.put_frame(other)[::2], 200)["persisted"] is False
    with open(os.path.join(d.data, "last-frame.png"), "rb") as f:
        assert f.read() == png

    d.stop()
    with open(d.fb, "wb") as f:
        f.write(bytes(FB_BYTES))
    d.start()
    assert d.fb_bytes() == expected_fb(pixels), "restarted daemon did not show the persisted frame"
    meta = jbody(*d.request("GET", "/api/v1/frame")[::2], 200)
    assert meta["frame_id"] == "keep-me" and meta["persisted"] is True and meta["restored"] is True, meta
    status, _, image = d.request("GET", "/api/v1/frame/image")
    assert status == 200 and image == png


# ---- PATCH /api/v1/frame: regions (SPEC §10.1) --------------------------------

REGIONS_TYPE = "application/x-tt7-regions"
GOLDEN = os.path.join(HERE, "test", "fixtures", "regions-v1.bin")


def regions_body(regions):
    """The container, written here from SPEC §10.1: "TT7R", version 1, reserved 0, count (u16),
    then x, y, w, h (u16) and png_len (u32) per region, then the PNGs; all big-endian."""
    head = b"TT7R" + bytes([1, 0]) + struct.pack(">H", len(regions))
    recs = b"".join(struct.pack(">HHHHI", x, y, w, h, len(png)) for x, y, w, h, png in regions)
    return head + recs + b"".join(png for *_, png in regions)


def parse_regions_body(body):
    n = struct.unpack(">H", body[6:8])[0]
    off, out = 8 + 12 * n, []
    for i in range(n):
        x, y, w, h, size = struct.unpack(">HHHHI", body[8 + 12 * i:20 + 12 * i])
        out.append((x, y, w, h, body[off:off + size]))
        off += size
    assert off == len(body)
    return out


def region_sha(base_sha, body):
    """The patched frame's SHA-256: sha256(lowercase hex of the base frame's sha256 + the body)."""
    return hashlib.sha256(base_sha.lower().encode() + body).hexdigest()


def compose(pixels, regions_pixels):
    """Apply (x, y, w, h, rgba) regions in order to logical RGBA pixels, here in Python."""
    out = bytearray(pixels)
    for x, y, w, h, rgba in regions_pixels:
        for row in range(h):
            o = ((y + row) * LOGICAL_W + x) * 4
            out[o:o + w * 4] = rgba[row * w * 4:(row + 1) * w * 4]
    return bytes(out)


def patch_frame(d, body, base, token=True, **extra):
    headers = {"Content-Type": REGIONS_TYPE, "X-Base-Frame-ID": base}
    if token:
        headers["Authorization"] = f"Bearer {d.token()}"
    headers.update(extra)
    return d.request("PATCH", "/api/v1/frame", body=body, headers=headers)


def test_patch_container_matches_golden():
    """This test's encoder rebuilds the shared golden vector byte for byte (tt7d and the Go server check it too)."""
    with open(GOLDEN, "rb") as f:
        golden = f.read()
    assert regions_body(parse_regions_body(golden)) == golden, "the e2e encoder drifted from the golden vector"


def test_patch_regions(d):
    """A PUT, then PATCHes onto it: pixels, metadata, /frame/image, every refusal leaving the screen alone."""
    base_pixels = random_image(20)
    base_png = png_bytes(LOGICAL_W, LOGICAL_H, base_pixels)
    jbody(*d.put_frame(base_png, **{"X-Frame-ID": "base-1"})[::2], 200)
    accepted = jbody(*d.request("GET", "/api/v1/state")[::2], 200)["frames"]

    # Two regions: a 300x200 block and a corner strip that overlaps nothing; RGBA and RGB PNGs.
    a_rgba, b_rgb = random_image(21, w=300, h=200), random_image(22, channels=3, w=64, h=800)
    b_rgba = b"".join(b_rgb[i:i + 3] + b"\xff" for i in range(0, len(b_rgb), 3))
    body = regions_body([(100, 50, 300, 200, png_bytes(300, 200, a_rgba)),
                         (1216, 0, 64, 800, png_bytes(64, 800, b_rgb, channels=3))])
    want = compose(base_pixels, [(100, 50, 300, 200, a_rgba), (1216, 0, 64, 800, b_rgba)])
    sha = region_sha(hashlib.sha256(base_png).hexdigest(), body)
    doc = jbody(*patch_frame(d, body, "base-1", **{"X-Frame-ID": "patch-1", "X-Frame-SHA256": sha})[::2], 200)
    assert doc["frame_id"] == "patch-1" and doc["sha256"] == sha and doc["updated_via"] == "regions", doc
    assert doc["regions"] == 2 and doc["bytes"] == len(body) and doc["deduplicated"] is False, doc
    assert doc["persisted"] is False and doc["content_type"] == "image/png", doc
    assert d.fb_bytes() == expected_fb(want), "framebuffer != base + regions, independently rotated"
    state = jbody(*d.request("GET", "/api/v1/state")[::2], 200)
    assert state["display"]["frame_id"] == "patch-1", state["display"]  # what events carry as frame_id
    assert state["frames"]["accepted"] == accepted["accepted"] + 1, state["frames"]
    assert state["frames"]["region_updates"] == accepted["region_updates"] + 1, state["frames"]

    # /frame/image is the composed frame, encoded on request (RGB, filter 0 on every row).
    status, headers, image = d.request("GET", "/api/v1/frame/image")
    assert status == 200 and headers["content-type"] == "image/png"
    pixels, channels = unfiltered_png_pixels(image)
    assert channels == 3 and pixels == b"".join(want[i:i + 3] for i in range(0, len(want), 4)), "image != composed"
    assert d.request("GET", "/api/v1/frame/image")[2] == image, "the encoded image was not kept"

    # Refusals: each leaves the framebuffer, the id and the counters of accepted frames alone.
    fb, small = d.fb_bytes(), png_bytes(8, 8, random_image(23, w=8, h=8))
    doc = check_error(patch_frame(d, regions_body([(0, 0, 8, 8, small)]), "base-1"), 409, "base_mismatch")
    assert doc["current_frame_id"] == "patch-1", doc
    check_error(patch_frame(d, regions_body([(0, 0, 8, 8, small)]), "patch-1", token=False), 401, "unauthorized")
    check_error(patch_frame(d, regions_body([(0, 0, 8, 8, small)]), "patch-1", **{"Content-Type": "image/png"}),
                415, "unsupported_media_type")
    check_error(d.request("PATCH", "/api/v1/frame", body=b"x", headers={
        "Authorization": f"Bearer {d.token()}", "Content-Type": REGIONS_TYPE}), 400, "missing_base_frame_id")
    check_error(patch_frame(d, regions_body([(0, 0, 8, 8, small)]), "patch-1", **{"X-Persist": "true"}),
                400, "persist_not_supported")
    check_error(patch_frame(d, regions_body([(0, 0, 8, 8, small)]), "patch-1", **{"X-Frame-SHA256": "0" * 64}),
                400, "sha256_mismatch")
    doc = check_error(patch_frame(d, regions_body([(1275, 0, 8, 8, small)]), "patch-1"), 422, "region_out_of_bounds")
    assert doc["region"] == 0, doc
    check_error(patch_frame(d, regions_body([(0, 0, 8, 8, small)] * 17), "patch-1"), 400, "too_many_regions")
    check_error(patch_frame(d, regions_body([(0, 0, 8, 8, small)])[:-1], "patch-1"), 400, "invalid_regions")
    # Atomic: region 0 is good, region 1 is 8x8 but claims 8x9. Neither may reach the screen.
    doc = check_error(patch_frame(d, regions_body([(0, 0, 8, 8, small), (20, 20, 8, 9, small)]), "patch-1"),
                      422, "region_size_mismatch")
    assert doc["region"] == 1 and doc["expected"] == [8, 9] and doc["received"] == [8, 8], doc
    doc = check_error(patch_frame(d, regions_body([(0, 0, 8, 8, small), (20, 20, 2, 2, b"junk")]), "patch-1"),
                      422, "invalid_image")
    assert doc["region"] == 1, doc
    assert d.fb_bytes() == fb, "a refused PATCH changed the framebuffer"
    meta = jbody(*d.request("GET", "/api/v1/frame")[::2], 200)
    assert meta["frame_id"] == "patch-1" and meta["sha256"] == sha, meta
    frames = jbody(*d.request("GET", "/api/v1/state")[::2], 200)["frames"]
    assert frames["rejected"] == state["frames"]["rejected"] + 11, frames
    assert frames["last_error"] == "invalid_image", frames

    # An empty batch changes no pixels: a new id, the same hash, deduplicated.
    doc = jbody(*patch_frame(d, regions_body([]), "patch-1", **{"X-Frame-ID": "patch-2"})[::2], 200)
    assert doc["frame_id"] == "patch-2" and doc["sha256"] == sha and doc["deduplicated"] is True, doc
    assert d.fb_bytes() == fb

    # A PUT of the base PNG again is not a duplicate of the patched frame: it is drawn in full.
    doc = jbody(*d.put_frame(base_png, **{"X-Frame-ID": "base-2"})[::2], 200)
    assert doc["deduplicated"] is False and doc["updated_via"] == "full" and doc["regions"] is None, doc
    assert d.fb_bytes() == expected_fb(base_pixels)
    assert d.request("GET", "/api/v1/frame/image")[2] == base_png, "a PUT frame's image is the PNG as received"


def test_patch_restored_frame(d):
    """A frame restored from last-frame.png keeps its id, so a PATCH can build on it; then it is no longer persisted."""
    meta = jbody(*d.request("GET", "/api/v1/frame")[::2], 200)
    assert meta["restored"] is True and meta["frame_id"] == "keep-me", meta
    status, _, image = d.request("GET", "/api/v1/frame/image")
    pixels, channels = unfiltered_png_pixels(image)
    rgba = random_image(24, w=40, h=30)
    body = regions_body([(600, 400, 40, 30, png_bytes(40, 30, rgba))])
    doc = jbody(*patch_frame(d, body, "keep-me", **{"X-Frame-ID": "on-restored"})[::2], 200)
    assert doc["restored"] is False and doc["persisted"] is False and doc["sha256"] == region_sha(meta["sha256"], body)
    assert d.fb_bytes() == expected_fb(compose(pixels, [(600, 400, 40, 30, rgba)]), channels)


def test_protocol_errors(d):
    def raw(data):
        s = socket.create_connection(("127.0.0.1", d.port), timeout=5)
        s.sendall(data)
        reply = b""
        while chunk := s.recv(65536):
            reply += chunk
        s.close()
        head, _, body = reply.decode("latin-1").partition("\r\n\r\n")
        return int(head.split()[1]), json.loads(body)

    status, doc = raw(b"BLAH\r\n\r\n")
    assert (status, doc["error"]) == (400, "bad_request"), doc
    status, doc = raw(b"PUT /api/v1/frame HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n")
    assert (status, doc["error"]) == (411, "length_required"), doc
    status, doc = raw(b"GET /api/v1/info HTTP/1.1\r\nX: " + b"a" * 20000 + b"\r\n\r\n")
    assert (status, doc["error"]) == (431, "headers_too_large"), doc
    status, doc = raw(b"GET /api/v1/info HTTP/2.0\r\n\r\n")
    assert (status, doc["error"]) == (505, "http_version_not_supported"), doc
    check_error(d.request("GET", "/nope"), 404, "not_found")
    status, headers, body = d.request("POST", "/api/v1/frame", body=b"")
    assert jbody(status, body, 405)["error"] == "method_not_allowed" and headers["allow"] == "GET, PUT, PATCH", headers


def test_slow_client_does_not_block(d):
    slow = socket.create_connection(("127.0.0.1", d.port), timeout=10)
    slow.sendall(b"GET /api/v1/info HTTP/1.1\r\nHost: slow\r\n")  # never finishes its head
    stalled_body = socket.create_connection(("127.0.0.1", d.port), timeout=10)
    stalled_body.sendall(f"PUT /api/v1/frame HTTP/1.1\r\nAuthorization: Bearer {d.token()}\r\n"
                         "Content-Type: image/png\r\nContent-Length: 1000\r\n\r\n0123456789".encode())
    t0 = time.monotonic()
    status, _, _ = d.request("GET", "/api/v1/state", timeout=5)
    took = time.monotonic() - t0
    assert status == 200 and took < 0.5, f"second client took {took:.2f} s"

    for s in (slow, stalled_body):
        t0 = time.monotonic()
        reply = s.recv(4096).decode("latin-1")
        waited = time.monotonic() - t0
        assert reply.startswith("HTTP/1.1 408 "), reply[:100]
        assert json.loads(reply.split("\r\n\r\n", 1)[1])["error"] == "request_timeout"
        assert waited < TIMEOUT_MS / 1000 + 1.0, f"timeout took {waited:.2f} s"
        s.close()


# ---- control panel ------------------------------------------------------------

def test_panel_assets(d):
    status, headers, body = d.request("GET", "/")
    assert status == 200 and headers["content-type"] == "text/html; charset=utf-8", (status, headers)
    assert headers["content-security-policy"] == CSP, headers.get("content-security-policy")
    assert headers["x-content-type-options"] == "nosniff" and headers["cache-control"] == "no-store", headers
    assert body.startswith(b"<!doctype html>") and b'<script src="/panel.js"' in body, body[:200]
    for path, ctype, start in (("/panel.css", "text/css; charset=utf-8", b"/* ABOUTME:"),
                               ("/panel.js", "text/javascript; charset=utf-8", b"// ABOUTME:")):
        status, headers, body = d.request("GET", path)
        assert status == 200 and headers["content-type"] == ctype and body.startswith(start), (path, status, headers)
        assert headers["content-security-policy"] == CSP
    check_error(d.request("GET", "/nope.js"), 404, "not_found")
    check_error(d.request("GET", "/test-pattern.png"), 404, "not_found")
    status, headers, body = d.request("POST", "/", body=b"")
    assert jbody(status, body, 405)["error"] == "method_not_allowed" and headers["allow"] == "GET", headers
    # API replies carry the same protections, and nobody gets CORS headers.
    status, headers, _ = d.request("GET", "/api/v1/state", headers={"Origin": "http://evil.example"})
    assert status == 200 and headers["cache-control"] == "no-store" and headers["x-content-type-options"] == "nosniff"
    assert not any(k.startswith("access-control-") for k in headers), headers


def test_hardware(d):
    hw = jbody(*d.request("GET", "/api/v1/hardware")[::2], 200)
    assert hw["display"] == {"device": d.fb, "native": {"width": 800, "height": 1280, "format": "rgb565",
                                                         "stride": 1600, "bits_per_pixel": 16},
                             "rotation": 90, "logical": {"width": 1280, "height": 800},
                             "blank_method": "backlight"}, hw["display"]
    inputs = {i["name"]: i for i in hw["input"]}
    assert inputs["rk29-keypad"]["device"] == "/dev/input/event0" and inputs["rk29-keypad"]["role"] == "buttons"
    assert inputs["rk29-keypad"]["keys"] == ["volume_down", "volume_up", "power", "wakeup"], inputs
    assert inputs["gslX680"]["device"] == "/dev/input/event1" and inputs["gslX680"]["role"] == "touchscreen"
    for attr, value in hw["backlight"]["attributes"].items():
        assert value == d.bl(attr), (attr, value)
    supplies = {p["name"]: p["attributes"] for p in hw["power_supplies"]}
    assert supplies["battery"]["capacity"] == "82" and supplies["ac"]["type"] == "Mains", supplies
    names = [n["name"] for n in hw["network_interfaces"]]
    assert names == [n for n in sorted(os.listdir(os.path.join(d.sysfs, "class", "net"))) if n != "lo"], names
    assert hw["audio"]["cards"] == [{"index": 0, "id": "RK29RT3261", "name": "RK29_RT3261 - RK29_RT3261"}]
    assert "pcmC0D0p" in hw["audio"]["devices"], hw["audio"]
    assert hw["video_devices"] == [{"device": "/dev/video0", "name": None}], hw["video_devices"]
    assert hw["thermal_zones"] == []


def test_system(d):
    sysd = jbody(*d.request("GET", "/api/v1/system")[::2], 200)
    assert sysd["firmware_version"] == "0.1.0" and sysd["build"], sysd
    assert sysd["kernel"]["release"] == os.uname().release, sysd["kernel"]
    assert sysd["memory"]["total"] == {"value": 1048576, "unit": "kibibyte"}, sysd["memory"]
    assert sysd["memory"]["available"] == {"value": None, "unit": "kibibyte"}, "3.0's meminfo has no MemAvailable"
    st = os.statvfs(d.data)
    storage = sysd["storage"][0]
    assert storage["path"] == d.data and storage["total"] == {"value": st.f_blocks * st.f_frsize, "unit": "byte"}
    assert sysd["time"]["plausible"] is (time.gmtime().tm_year >= 2024), sysd["time"]
    assert sysd["time"]["timezone"] == "UTC" and isinstance(sysd["uptime_s"], int)
    assert sysd["time"]["synchronized"] is False, "no NTP sync marker in this test"


def test_brightness(d):
    path = "/api/v1/display/brightness"
    before = d.bl("brightness")
    check_error(d.api("PUT", path, {"value": 10}, token=False), 401, "unauthorized")
    check_error(d.api("PUT", path, {"value": 10}, token="wrong"), 401, "unauthorized")
    assert d.bl("brightness") == before, "an unauthenticated PUT changed the backlight"

    doc = jbody(*d.api("PUT", path, {"value": 200})[::2], 200)
    assert d.bl("brightness") == "200" and doc["brightness_raw"] == 200 and doc["on"] is True, doc
    doc = jbody(*d.api("PUT", path, {"value": 50, "unit": "percent"})[::2], 200)
    assert d.bl("brightness") == "128" and doc["brightness"] == {"value": 50, "unit": "percent", "available": True}
    state = jbody(*d.request("GET", "/api/v1/state")[::2], 200)
    assert state["display"]["brightness"]["value"] == 50

    doc = check_error(d.api("PUT", path, {"value": 256}), 400, "brightness_out_of_range")
    assert doc["max"] == 255 and doc["unit"] == "raw", doc
    doc = check_error(d.api("PUT", path, {"value": 101, "unit": "percent"}), 400, "brightness_out_of_range")
    assert doc["max"] == 100, doc
    check_error(d.api("PUT", path, {"value": -1}), 400, "brightness_out_of_range")
    check_error(d.api("PUT", path, body=b'{"value": "high"}'), 400, "invalid_brightness")
    check_error(d.api("PUT", path, body=b""), 400, "invalid_brightness")
    assert d.bl("brightness") == "128", "a refused PUT changed the backlight"
    status, headers, body = d.api("GET", path)
    assert jbody(status, body, 405)["error"] == "method_not_allowed" and headers["allow"] == "PUT"


def test_blank_and_wake(d):
    jbody(*d.api("PUT", "/api/v1/display/brightness", {"value": 180})[::2], 200)
    check_error(d.api("POST", "/api/v1/display/blank", token=False), 401, "unauthorized")
    assert d.bl("brightness") == "180"

    doc = jbody(*d.api("POST", "/api/v1/display/blank")[::2], 200)
    assert d.bl("brightness") == "0" and doc["on"] is False and doc["wake_brightness_raw"] == 180, doc
    doc = jbody(*d.api("POST", "/api/v1/display/blank")[::2], 200)  # idempotent: keeps the level to wake to
    assert d.bl("brightness") == "0" and doc["wake_brightness_raw"] == 180, doc
    state = jbody(*d.request("GET", "/api/v1/state")[::2], 200)
    assert state["display"]["on"] is False, state["display"]

    check_error(d.api("POST", "/api/v1/display/wake", token=False), 401, "unauthorized")
    assert d.bl("brightness") == "0"
    doc = jbody(*d.api("POST", "/api/v1/display/wake")[::2], 200)
    assert d.bl("brightness") == "180" and doc["on"] is True, doc
    jbody(*d.api("POST", "/api/v1/display/wake")[::2], 200)  # already awake: no change
    assert d.bl("brightness") == "180"

    # Something else powered the backlight down: wake turns bl_power back on.
    with open(os.path.join(d.backlight, "bl_power"), "w") as f:
        f.write("4\n")
    doc = jbody(*d.api("POST", "/api/v1/display/wake")[::2], 200)
    assert d.bl("bl_power") == "0" and d.bl("brightness") == "180" and doc["on"] is True, doc


def test_test_pattern(d):
    before = d.fb_bytes()
    accepted = jbody(*d.request("GET", "/api/v1/state")[::2], 200)["frames"]["accepted"]
    check_error(d.api("POST", "/api/v1/display/test-pattern", token=False), 401, "unauthorized")
    assert d.fb_bytes() == before, "an unauthenticated test pattern reached the screen"

    doc = jbody(*d.api("POST", "/api/v1/display/test-pattern")[::2], 200)
    assert re.fullmatch(r"test-pattern-[0-9a-f]{16}", doc["frame_id"]) and doc["persisted"] is False, doc
    fb = d.fb_bytes()
    assert fb != before, "the test pattern did not change the framebuffer"
    meta = jbody(*d.request("GET", "/api/v1/frame")[::2], 200)
    assert meta["frame_id"] == doc["frame_id"], meta
    status, headers, image = d.request("GET", "/api/v1/frame/image")
    assert status == 200 and headers["content-type"] == "image/png" and image.startswith(b"\x89PNG\r\n\x1a\n")
    assert hashlib.sha256(image).hexdigest() == meta["sha256"]
    pixels, channels = unfiltered_png_pixels(image)
    assert fb == expected_fb(pixels, channels), "framebuffer != the pattern PNG, independently rotated"
    o = 2 * STRIDE + (NATIVE_W - 1 - 2) * 2  # logical TOP-LEFT red block -> native top-right
    assert fb[o:o + 2] == b"\x00\xf8", fb[o:o + 2].hex()
    state = jbody(*d.request("GET", "/api/v1/state")[::2], 200)
    assert state["display"]["frame_id"] == doc["frame_id"] and state["frames"]["accepted"] == accepted + 1


def test_logs(d):
    check_error(d.request("GET", "/api/v1/logs"), 401, "unauthorized")
    check_error(d.api("GET", "/api/v1/logs?lines=0"), 400, "invalid_lines")
    check_error(d.api("GET", "/api/v1/logs?lines=abc"), 400, "invalid_lines")
    time.sleep(0.3)  # tt7d logs a failed request once its connection closes; let the 400s above land
    doc = jbody(*d.api("GET", "/api/v1/logs?lines=3")[::2], 200)
    assert doc["lines_requested"] == 3 and doc["tt7d"]["path"] == d.log_path and doc["tt7d"]["available"] is True
    with open(d.log_path, "rb") as f:  # a 200 is not logged, so the file is as it was for the request
        want = f.read().decode("ascii", "replace").splitlines()[-3:]
    assert doc["tt7d"]["lines"] == want and "invalid_lines" in want[-1], (doc["tt7d"]["lines"], want)
    k = doc["kernel"]
    assert isinstance(k["available"], bool) and isinstance(k["lines"], list), k
    assert k["available"] or (k["error"] and k["lines"] == []), k


def test_reboot(d):
    check_error(d.api("POST", "/api/v1/system/reboot", token=False), 401, "unauthorized")
    time.sleep(1.5)
    assert not os.path.exists(d.reboot_marker), "an unauthenticated reboot ran the command"
    status, _, body = d.api("POST", "/api/v1/system/reboot")
    doc = jbody(status, body, 202)
    assert doc == {"rebooting": True, "delay": {"value": 1, "unit": "second"}}, doc
    deadline = time.monotonic() + 10
    while not os.path.exists(d.reboot_marker) and time.monotonic() < deadline:
        time.sleep(0.05)
    assert os.path.exists(d.reboot_marker), "the reboot command did not run"
    assert jbody(*d.request("GET", "/api/v1/info")[::2], 200)["model"] == "C4-TT7", "the daemon kept serving"


def test_token_never_logged(d):
    with open(d.log_path, "rb") as f:
        assert d.token().encode() not in f.read(), "the bearer token appeared in the log"


def test_default_rotation(binary, workdir):
    """With no --rotation flag, tt7d uses 270: measured upright on the docked TT7 (2026-09-28)."""
    rd = os.path.join(workdir, "default-rotation")
    os.makedirs(rd)
    d = Daemon(binary, rd, ["--fallback-timeout", "0", "--ntp-marker", os.path.join(rd, "ntp-synced")])
    d.rotation = None
    try:
        d.start()
        status, _, body = d.request("GET", "/api/v1/info")
        assert status == 200, status
        disp = json.loads(body)["display"]
        assert (disp["width"], disp["height"], disp["rotation"]) == (1280, 800, 270), disp
    finally:
        d.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--daemon", required=True, help="host-built tt7d binary")
    args = ap.parse_args()
    binary = os.path.abspath(args.daemon)

    with tempfile.TemporaryDirectory(prefix="tt7d-e2e-") as workdir:
        # The fallback clock off: these steps check an untouched framebuffer
        # before the first frame. tt7d/test_fallback_e2e.py covers the clock.
        d = Daemon(binary, workdir, ["--fallback-timeout", "0", "--ntp-marker", os.path.join(workdir, "ntp-synced")])
        steps = []
        try:
            d.start()
            steps.append("info/state before any frame")
            test_info_and_empty_state(d)
            steps.append("PUT RGBA frame, fb bytes, /frame, /frame/image")
            test_put_frame(d)
            steps.append("RGB PNG through tools/push-frame.sh")
            test_rgb_png_via_push_frame_tool(d, workdir)
            steps.append("tools/make-test-frame.py output")
            test_make_test_frame_tool(d, workdir)
            steps.append("rejections leave the display alone")
            test_rejections_leave_display_alone(d)
            steps.append("oversized refused before the body")
            test_oversized_is_refused_before_the_body(d)
            steps.append("Expect: 100-continue upload")
            png, _ = test_expect_continue_upload(d)
            steps.append("dedup skips the redraw")
            test_dedup(d, png)
            steps.append("protocol errors")
            test_protocol_errors(d)
            steps.append("slow clients time out without blocking others")
            test_slow_client_does_not_block(d)
            steps.append("persist, restart, restore")
            test_persist_and_restart(d)
            steps.append("PATCH onto the restored frame")
            test_patch_restored_frame(d)
            steps.append("PATCH container encoder == the shared golden vector")
            test_patch_container_matches_golden()
            steps.append("PATCH regions: pixels, metadata, /frame/image, refusals are atomic")
            test_patch_regions(d)
            steps.append("panel: HTML/CSS/JS, CSP and security headers")
            test_panel_assets(d)
            steps.append("panel: /hardware matches the fixtures")
            test_hardware(d)
            steps.append("panel: /system")
            test_system(d)
            steps.append("panel: brightness auth, write, range")
            test_brightness(d)
            steps.append("panel: blank and wake")
            test_blank_and_wake(d)
            steps.append("panel: test pattern through the frame path")
            test_test_pattern(d)
            steps.append("panel: logs")
            test_logs(d)
            steps.append("panel: reboot runs the configured command")
            test_reboot(d)
            steps.append("token never in the log")
            test_token_never_logged(d)
        except Exception as e:  # noqa: BLE001 - report which step failed, with the daemon log
            print(f"FAIL test_e2e: {steps[-1] if steps else 'start'}: {type(e).__name__}: {e}", file=sys.stderr)
            d.stop()
            with open(d.log_path, "rb") as f:
                tail = f.read().decode("utf-8", "replace").splitlines()[-30:]
            print("---- tt7d log (last 30 lines) ----", *tail, sep="\n", file=sys.stderr)
            return 1
        finally:
            d.stop()
        with open(d.log_path, "rb") as f:
            log = f.read().decode("utf-8", "replace")
        bad = [line for line in log.splitlines() if re.search(r"AddressSanitizer|runtime error|LeakSanitizer", line)]
        if bad:
            print("FAIL test_e2e: sanitizer findings in the tt7d log:", *bad[:10], sep="\n", file=sys.stderr)
            return 1
        try:
            test_default_rotation(binary, workdir)
        except Exception as e:  # noqa: BLE001
            print(f"FAIL test_e2e: default rotation: {type(e).__name__}: {e}", file=sys.stderr)
            return 1
        steps.append("default rotation is 270 (upright on the TT7)")
        for s in steps:
            print(f"  ok   e2e: {s}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
