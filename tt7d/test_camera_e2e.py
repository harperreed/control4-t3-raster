#!/usr/bin/env python3
# ABOUTME: Camera end-to-end test: the real host-built tt7d and its real worker process, fed NV12 frames through a
# ABOUTME: FIFO (the test-only fake source); snapshots, presence, display wake, MQTT/HA, and killing a stuck worker.
"""Usage: uv run --no-project --with amqtt==0.12.1 --with paho-mqtt==2.1.0 --with pillow==12.3.0 \\
          python tt7d/test_camera_e2e.py --daemon build/host/tt7d      (run by `make test-camera`)

The panel's camera cannot run on a host, so tt7d runs with
--camera-fake-source pointing at a named pipe, and a thread here writes
1280x720 NV12 frames into it: an empty room, or a room with a bright block
("someone") in it. Everything after the frame source is the real code: the
worker process, the JPEG encoder, the snapshot cache, the motion detector,
the presence -> display wake, the WebSocket and MQTT publishing, and the
supervision that kills a stuck worker. A reader of the fake source ignores
signals the way a stuck driver call does, so a paused writer is a stuck
worker that only SIGKILL ends.
"""
import argparse
import io
import json
import os
import re
import socket
import sys
import tempfile
import threading
import time

import paho.mqtt.client as mqtt
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import test_e2e  # noqa: E402 - the Daemon wrapper and PNG helpers
from test_mqtt_e2e import Broker, wait_for  # noqa: E402 - a real amqtt broker
from events import EventStream  # noqa: E402 - tools/events.py's WebSocket client

W, H = 1280, 720
PERSON = (400, 150, 900, 650)  # x0, y0, x1, y1 of the bright block


