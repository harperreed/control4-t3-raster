#!/usr/bin/env python3
# ABOUTME: Input end-to-end test: the real host-built tt7d reads real struct input_event records from FIFOs and streams
# ABOUTME: touch/button JSON over its WebSocket (checked with tools/events.py too); buttons also reach a real amqtt broker.
"""Usage: uv run --no-project --with amqtt==0.12.1 --with paho-mqtt==2.1.0 \\
          python tt7d/test_input_e2e.py --daemon build/host/tt7d      (run by `make test-input`)

Nothing is mocked. evdev itself cannot be faked without /dev/uinput (root),
so the daemon's --input-dir points at named pipes with the panel's node
names (event0 = rk29-keypad, event1 = gslX680, from the sysfs fixture), and
the test writes native `struct input_event` records into them. The pipes
are not evdev devices, so tt7d takes their capabilities from the fixture's
modalias and the touch ranges from event1.absinfo, which holds the ranges
tt7probe recorded on the panel (hardware/discovery/boot-0002-up22s). The
expected logical coordinates are computed here, independently of the C code.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import test_e2e  # noqa: E402 - the Daemon wrapper and PNG helpers
from test_mqtt_e2e import Broker, Observer, wait_for  # noqa: E402 - real amqtt broker, real paho client
from events import EventStream, HandshakeError  # noqa: E402 - tools/events.py's WebSocket client

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
SYN_REPORT = 0
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2F, 0x35, 0x36, 0x39
KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_POWER, KEY_WAKEUP = 114, 115, 116, 143

# gslX680's ranges as tt7probe read them with EVIOCGABS on the panel.
RAW_X_MAX, RAW_Y_MAX = 1280, 800
ABSINFO = "# from hardware/discovery/boot-0002-up22s/input-devices.txt\n0x2f 0 10\n0x35 0 1280\n0x36 0 800\n"
NATIVE_W, NATIVE_H = test_e2e.NATIVE_W, test_e2e.NATIVE_H
ISO = re.compile(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z")


def record(etype, code, value):
    """One struct input_event in the host's native layout (timeval, u16 type, u16 code, s32 value)."""
    return struct.pack("@llHHi", 0, 0, etype, code, value)


def syn():
    return record(EV_SYN, SYN_REPORT, 0)


def expected_logical(rx, ry, rotation):
    """Raw -> native (scaled like tt7probe's dots) -> logical, by the rotation rule in tt7d/README.md."""
    nx = min(max(rx, 0), RAW_X_MAX) * (NATIVE_W - 1) // RAW_X_MAX
    ny = min(max(ry, 0), RAW_Y_MAX) * (NATIVE_H - 1) // RAW_Y_MAX
    if rotation == 90:  # logical (x, y) is drawn at native (799 - y, x)
        return ny, NATIVE_W - 1 - nx
    if rotation == 270:  # logical (x, y) is drawn at native (y, 1279 - x)
        return NATIVE_H - 1 - ny, nx
    raise ValueError(rotation)


class Panel(test_e2e.Daemon):
    """tt7d with FIFOs for its two input devices."""

    def __init__(self, binary, workdir, extra_args=()):
        super().__init__(binary, workdir, extra_args)
        for node in ("event0", "event1"):
            os.mkfifo(os.path.join(self.input_dir, node))
        with open(os.path.join(self.input_dir, "event1.absinfo"), "w") as f:
            f.write(ABSINFO)
        self.keys = self.touch = None

    def start(self):
        super().start()
        # The daemon holds the read ends open, so these opens return at once.
        self.keys = os.open(os.path.join(self.input_dir, "event0"), os.O_WRONLY)
        self.touch = os.open(os.path.join(self.input_dir, "event1"), os.O_WRONLY)

    def stop(self):
        super().stop()
        for fd in (self.keys, self.touch):
            if fd is not None:
                os.close(fd)
        self.keys = self.touch = None

    def write_touch(self, *records):
        os.write(self.touch, b"".join(records))

    def write_keys(self, *records):
        os.write(self.keys, b"".join(records))

    def get_json(self, path):
        status, _, body = self.request("GET", path)
        assert status == 200, f"GET {path}: {status} {body[:200]!r}"
        return json.loads(body)

    def stream(self, **kw):
        ws = EventStream("127.0.0.1", self.port, self.token(), **kw)
        hello = json.loads(ws.next_message())
        assert hello["type"] == "hello", hello
        return ws, hello

    def log(self):
        with open(self.log_path, "rb") as f:
            return f.read().decode("utf-8", "replace")


