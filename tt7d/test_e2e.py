#!/usr/bin/env python3
# ABOUTME: End-to-end test: runs the real host-built tt7d on a file-backed 800x1280 RGB565 framebuffer.
# ABOUTME: Pushes frames over real HTTP and checks the framebuffer bytes against an independent Python rotation.
"""Usage: tt7d/test_e2e.py --daemon build/host/tt7d   (run by `make test-e2e`)

Nothing here is mocked: the daemon is the same source as the panel build,
compiled for the host, listening on a free loopback port, with a temporary
data dir and the sysfs fixture copied from the real panel. The expected
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
    def __init__(self, binary, workdir):
        self.binary = binary
        self.fb = os.path.join(workdir, "fb.raw")
        self.data = os.path.join(workdir, "data")
        self.log_path = os.path.join(workdir, "tt7d.log")
        self.port = free_port()
        self.proc = None
        with open(self.fb, "wb") as f:
            f.write(bytes(FB_BYTES))

    def start(self):
        log = open(self.log_path, "ab")
        self.proc = subprocess.Popen(
            [self.binary, "--listen", f"127.0.0.1:{self.port}", "--fb-file", self.fb,
             "--fb-geometry", f"{NATIVE_W}x{NATIVE_H}x16", "--fb-stride", str(STRIDE), "--fb-format", "rgb565",
             "--rotation", "90", "--data-dir", self.data, "--sysfs-root", FIXTURE,
             "--request-timeout-ms", str(TIMEOUT_MS)],
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
    assert disp["native"] == {"width": 800, "height": 1280, "format": "rgb565", "stride": 1600,
                              "bits_per_pixel": 16}, disp["native"]
    caps = info["capabilities"]
    assert caps["touch"] == {"available": True, "device": "gslX680"}, caps["touch"]
    assert caps["ethernet"] == {"available": False, "interface": None}, caps["ethernet"]
    assert caps["camera"]["available"] is None
    assert os.stat(os.path.join(d.data, "token")).st_mode & 0o777 == 0o600

    state = jbody(*d.request("GET", "/api/v1/state")[::2], 200)
    assert state["display"]["frame_id"] is None and state["display"]["frame_age_s"] is None, state["display"]
    assert state["display"]["brightness"] == {"value": 50, "unit": "percent", "available": True}
    assert state["power"]["battery_percent"]["estimate"] is True
    assert state["frames"] == {"accepted": 0, "deduplicated": 0, "rejected": 0, "last_error": None}
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
    assert jbody(status, body, 405)["error"] == "method_not_allowed" and headers["allow"] == "GET, PUT", headers


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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--daemon", required=True, help="host-built tt7d binary")
    args = ap.parse_args()
    binary = os.path.abspath(args.daemon)

    with tempfile.TemporaryDirectory(prefix="tt7d-e2e-") as workdir:
        d = Daemon(binary, workdir)
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
        for s in steps:
            print(f"  ok   e2e: {s}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