def nv12(scene):
    """One NV12 frame: a horizontal luma ramp, with a bright block when someone is there. Grey chroma."""
    row = bytes(40 + x * 120 // W for x in range(W))
    luma = bytearray(row * H)
    if scene == "person":
        x0, y0, x1, y1 = PERSON
        for y in range(y0, y1):
            luma[y * W + x0:y * W + x1] = bytes([230]) * (x1 - x0)
    return bytes(luma) + bytes([128]) * (W * H // 2)


class Feeder(threading.Thread):
    """Writes whole frames of the current scene into the FIFO, as fast as the worker reads them.

    Holding the FIFO open read-write means neither side's open() waits, and the
    worker never sees end-of-file. pause() takes effect between frames, so the
    worker then waits for a frame that does not come: stuck, in a read()."""

    def __init__(self, path):
        super().__init__(daemon=True)
        self.fd = os.open(path, os.O_RDWR)
        self.frames = {"empty": nv12("empty"), "person": nv12("person")}
        self.scene = "empty"
        self.paused = threading.Event()
        self.stopped = False
        self.written = 0

    def run(self):
        while not self.stopped:
            if self.paused.is_set():
                time.sleep(0.02)
                continue
            os.write(self.fd, self.frames[self.scene])  # blocks until the worker has read most of it
            self.written += 1

    def stop(self):
        self.stopped = True


class RawObserver:
    """A paho client subscribed to tt7/# and homeassistant/#, keeping payloads as bytes."""

    def __init__(self, broker, name="camera-observer"):
        self.lock = threading.Lock()
        self.messages = []
        self.connected = False
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"{name}-{os.getpid()}")
        self.c.on_message = self._on_message
        self.c.on_connect = self._on_connect
        self.c.connect_async("127.0.0.1", broker.port, keepalive=10)
        self.c.loop_start()
        wait_for(f"{name} connected", lambda: self.connected)

    def _on_connect(self, c, userdata, flags, reason, props):
        if not reason.is_failure:
            c.subscribe([("tt7/#", 0), ("homeassistant/#", 0)])
            self.connected = True

    def _on_message(self, c, userdata, m):
        with self.lock:
            self.messages.append((m.topic, bytes(m.payload), bool(m.retain)))

    def mark(self):
        with self.lock:
            return len(self.messages)

    def expect(self, topic, pred=lambda payload, retain: True, since=0, timeout=15.0):
        """The first message on topic after index `since` that satisfies pred."""
        def find():
            with self.lock:
                for t, p, r in self.messages[since:]:
                    if t == topic and pred(p, r):
                        return (p, r)
            return None
        return wait_for(f"a matching message on {topic}", find, timeout=timeout)

    def retained(self, topic):
        """The latest retained value this client saw on topic (on subscribe or live with retain)."""
        with self.lock:
            vals = [p for t, p, r in self.messages if t == topic and r]
        return vals[-1] if vals else None

    def publish(self, topic, payload):
        self.c.publish(topic, payload, qos=0, retain=False).wait_for_publish(5)

    def close(self):
        self.c.loop_stop()
        self.c.disconnect()


def retained_now(broker):
    """What a brand-new subscriber gets retained: {topic: payload bytes} (empty payloads are deletions)."""
    o = RawObserver(broker, name="retained")
    time.sleep(1.5)
    with o.lock:
        snap = {t: p for t, p, r in o.messages if r}
    o.close()
    return snap


class Tt7d(test_e2e.Daemon):
    def get_json(self, path, token=True):
        headers = {"Authorization": f"Bearer {self.token()}"} if token else {}
        status, _, body = self.request("GET", path, headers=headers)
        assert status == 200, f"GET {path}: {status} {body[:300]!r}"
        return json.loads(body)

    def camera(self):
        return self.get_json("/api/v1/state")["camera"]

    def snapshot(self, token=True):
        headers = {"Authorization": f"Bearer {self.token()}"} if token else {}
        return self.request("GET", "/api/v1/camera/snapshot", headers=headers)

    def put_camera(self, doc, token=True):
        return self.api("PUT", "/api/v1/config/camera", doc, token=token)

    def log(self):
        with open(self.log_path, "rb") as f:
            return f.read().decode("utf-8", "replace")


def check_error(result, status, code):
    got, _, body = result
    assert got == status, f"HTTP {got}, want {status}: {body[:300]!r}"
    assert json.loads(body)["error"] == code, body


def check_jpeg(data, scene=None):
    """A baseline JPEG of 1280x720, and (with scene) showing that scene."""
    assert data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9", "no SOI/EOI"
    i = data.find(b"\xff\xc0")
    assert i > 0, "no SOF0 (baseline) marker"
    h, w = int.from_bytes(data[i + 5:i + 7], "big"), int.from_bytes(data[i + 7:i + 9], "big")
    assert (w, h) == (W, H), f"SOF0 says {w}x{h}"
    img = Image.open(io.BytesIO(data))
    assert img.size == (W, H) and img.mode == "RGB", (img.size, img.mode)
    if scene:
        inside = img.getpixel(((PERSON[0] + PERSON[2]) // 2, (PERSON[1] + PERSON[3]) // 2))
        outside = img.getpixel((60, 60))  # luma 45: dark grey
        assert max(outside) < 80, f"background pixel {outside}"
        if scene == "person":
            assert min(inside) > 200, f"the bright block is {inside}"
        else:
            assert max(inside) < 200, f"no block expected, got {inside}"


# ---- steps ----

def test_panel_section(d):
    status, headers, page = d.request("GET", "/")
    assert status == 200 and b'<script src="/camera.js" defer></script>' in page and b'id="camera"' in page
    status, headers, js = d.request("GET", "/camera.js")
    assert status == 200 and headers["content-type"] == "text/javascript; charset=utf-8", (status, headers)
    assert js.startswith(b"// ABOUTME:") and headers["content-security-policy"] == test_e2e.CSP
    # The snapshot is shown as a data: URL, which the unchanged img-src allows; no HTML is ever built from text.
    assert b"readAsDataURL" in js and b"innerHTML" not in js and b"insertAdjacentHTML" not in js


def test_off_by_default(d, obs, base, device_id):
    info = d.get_json("/api/v1/info", token=False)
    cam = info["capabilities"]["camera"]
    assert cam["available"] is True and cam["enabled"] is False and cam["test_source"] is True, cam
    assert cam["snapshot"] == {"path": "/api/v1/camera/snapshot", "format": "image/jpeg", "width": W, "height": H,
                               "quality": 80}, cam
    for route in ("GET /api/v1/camera/snapshot", "GET /api/v1/config/camera", "PUT /api/v1/config/camera"):
        assert route in info["auth"]["required_for"], route
    state = d.camera()
    assert state["enabled"] is False and state["presence_enabled"] is False and state["present"] is None, state
    assert state["worker"] == "off" and state["last_snapshot_at"] is None and state["worker_restarts"] == 0, state
    check_error(d.snapshot(token=False), 401, "unauthorized")
    check_error(d.snapshot(), 503, "camera_disabled")
    check_error(d.request("GET", "/api/v1/config/camera"), 401, "unauthorized")
    check_error(d.request("POST", "/api/v1/camera/snapshot", body=b"", headers={}), 405, "method_not_allowed")
    snap = retained_now(obs.broker)
    for topic in (f"homeassistant/camera/{device_id}/camera/config",
                  f"homeassistant/binary_sensor/{device_id}/presence/config"):
        assert not snap.get(topic), f"{topic} announced while the camera is off: {snap.get(topic)!r}"
    assert not snap.get(f"{base}/presence"), "presence published while off"


def test_enable(d, obs, base, device_id):
    before = d.get_json("/api/v1/config/camera")
    assert before["enabled"] is False and before["set_by_flags"] == [] and before["available"] is True, before
    check_error(d.put_camera({"enabled": True}, token=False), 401, "unauthorized")
    check_error(d.put_camera({"enabled": "yes"}), 400, "invalid_config")
    status, _, body = d.request("PUT", "/api/v1/config/camera", body=b'{"enabled":true}',
                                headers={"Authorization": f"Bearer {d.token()}", "Content-Type": "text/plain"})
    assert status == 415, body
    mark = obs.mark()
    status, _, body = d.put_camera({"enabled": True, "settle_frames": 1, "worker_timeout_s": 2,
                                    "snapshot_max_age_s": 2})
    after = test_e2e.jbody(status, body, 200)
    assert after["enabled"] is True and after["config_revision"] == before["config_revision"] + 1, after
    with open(os.path.join(d.data, "camera.conf")) as f:
        conf = f.read()
    assert "\ncamera=on\n" in conf and "\nsettle_frames=1\n" in conf, conf
    wait_for("the worker running", lambda: d.camera()["worker"] == "running")

    cfg, _ = obs.expect(f"homeassistant/camera/{device_id}/camera/config", lambda p, r: bool(p), since=mark)
    cfg = json.loads(cfg)
    assert cfg["topic"] == f"{base}/camera/image" and cfg["unique_id"] == f"{device_id}_camera", cfg
    assert cfg["availability_topic"] == f"{base}/availability" and cfg["device"]["identifiers"] == [device_id], cfg
    assert "image_encoding" not in cfg, "raw JPEG bytes, not base64"
    assert not retained_now(obs.broker).get(f"homeassistant/binary_sensor/{device_id}/presence/config"), \
        "presence entity announced while presence is off"


def test_snapshot(d, feeder, workdir):
    feeder.scene = "person"
    t0 = time.monotonic()
    status, headers, body = d.snapshot()
    assert status == 200 and headers["content-type"] == "image/jpeg", (status, headers, body[:200])
    check_jpeg(body, "person")
    captured = headers["x-captured-at"]
    assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z", captured), captured
    state = d.camera()
    assert state["last_snapshot_at"] == captured, state

    # Within snapshot_max_age_s the same picture comes from memory: no capture.
    written = feeder.written
    status2, headers2, body2 = d.snapshot()
    assert status2 == 200 and headers2["x-captured-at"] == captured and body2 == body, "not served from the cache"
    assert feeder.written == written, "a cached snapshot read frames from the camera"
    wait_for("the cache to expire", lambda: time.monotonic() - t0 > 2.2, timeout=5)
    feeder.scene = "empty"
    status3, headers3, body3 = d.snapshot()
    assert status3 == 200 and headers3["x-captured-at"] != captured, "the cache did not expire"
    check_jpeg(body3)

    # Privacy: pictures live in memory only.
    for root, _, files in os.walk(d.data):
        for name in files:
            with open(os.path.join(root, name), "rb") as f:
                assert f.read(2) != b"\xff\xd8", f"a JPEG was written to {name}"


def test_mqtt_snapshot(d, obs, base):
    time.sleep(2.1)  # past the cache, so this is a fresh capture
    mark = obs.mark()
    obs.publish(f"{base}/cmd/snapshot", b"")
    payload, retain = obs.expect(f"{base}/camera/image", since=mark)
    assert not retain, "camera/image must not be retained"
    check_jpeg(payload)
    # A retained cmd/snapshot is ignored, like every retained command.
    mark = obs.mark()
    obs.c.publish(f"{base}/cmd/snapshot", b"", retain=True).wait_for_publish(5)
    time.sleep(1.0)
    obs.c.publish(f"{base}/cmd/snapshot", b"", retain=True).wait_for_publish(5)  # clear it again
    with obs.lock:
        images = [m for m in obs.messages[mark:] if m[0] == f"{base}/camera/image"]
    assert len(images) <= 1, "a retained cmd/snapshot kept triggering"


def messages_of(ws, kind, n, what, timeout=15):
    out = []
    deadline = time.monotonic() + timeout
    while len(out) < n and time.monotonic() < deadline:
        msg = ws.next_message()
        assert msg is not None, f"stream closed while waiting for {what}"
        ev = json.loads(msg)
        if ev.get("type") == kind:
            out.append(ev)
    assert len(out) == n, f"got {out} while waiting for {what}"
    return out


def test_presence(d, obs, base, device_id, feeder):
    feeder.scene = "empty"
    mark = obs.mark()
    status, _, body = d.put_camera({"presence": True, "presence_wake": True, "presence_idle_blank_s": 1,
                                    "presence_interval_ms": 100})
    assert status == 200, body
    cfg, _ = obs.expect(f"homeassistant/binary_sensor/{device_id}/presence/config", lambda p, r: bool(p), since=mark)
    cfg = json.loads(cfg)
    assert cfg["state_topic"] == f"{base}/presence" and cfg["device_class"] == "occupancy", cfg
    assert (cfg["payload_on"], cfg["payload_off"]) == ("ON", "OFF") and cfg["unique_id"] == f"{device_id}_presence"
    obs.expect(f"{base}/presence", lambda p, r: p == b"OFF", since=mark)
    wait_for("presence frames scored", lambda: (c := d.camera())["frames_scored"] >= 3 and c["present"] is False)

    ws = EventStream("127.0.0.1", d.port, d.token())
    assert json.loads(ws.next_message())["type"] == "hello"
    status, _, body = d.api("POST", "/api/v1/display/blank")
    assert status == 200 and d.bl("bl_power") == "4", body
    level = test_e2e.jbody(status, body, 200)["wake_brightness_raw"]
    assert d.bl("brightness") == str(level), "blank changed brightness"

    mark = obs.mark()
    feeder.scene = "person"
    ev = messages_of(ws, "presence", 1, "presence on")[0]
    assert ev["present"] is True and ev["score"] >= 8 and "timestamp" in ev, ev
    obs.expect(f"{base}/presence", lambda p, r: p == b"ON", since=mark)
    wait_for("presence woke the display", lambda: d.bl("bl_power") == "0")
    assert d.bl("brightness") == str(level), d.bl("brightness")
    assert d.camera()["present"] is True

    # A snapshot while presence streams shares the stream: same worker, no restart.
    pid = d.camera()["worker_pid"]
    time.sleep(2.1)
    status, _, jpeg = d.snapshot()
    assert status == 200, jpeg[:200]
    check_jpeg(jpeg, "person")
    assert d.camera()["worker_pid"] == pid and d.camera()["worker_restarts"] == 0

    mark = obs.mark()
    feeder.scene = "empty"
    ev = messages_of(ws, "presence", 1, "presence off")[0]
    assert ev["present"] is False, ev
    obs.expect(f"{base}/presence", lambda p, r: p == b"OFF", since=mark)
    left = time.monotonic()
    wait_for("the idle blank", lambda: d.bl("bl_power") == "4", timeout=10)
    assert time.monotonic() - left >= 0.8, "blanked before presence_idle_blank_s"
    ws.close()


def test_stuck_worker(d, feeder):
    assert d.put_camera({"snapshot_max_age_s": 0})[0] == 200  # every request captures; the worker keeps running
    before = d.camera()
    feeder.paused.set()
    result = {}

    def snap():
        t = time.monotonic()
        result["r"] = d.snapshot()
        result["s"] = time.monotonic() - t
    # Long enough for the worker to be stuck in its next read (100 ms frames),
    # well inside its 2 s worker timeout.
    time.sleep(0.5)
    th = threading.Thread(target=snap)
    th.start()
    # The display keeps working while the worker hangs.
    for i in range(6):
        png = test_e2e.png_bytes(test_e2e.LOGICAL_W, test_e2e.LOGICAL_H, test_e2e.random_image(700 + i))
        t = time.monotonic()
        status, _, body = d.put_frame(png, **{"X-Frame-ID": f"during-hang-{i}"})
        took = time.monotonic() - t
        assert status == 200 and took < 1.0, f"frame PUT {status} in {took:.2f} s while the worker hangs: {body[:200]!r}"
        time.sleep(0.3)
    th.join(20)
    assert "r" in result, "the snapshot request never finished"
    check_error(result["r"], 503, "camera_timeout")
    assert result["s"] < 8, f"the snapshot waited {result['s']:.1f} s"
    log = d.log()
    assert re.search(r"worker \d+ gave no sign of life for 2 s", log), log[-2000:]
    assert re.search(r"worker \d+ still running 1000 ms after SIGTERM; SIGKILL", log), log[-2000:]
    assert re.search(r"worker \d+ reaped \(killed by signal 9\)", log), log[-2000:]
    after = wait_for("a new worker", lambda: (c := d.camera())["worker"] == "running" and c["worker_pid"] != before["worker_pid"] and c)
    assert after["worker_restarts"] == before["worker_restarts"] + 1, after
    feeder.paused.clear()
    wait_for("presence frames again", lambda: d.camera()["frames_scored"] >= 2)
    time.sleep(2.1)
    status, _, body = d.snapshot()
    assert status == 200, body[:200]
    check_jpeg(body)


def test_disable(d, obs, base, device_id):
    mark = obs.mark()
    status, _, body = d.put_camera({"enabled": False})
    assert status == 200, body
    obs.expect(f"homeassistant/camera/{device_id}/camera/config", lambda p, r: p == b"", since=mark)
    obs.expect(f"homeassistant/binary_sensor/{device_id}/presence/config", lambda p, r: p == b"", since=mark)
    check_error(d.snapshot(), 503, "camera_disabled")
    state = wait_for("the worker gone", lambda: (c := d.camera())["worker"] == "off" and c)
    assert state["last_snapshot_at"] is None and state["present"] is None, "the picture outlived the camera"
    snap = retained_now(obs.broker)
    for topic in (f"homeassistant/camera/{device_id}/camera/config",
                  f"homeassistant/binary_sensor/{device_id}/presence/config", f"{base}/presence"):
        assert not snap.get(topic), f"{topic} still retained: {snap.get(topic)!r}"
    log = d.log()
    assert not re.search(r"worker \d+ reaped", log.split("settings changed (revision")[-1]), \
        "a settings stop should exit cleanly"


def test_flag(binary, workdir, fifo):
    """--camera on turns it on over camera.conf, and PUT cannot change what the flag set."""
    flagdir = os.path.join(workdir, "flag")
    os.makedirs(flagdir)
    d = Tt7d(binary, flagdir)
    d.extra_args = ["--camera", "on", "--camera-fake-source", fifo, "--ntp-marker", os.path.join(workdir, "nomarker")]
    try:
        d.start()
        cfg = d.get_json("/api/v1/config/camera")
        assert cfg["enabled"] is True and cfg["set_by_flags"] == ["enabled"], cfg
        check_error(d.put_camera({"enabled": False}), 409, "set_by_flag")
        assert d.put_camera({"snapshot_max_age_s": 5})[0] == 200
    finally:
        d.stop()


def test_update_restart(binary, workdir, fifo):
    """An update restart (tt7d exits 75) stops the worker through SIGTERM and reaps it before tt7d exits.

    The rollback is the cheapest way to that exit: the update root holds one
    usable previous release (an executable bin/tt7d and app, never run here)."""
    rundir = os.path.join(workdir, "update-restart")
    root = os.path.join(rundir, "tt7")
    rel = os.path.join(root, "releases", "older", "bin")
    os.makedirs(rel)
    os.makedirs(os.path.join(root, "update"))
    for path in (os.path.join(rel, "tt7d"), os.path.join(root, "releases", "older", "app")):
        with open(path, "w") as f:
            f.write("#!/bin/sh\nexit 0\n")
        os.chmod(path, 0o755)
    with open(os.path.join(root, "update", "previous"), "w") as f:
        f.write("older\n")
    d = Tt7d(binary, rundir)
    d.extra_args = ["--camera", "on", "--camera-fake-source", fifo, "--update-root", root,
                    "--ntp-marker", os.path.join(workdir, "nomarker")]
    try:
        d.start()
        state = d.get_json("/api/v1/state")
        assert "camera" in state and "update" in state, f"/state members: {sorted(state)}"
        assert state["update"] == {"release": None, "restart_pending": False}, state["update"]
        cam = wait_for("the worker running", lambda: (c := d.camera())["worker"] == "running" and c)
        pid = cam["worker_pid"]
        status, _, body = d.api("POST", "/api/v1/system/update/rollback")
        assert status == 202, body
        assert d.get_json("/api/v1/state")["update"]["restart_pending"] is True
        rc = d.proc.wait(timeout=10)
        d.proc = None
        assert rc == 75, f"tt7d exit status {rc}, want 75"
        # tt7d reaped it: not a zombie, not an orphan still holding the camera.
        assert not os.path.exists(f"/proc/{pid}"), f"worker {pid} outlived tt7d"
        log = d.log()
        assert re.search(rf"camera: worker {pid} stopped before tt7d exits \(exit status 0\)", log), log[-1500:]
        assert "SIGKILL" not in log, "the worker needed SIGKILL"
        assert not re.search(r"AddressSanitizer|runtime error|LeakSanitizer", log), "sanitizer findings"
    finally:
        d.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--daemon", required=True, help="host-built tt7d binary")
    args = ap.parse_args()
    binary = os.path.abspath(args.daemon)

    with tempfile.TemporaryDirectory(prefix="tt7d-camera-") as workdir:
        fifo = os.path.join(workdir, "camera.nv12")
        os.mkfifo(fifo)
        feeder = Feeder(fifo)
        feeder.start()
        broker = Broker(workdir, "broker")
        d = Tt7d(binary, workdir)
        obs, steps = None, []
        try:
            broker.start()
            obs = RawObserver(broker)
            obs.broker = broker
            d.extra_args = ["--mqtt-host", "127.0.0.1", "--mqtt-port", str(broker.port),
                            "--camera-fake-source", fifo, "--ntp-marker", os.path.join(workdir, "ntp-synced")]
            d.start()
            device_id = d.get_json("/api/v1/info", token=False)["device_id"]
            base = f"tt7/{device_id}"
            wait_for("MQTT connected", lambda: d.get_json("/api/v1/state")["mqtt"]["connected"])
            steps.append("control panel: Camera section and /camera.js under the same CSP")
            test_panel_section(d)
            steps.append("off by default: 503 camera_disabled, 401 without the token, no HA entities")
            test_off_by_default(d, obs, base, device_id)
            steps.append("PUT /api/v1/config/camera: token, validation, camera.conf, config_revision, HA camera entity")
            test_enable(d, obs, base, device_id)
            steps.append("snapshot: 1280x720 baseline JPEG of the scene, served from memory within max age")
            test_snapshot(d, feeder, workdir)
            steps.append("MQTT cmd/snapshot -> camera/image (raw JPEG, not retained)")
            test_mqtt_snapshot(d, obs, base)
            steps.append("presence: WebSocket + MQTT ON/OFF, HA occupancy, wakes a blank display, idle blank")
            test_presence(d, obs, base, device_id, feeder)
            steps.append("a stuck worker: SIGTERM, SIGKILL, reaped, restarted; frame PUTs never wait")
            test_stuck_worker(d, feeder)
            steps.append("disable: 503, worker gone, picture dropped, HA entities and presence removed")
            test_disable(d, obs, base, device_id)
            d.stop()
            steps.append("--camera on beats camera.conf and locks enabled (409)")
            test_flag(binary, workdir, fifo)
            steps.append("an update restart (exit 75) stops the worker with SIGTERM and reaps it first; "
                         "/state has camera and update")
            test_update_restart(binary, workdir, fifo)
        except Exception as e:  # noqa: BLE001 - report which step failed, with the logs
            print(f"FAIL test_camera_e2e: {steps[-1] if steps else 'start'}: {type(e).__name__}: {e}", file=sys.stderr)
            d.stop()
            for path in (d.log_path, broker.log_path):
                if os.path.exists(path):
                    with open(path, "rb") as f:
                        tail = f.read().decode("utf-8", "replace").splitlines()[-30:]
                    print(f"---- {os.path.basename(path)} (last 30 lines) ----", *tail, sep="\n", file=sys.stderr)
            return 1
        finally:
            if obs:
                obs.close()
            d.stop()
            feeder.stop()
            feeder.paused.clear()
            broker.stop()
        log = d.log()
        if d.token() in log:
            print("FAIL test_camera_e2e: the token appeared in the log", file=sys.stderr)
            return 1
        bad = [line for line in log.splitlines() if re.search(r"AddressSanitizer|runtime error|LeakSanitizer", line)]
        if bad:
            print("FAIL test_camera_e2e: sanitizer findings in the tt7d log:", *bad[:10], sep="\n", file=sys.stderr)
            return 1
        for s in steps:
            print(f"  ok   camera: {s}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