def messages(ws, n, what):
    out = []
    for _ in range(n):
        msg = ws.next_message()
        assert msg is not None, f"stream closed (code {ws.close_code}) after {out} while waiting for {what}"
        out.append(json.loads(msg))
    return out


def put_frame(d, frame_id):
    png = test_e2e.png_bytes(test_e2e.LOGICAL_W, test_e2e.LOGICAL_H, bytes(test_e2e.LOGICAL_W * test_e2e.LOGICAL_H * 4))
    status, _, body = d.put_frame(png, **{"X-Frame-ID": frame_id})
    assert status == 200, f"PUT frame: {status} {body[:200]!r}"


def check_touch(ev, action, pointer, raw, rotation, frame_id):
    x, y = expected_logical(raw[0], raw[1], rotation)
    assert ev["type"] == "touch" and ev["action"] == action and ev["pointer"] == pointer, ev
    assert (ev["x"], ev["y"]) == (x, y), f"{action} at raw {raw}, rotation {rotation}: got ({ev['x']}, {ev['y']}), want ({x}, {y})"
    assert abs(ev["nx"] - x / 1280) < 1e-4 and abs(ev["ny"] - y / 800) < 1e-4, ev
    assert ev["frame_id"] == frame_id, f"frame_id {ev['frame_id']!r}, want {frame_id!r}"
    assert ISO.fullmatch(ev["timestamp"]) and isinstance(ev["monotonic_ms"], int), ev


# ---- tests ------------------------------------------------------------------

def test_info_and_state(d):
    info = d.get_json("/api/v1/info")
    inp = info["capabilities"]["input"]
    assert inp["events"] == "/api/v1/events", inp
    assert inp["touch"]["device"] == "gslX680" and inp["touch"]["protocol"] == "mt_b", inp["touch"]
    assert inp["touch"]["pointers"] == 11, inp["touch"]
    assert inp["touch"]["raw"] == {"x": {"min": 0, "max": 1280}, "y": {"min": 0, "max": 800}}, inp["touch"]
    assert inp["touch"]["coordinates"] == {"width": 1280, "height": 800, "space": "logical"}, inp["touch"]
    assert inp["buttons"] == ["volume_down", "volume_up", "power", "key_143"], inp["buttons"]
    assert "GET /api/v1/events" in info["auth"]["required_for"]
    state = d.get_json("/api/v1/state")["input"]
    assert state == {"last_touch": None, "last_button": None, "event_clients": 0, "event_clients_dropped_slow": 0}, state
    log = d.log()
    assert 'event1 "gslX680": touch, multitouch protocol B, 11 pointer(s), raw x 0..1280 y 0..800' in log, log
    assert "114=volume_down 115=volume_up 116=power 143=key_143" in log, log


def test_auth(d):
    token = d.token()
    for kwargs, why in (({"token": None}, "no token"), ({"token": "wrong"}, "wrong header token"),
                        ({"token": "wrong", "query_token": True}, "wrong query token")):
        try:
            EventStream("127.0.0.1", d.port, **kwargs)
            raise AssertionError(f"{why}: the stream opened")
        except HandshakeError as e:
            assert e.status == 401 and b'"unauthorized"' in e.body, f"{why}: {e}"
    ws = EventStream("127.0.0.1", d.port, token, query_token=True)
    assert json.loads(ws.next_message())["type"] == "hello"
    ws.close()
    status, headers, body = d.request("GET", "/api/v1/events", headers={"Authorization": f"Bearer {token}"})
    assert status == 426 and headers.get("upgrade") == "websocket", (status, headers, body)
    status, headers, _ = d.request("POST", "/api/v1/events", body=b"", headers={"Authorization": f"Bearer {token}"})
    assert status == 405 and headers.get("allow") == "GET", status


def touch_sequence(d, ws, rotation, frame_id):
    """Down, two spaced moves, a burst of moves, up: check order, coordinates, throttling, final position."""
    d.write_touch(record(EV_ABS, ABS_MT_SLOT, 0), record(EV_ABS, ABS_MT_TRACKING_ID, 100),
                  record(EV_ABS, ABS_MT_POSITION_X, 100), record(EV_ABS, ABS_MT_POSITION_Y, 50), syn())
    check_touch(messages(ws, 1, "down")[0], "down", 0, (100, 50), rotation, frame_id)
    time.sleep(0.05)
    d.write_touch(record(EV_ABS, ABS_MT_POSITION_X, 640), syn())
    check_touch(messages(ws, 1, "move")[0], "move", 0, (640, 50), rotation, frame_id)
    time.sleep(0.05)
    # A second finger in slot 3 while the first rests, then 20 moves of it in one burst and its lift.
    d.write_touch(record(EV_ABS, ABS_MT_SLOT, 3), record(EV_ABS, ABS_MT_TRACKING_ID, 101),
                  record(EV_ABS, ABS_MT_POSITION_X, 1280), record(EV_ABS, ABS_MT_POSITION_Y, 800), syn())
    check_touch(messages(ws, 1, "second down")[0], "down", 3, (1280, 800), rotation, frame_id)
    time.sleep(0.05)
    burst = []
    for i in range(20):
        burst += [record(EV_ABS, ABS_MT_POSITION_X, 1000 - 10 * i), syn()]
    burst += [record(EV_ABS, ABS_MT_TRACKING_ID, -1), syn()]
    d.write_touch(*burst)
    got = []
    while not got or got[-1]["action"] != "up":
        got += messages(ws, 1, "burst")
    moves, up = got[:-1], got[-1]
    assert all(m["action"] == "move" and m["pointer"] == 3 for m in moves), got
    assert 1 <= len(moves) <= 3, f"20 moves within a few ms should be throttled to 1-3, got {len(moves)}"
    check_touch(moves[-1], "move", 3, (810, 800), rotation, frame_id)  # the final position, just before the up
    check_touch(up, "up", 3, (810, 800), rotation, frame_id)
    gaps = [b["monotonic_ms"] - a["monotonic_ms"] for a, b in zip(moves, moves[1:])]
    assert all(g >= 16 for g in gaps[:-1]), f"moves closer than 16 ms: {gaps}"
    d.write_touch(record(EV_ABS, ABS_MT_SLOT, 0), record(EV_ABS, ABS_MT_TRACKING_ID, -1), syn())
    check_touch(messages(ws, 1, "first up")[0], "up", 0, (640, 50), rotation, frame_id)


def test_touch_on_fallback_clock(d, obs, base):
    """Before any frame the fallback clock is on screen: touches carry its id, and MQTT state has the last touch."""
    shown = d.get_json("/api/v1/state")["display"]["frame_id"]
    assert shown and shown.startswith("fallback-clock-"), f"/state frame_id {shown!r}, want the fallback clock's"
    ws, hello = d.stream()
    assert hello["frame_id"] == shown, hello
    d.write_touch(record(EV_ABS, ABS_MT_SLOT, 0), record(EV_ABS, ABS_MT_TRACKING_ID, 50),
                  record(EV_ABS, ABS_MT_POSITION_X, 10), record(EV_ABS, ABS_MT_POSITION_Y, 10), syn(),
                  record(EV_ABS, ABS_MT_TRACKING_ID, -1), syn())
    down, up = messages(ws, 2, "touch on the fallback clock")
    assert down["frame_id"] == up["frame_id"] == shown, (down, up)
    ws.close()
    last_touch = d.get_json("/api/v1/state")["input"]["last_touch"]
    assert ISO.fullmatch(last_touch or ""), last_touch
    # A frame changes the state, so tt7d publishes it; it carries the touch time.
    obs.drain()
    put_frame(d, "after-clock-touch")
    payload, _ = obs.expect(f"{base}/state", lambda p, r: json.loads(p)["frame_id"] == "after-clock-touch")
    assert json.loads(payload)["last_touch"] == last_touch, payload


def test_touch(d, rotation, events_tool):
    frame_id = f"touch-frame-{rotation}"
    put_frame(d, frame_id)
    ws, hello = d.stream()
    assert hello["width"] == 1280 and hello["height"] == 800 and hello["rotation"] == rotation, hello
    assert hello["touch"] is True and hello["frame_id"] == frame_id, hello
    tool, lines = None, []
    if events_tool:  # the shipped CLI as a second, independent client, printing one JSON per line
        env = dict(os.environ, TT7_TOKEN_FILE=os.path.join(d.data, "token"))
        tool = subprocess.Popen([sys.executable, events_tool, f"127.0.0.1:{d.port}", "--timeout", "30",
                                 "--query-token"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, text=True)
        threading.Thread(target=lambda: lines.extend(json.loads(line) for line in tool.stdout), daemon=True).start()
        wait_for("tools/events.py to connect", lambda: d.get_json("/api/v1/state")["input"]["event_clients"] == 2)
    touch_sequence(d, ws, rotation, frame_id)
    if tool:
        wait_for("tools/events.py to print the last up", lambda: lines and lines[-1].get("action") == "up"
                 and lines[-1].get("pointer") == 0)
        tool.terminate()
        tool.wait(10)
        assert lines[0]["type"] == "hello", lines[0]
        acts = [(e["action"], e["pointer"]) for e in lines[1:]]
        assert acts[:3] == [("down", 0), ("move", 0), ("down", 3)] and acts[-2:] == [("up", 3), ("up", 0)], acts
        check_touch(lines[1], "down", 0, (100, 50), rotation, frame_id)
    # A new frame: touches now carry its id.
    put_frame(d, frame_id + "-next")
    d.write_touch(record(EV_ABS, ABS_MT_SLOT, 0), record(EV_ABS, ABS_MT_TRACKING_ID, 102),
                  record(EV_ABS, ABS_MT_POSITION_X, 0), record(EV_ABS, ABS_MT_POSITION_Y, 0), syn(),
                  record(EV_ABS, ABS_MT_TRACKING_ID, -1), syn())
    down, up = messages(ws, 2, "touch on the next frame")
    check_touch(down, "down", 0, (0, 0), rotation, frame_id + "-next")
    check_touch(up, "up", 0, (0, 0), rotation, frame_id + "-next")
    ws.close()
    assert ISO.fullmatch(d.get_json("/api/v1/state")["input"]["last_touch"] or "")


def test_buttons_and_mqtt(d, obs, base):
    ws, _ = d.stream()
    obs.drain()
    d.write_keys(record(EV_KEY, KEY_POWER, 1), syn(), record(EV_KEY, KEY_POWER, 2), syn(),  # 2 = autorepeat, ignored
                 record(EV_KEY, KEY_POWER, 0), syn(), record(EV_KEY, KEY_VOLUMEUP, 1), syn(),
                 record(EV_KEY, KEY_WAKEUP, 1), syn())
    got = messages(ws, 4, "button events")
    want = [("power", "press", 116), ("power", "release", 116), ("volume_up", "press", 115), ("key_143", "press", 143)]
    assert [(e["type"], e["button"], e["action"], e["code"]) for e in got] == [("button",) + w for w in want], got
    assert all(ISO.fullmatch(e["timestamp"]) and "frame_id" in e for e in got), got
    for w in want:
        payload, retained = obs.expect(f"{base}/event/button", lambda p, r: json.loads(p)["action"] == w[1]
                                       and json.loads(p)["button"] == w[0])
        assert not retained, "button events must not be retained"
    ws.close()
    assert ISO.fullmatch(d.get_json("/api/v1/state")["input"]["last_button"] or "")


def test_touch_not_on_mqtt(d, obs):
    ws, _ = d.stream()
    obs.drain()
    d.write_touch(record(EV_ABS, ABS_MT_SLOT, 0), record(EV_ABS, ABS_MT_TRACKING_ID, 200),
                  record(EV_ABS, ABS_MT_POSITION_X, 5), record(EV_ABS, ABS_MT_POSITION_Y, 5), syn(),
                  record(EV_ABS, ABS_MT_TRACKING_ID, -1), syn())
    assert [e["action"] for e in messages(ws, 2, "touch")] == ["down", "up"]
    ws.close()
    time.sleep(1.5)
    for topic, payload, _ in obs.drain():
        assert "touch" not in topic, f"touch reached MQTT: {topic}"
        try:
            doc = json.loads(payload)
        except ValueError:
            continue
        assert not (isinstance(doc, dict) and doc.get("type") == "touch"), f"touch event on {topic}: {payload}"


def test_slow_client(d):
    """A client that never reads is dropped; frames and a reading client carry on."""
    slow = EventStream("127.0.0.1", d.port, d.token(), rcvbuf=4096)
    fast, _ = d.stream()
    wait_for("both clients", lambda: d.get_json("/api/v1/state")["input"]["event_clients"] == 2)
    seen, stop = [], threading.Event()

    def reader():
        while True:
            msg = fast.next_message()
            if msg is None:
                return
            doc = json.loads(msg)
            seen.append(doc)
            if doc.get("button") == "volume_down":
                return

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    pairs = 1500

    def flood():
        for _ in range(pairs // 100):
            recs = []
            for _ in range(100):
                recs += [record(EV_KEY, KEY_POWER, 1), syn(), record(EV_KEY, KEY_POWER, 0), syn()]
            d.write_keys(*recs)
        d.write_keys(record(EV_KEY, KEY_VOLUMEDOWN, 1), syn())  # the end marker
        stop.set()

    f = threading.Thread(target=flood, daemon=True)
    f.start()
    n = 0
    while not stop.is_set() or n < 3:
        t0 = time.monotonic()
        put_frame(d, f"during-flood-{n}")
        assert time.monotonic() - t0 < 5, "a frame PUT took over 5 s during the flood"
        n += 1
    f.join(60)
    t.join(60)
    assert not t.is_alive(), f"the reading client stopped after {len(seen)} messages"
    buttons = [e for e in seen if e.get("type") == "button"]
    assert len(buttons) == 2 * pairs + 1, f"the reading client got {len(buttons)} of {2 * pairs + 1} button events"
    state = wait_for("the slow client to be dropped",
                     lambda: (s := d.get_json("/api/v1/state")["input"])["event_clients_dropped_slow"] == 1 and s)
    assert state["event_clients"] == 1, state
    assert "dropped client" in d.log() and "reads too slowly" in d.log()
    fast.close()
    slow.sock.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--daemon", required=True, help="host-built tt7d binary")
    args = ap.parse_args()
    binary = os.path.abspath(args.daemon)
    events_tool = os.path.join(ROOT, "tools", "events.py")

    with tempfile.TemporaryDirectory(prefix="tt7d-input-") as workdir:
        broker = Broker(workdir, "broker")
        d = Panel(binary, workdir)
        steps, obs = [], None
        try:
            broker.start()
            obs = Observer(broker)
            # Never the host's NTP marker: the fallback clock shows "Setting clock" either way.
            ntp_marker = ["--ntp-marker", os.path.join(workdir, "ntp-synced")]
            d.extra_args = ["--mqtt-host", "127.0.0.1", "--mqtt-port", str(broker.port)] + ntp_marker
            d.start()
            base = f"tt7/{d.get_json('/api/v1/info')['device_id']}"
            wait_for("MQTT connected", lambda: d.get_json("/api/v1/state")["mqtt"]["connected"])
            steps.append("/info lists the touch range and buttons; /state input; the log names the devices")
            test_info_and_state(d)
            steps.append("touches on the fallback clock carry its frame_id; MQTT state last_touch")
            test_touch_on_fallback_clock(d, obs, base)
            steps.append("WebSocket auth: 401 without or with a wrong token, ?token= works, 426, 405")
            test_auth(d)
            steps.append("rotation 90: down/move/up order, logical coords, throttle, frame_id (+ tools/events.py)")
            test_touch(d, 90, events_tool)
            steps.append("buttons on the WebSocket and on MQTT event/button (not retained)")
            test_buttons_and_mqtt(d, obs, base)
            steps.append("touch does not reach MQTT")
            test_touch_not_on_mqtt(d, obs)
            steps.append("a slow client is dropped while frame PUTs and a reading client carry on")
            test_slow_client(d)
            d.stop()
            d.extra_args = ["--rotation", "270"] + ntp_marker
            d.start()
            steps.append("rotation 270: the same touches map to the other logical corners")
            test_touch(d, 270, None)
            log = d.log()
            steps.append("the token (header or ?token=) never reaches the log")
            assert d.token() not in log, "the token appeared in the log"
        except Exception as e:  # noqa: BLE001 - report which step failed, with the logs
            print(f"FAIL test_input_e2e: {steps[-1] if steps else 'start'}: {type(e).__name__}: {e}", file=sys.stderr)
            d.stop()
            for path in (d.log_path, broker.log_path):
                if os.path.exists(path):
                    with open(path, "rb") as f:
                        tail = f.read().decode("utf-8", "replace").splitlines()[-25:]
                    print(f"---- {os.path.basename(path)} (last 25 lines) ----", *tail, sep="\n", file=sys.stderr)
            return 1
        finally:
            if obs:
                obs.close()
            d.stop()
            broker.stop()
        bad = [line for line in d.log().splitlines() if re.search(r"AddressSanitizer|runtime error|LeakSanitizer", line)]
        if bad:
            print("FAIL test_input_e2e: sanitizer findings in the tt7d log:", *bad[:10], sep="\n", file=sys.stderr)
            return 1
        for s in steps:
            print(f"  ok   input: {s}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
