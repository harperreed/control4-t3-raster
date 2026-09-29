#!/usr/bin/env python3
# ABOUTME: MQTT integration test: the real host-built tt7d against real amqtt brokers, watched by a real paho client.
# ABOUTME: Covers availability/LWT, state, HA discovery, commands, reconnects, runtime broker switching and secrets.
"""Usage: uv run --no-project --with amqtt==0.12.1 --with paho-mqtt==2.1.0 \\
          python tt7d/test_mqtt_e2e.py --daemon build/host/tt7d      (run by `make test-mqtt`)

Nothing is mocked. The brokers are amqtt (a pure-Python MQTT 3.1.1 broker)
processes on loopback ports; the observer is paho-mqtt; tt7d is the same
source as the panel build, on a file-backed framebuffer with the panel's
sysfs fixture, as in test_e2e.py.
"""
import argparse
import http.client
import json
import os
import queue
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

import paho.mqtt.client as mqtt
from pwdlib.hashers.argon2 import Argon2Hasher

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import test_e2e  # noqa: E402 - the frame helpers and the Daemon wrapper

PASSWORD = "c0rrect-h0rse \"battery\" staple\\"
USER = "tt7"
STATE_KEYS = {"time", "uptime_s", "battery_percent", "battery_estimate", "charging", "external_power", "brightness",
              "display_on", "wifi_ip", "ethernet_ip", "frame_id", "frame_age_s", "fallback", "last_touch"}


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(what, cond, timeout=15.0, step=0.05):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = cond()
        if value:
            return value
        time.sleep(step)
    raise AssertionError(f"timed out after {timeout:.0f} s waiting for {what}")


class Broker:
    """An amqtt broker process on 127.0.0.1:<port>, optionally requiring a user and password."""

    def __init__(self, workdir, name, users=None):
        self.port = free_port()
        self.name = name
        self.config = os.path.join(workdir, f"{name}.yaml")
        self.log_path = os.path.join(workdir, f"{name}.log")
        lines = ["listeners:", "  default:", "    type: tcp", f"    bind: 127.0.0.1:{self.port}", "plugins:"]
        if users:
            pwfile = os.path.join(workdir, f"{name}.passwd")
            with open(pwfile, "w") as f:
                for user, password in users.items():
                    f.write(f"{user}:{Argon2Hasher().hash(password)}\n")
            lines += ["  amqtt.plugins.authentication.FileAuthPlugin:", f"    password_file: {pwfile}"]
        else:
            lines += ["  amqtt.plugins.authentication.AnonymousAuthPlugin:", "    allow_anonymous: true"]
        with open(self.config, "w") as f:
            f.write("\n".join(lines) + "\n")
        self.proc = None

    def start(self):
        log = open(self.log_path, "ab")
        self.proc = subprocess.Popen([sys.executable, "-m", "amqtt.scripts.broker_script", "-c", self.config],
                                     stdout=log, stderr=subprocess.STDOUT)
        def listening():
            if self.proc.poll() is not None:
                raise AssertionError(f"broker {self.name} exited: see {self.log_path}")
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return True
            except OSError:
                return False
        wait_for(f"broker {self.name} to listen", listening, timeout=30)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc = None


class Observer:
    """A paho client subscribed to everything, collecting messages in order."""

    def __init__(self, broker, user=None, password=None, name="observer"):
        self.messages = queue.Queue()
        self.connected = False
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"{name}-{os.getpid()}")
        if user:
            self.c.username_pw_set(user, password)
        self.c.on_message = lambda c, u, m: self.messages.put((m.topic, m.payload.decode("utf-8", "replace"),
                                                               bool(m.retain)))
        self.c.on_connect = self._on_connect
        self.c.on_disconnect = lambda *a: setattr(self, "connected", False)
        self.c.reconnect_delay_set(0.2, 1)
        self.c.connect_async("127.0.0.1", broker.port, keepalive=10)
        self.c.loop_start()
        wait_for(f"observer on {broker.name}", lambda: self.connected)

    def _on_connect(self, c, userdata, flags, reason, props):
        if not reason.is_failure:
            c.subscribe([("tt7/#", 0), ("homeassistant/#", 0)])
            self.connected = True

    def drain(self):
        out = []
        while True:
            try:
                out.append(self.messages.get_nowait())
            except queue.Empty:
                return out

    def expect(self, topic, pred=lambda payload, retain: True, timeout=15.0):
        """The next message on topic that satisfies pred (other messages are skipped)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                t, payload, retain = self.messages.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if t == topic and pred(payload, retain):
                return payload, retain
        raise AssertionError(f"no matching message on {topic} within {timeout:.0f} s")

    def publish(self, topic, payload, retain=False):
        self.c.publish(topic, payload, qos=0, retain=retain).wait_for_publish(5)

    def close(self):
        self.c.loop_stop()
        self.c.disconnect()


def retained_snapshot(broker, user=None, password=None, until=None, settle=1.5, timeout=15.0):
    """The retained messages the broker hands a brand-new subscriber: {topic: payload}.

    With `until`, collects until until(snapshot) is true (or timeout), then
    0.5 s more so the rest of the burst arrives too; without it, for
    `settle` seconds."""
    o = Observer(broker, user, password, name="snapshot")
    snap = {}
    deadline = time.monotonic() + (timeout if until else settle)
    while time.monotonic() < deadline:
        snap.update({t: p for t, p, retain in o.drain() if retain})
        if until and until(snap):
            until = None
            deadline = time.monotonic() + 0.5
        time.sleep(0.05)
    o.close()
    return snap


class Tt7d(test_e2e.Daemon):
    """test_e2e's daemon wrapper plus a hard kill and JSON helpers."""

    def kill(self):
        self.proc.kill()
        self.proc.wait()
        self.proc = None

    def auth(self):
        return {"Authorization": f"Bearer {self.token()}"}

    def get_json(self, path, headers=None):
        status, _, body = self.request("GET", path, headers=headers)
        assert status == 200, f"GET {path}: {status} {body[:300]!r}"
        return json.loads(body)

    def mqtt_state(self):
        return self.get_json("/api/v1/state")["mqtt"]

    def device_id(self):
        return self.get_json("/api/v1/info")["device_id"]

    def log(self):
        with open(self.log_path, "rb") as f:
            return f.read().decode("utf-8", "replace")


def put_config(d, doc, token=True):
    headers = {"Content-Type": "application/json"}
    if token:
        headers.update(d.auth())
    status, _, body = d.request("PUT", "/api/v1/config/mqtt", body=json.dumps(doc).encode(), headers=headers)
    return status, json.loads(body)


def frame(seed):
    return test_e2e.png_bytes(test_e2e.LOGICAL_W, test_e2e.LOGICAL_H, test_e2e.random_image(seed))


# ---- tests ------------------------------------------------------------------

def test_connect_state_and_boot(d, obs, base):
    obs.expect(f"{base}/availability", lambda p, r: p == "online")  # retention: see the snapshot below
    obs.expect(f"{base}/event/boot", lambda p, r: json.loads(p)["type"] == "boot" and not r)
    state, _ = obs.expect(f"{base}/state")
    doc = json.loads(state)
    assert set(doc) == STATE_KEYS, f"state keys {sorted(doc)}"
    assert doc["battery_percent"] == 82 and doc["battery_estimate"] is True, doc
    assert doc["brightness"] == 50, doc
    # No frame yet, so the fallback clock is what the screen shows (SPEC 41.1).
    assert doc["frame_id"].startswith("fallback-clock-") and doc["frame_age_s"] >= 0, doc
    fb = doc["fallback"]
    assert fb["active"] is True and fb["reason"] == "no_frame_since_boot" and fb["timeout_s"] == 300, fb
    assert fb["since"].endswith("Z"), fb
    assert doc["charging"] is False and doc["last_touch"] is None, doc

    # Everything tt7d announces at connect, discovery included.
    snap = retained_snapshot(obs.broker, until=lambda s: s.get(f"{base}/availability") == "online"
                             and f"{base}/state" in s and f"{base}/sensor/battery_percent" in s
                             and sum(t.startswith("homeassistant/") for t in s) >= 6)
    assert snap.get(f"{base}/availability") == "online", "availability 'online' is not retained"
    assert set(json.loads(snap[f"{base}/state"])) == STATE_KEYS, "state is not retained"
    assert snap.get(f"{base}/sensor/battery_percent") == "82", snap.get(f"{base}/sensor/battery_percent")
    assert not any("/event/" in t for t in snap), f"an event was retained: {[t for t in snap if '/event/' in t]}"

    m = d.mqtt_state()
    assert m["connected"] is True and m["broker"] == f"127.0.0.1:{obs.broker.port}", m
    assert m["last_publish"] and m["last_error"] is None and m["reconnects"] == 0, m
    return snap


def test_discovery(snap, device_id, base):
    def config(component, obj):
        raw = snap.get(f"homeassistant/{component}/{device_id}/{obj}/config")
        return json.loads(raw) if raw else None

    present = {("sensor", "battery"), ("binary_sensor", "charging"), ("sensor", "uptime"), ("sensor", "frame_age"),
               ("sensor", "wifi_ip"), ("number", "brightness")}
    # Absent: no Ethernet in the fixture; the read-only brightness sensor,
    # since the number entity sets it; reboot, since allow_reboot_cmd is off.
    absent = {("sensor", "ethernet_ip"), ("sensor", "brightness"), ("button", "reboot")}
    for component, obj in present:
        c = config(component, obj)
        assert c, f"no discovery config for {component}/{obj}"
        assert c["unique_id"] == f"{device_id}_{obj}", c
        assert c["availability_topic"] == f"{base}/availability", c
        assert c["state_topic"] == f"{base}/state" and "value_json" in c["value_template"], c
        dev = c["device"]
        assert dev["identifiers"] == [device_id] and dev["model"] == "C4-TT7", dev
        assert dev["manufacturer"] == "Control4 (repurposed)" and dev["sw_version"] and dev["name"], dev
    for component, obj in absent:
        assert config(component, obj) is None, f"{component}/{obj} advertised without the capability"
    assert config("sensor", "battery")["device_class"] == "battery"
    assert "estimate" in config("sensor", "battery")["name"].lower()
    assert config("binary_sensor", "charging")["device_class"] == "battery_charging"
    assert config("sensor", "uptime")["unit_of_measurement"] == "s"
    number = config("number", "brightness")
    assert number["command_topic"] == f"{base}/cmd/brightness" and number["max"] == 100, number
    for obj in ("wake", "blank"):
        c = config("button", obj)
        assert c and c["command_topic"] == f"{base}/cmd/{obj}" and c["unique_id"] == f"{device_id}_{obj}", (obj, c)


def test_frames_and_commands(d, obs, base):
    obs.drain()
    status, _, body = d.put_frame(frame(11), **{"X-Frame-ID": "mqtt-1"})
    assert status == 200, body
    ev, retain = obs.expect(f"{base}/event/frame")
    doc = json.loads(ev)
    assert doc["frame_id"] == "mqtt-1" and doc["deduplicated"] is False and not retain, (doc, retain)
    state, _ = obs.expect(f"{base}/state", lambda p, r: json.loads(p)["frame_id"] == "mqtt-1", timeout=5)
    assert json.loads(state)["frame_age_s"] < 5
    assert json.loads(state)["fallback"]["active"] is False, "a frame ends the fallback clock"

    def error_for(cmd, payload, retain=False):
        obs.drain()
        obs.publish(f"{base}/cmd/{cmd}", payload, retain=retain)
        p, _ = obs.expect(f"{base}/event/error", lambda p, r: json.loads(p)["command"] == cmd, timeout=5)
        return json.loads(p)["error"]

    assert error_for("brightness", "bright") == "invalid_payload"
    assert error_for("brightness", "256") == "invalid_payload"  # raw above max_brightness
    assert error_for("reboot", "PRESS") == "command_disabled"
    time.sleep(1.5)  # the reboot command would have run after 1 s
    assert not os.path.exists(d.reboot_marker), "cmd/reboot ran with allow_reboot_cmd off"

    # A retained command is only taken live. The broker hands it over again,
    # flagged retained, on every new subscription (MQTT 3.1.1 section
    # 3.3.1.3), and tt7d must not act on that replay: for cmd/reboot it
    # would mean a reboot loop.
    assert error_for("reboot", "PRESS", retain=True) == "command_disabled"
    d.stop()
    obs.drain()
    d.start()
    obs.expect(f"{base}/availability", lambda p, r: p == "online")
    time.sleep(1.5)
    replies = [m for m in obs.drain() if m[0] == f"{base}/event/error"]
    obs.publish(f"{base}/cmd/reboot", "", retain=True)  # clear it from the broker
    assert not replies, f"tt7d acted on a replayed retained command: {replies}"


# How long tt7d may take to publish a state change: it compares state every
# second (STATE_CHECK_MS in mqtt.c); commands and HTTP display actions ask
# for an immediate check. The slack covers loopback and the ASan build.
CHANGE_WINDOW_S = 1.5


def backlight(d, attr="brightness"):
    with open(os.path.join(d.backlight, attr)) as f:
        return int(f.read().strip())


def test_display_commands(d, obs, base):
    def state_where(pred):
        obs.expect(f"{base}/state", lambda p, r: pred(json.loads(p)), timeout=CHANGE_WINDOW_S)

    def command(cmd, payload):
        obs.drain()
        obs.publish(f"{base}/cmd/{cmd}", payload)

    # cmd/brightness as a percentage and as a raw level both reach the backlight file.
    command("brightness", "40%")
    wait_for("cmd/brightness 40% on the backlight", lambda: backlight(d) == 102, timeout=5)  # 40 % of 255
    state_where(lambda s: s["brightness"] == 40 and s["display_on"] is True)
    command("brightness", "200")
    wait_for("cmd/brightness 200 on the backlight", lambda: backlight(d) == 200, timeout=5)  # raw, not rounded
    state_where(lambda s: s["brightness"] == 78)

    # HTTP changes show up in MQTT state within the change window.
    obs.drain()
    status, _, body = d.request("PUT", "/api/v1/display/brightness", body=b'{"value": 30, "unit": "percent"}',
                                headers={**d.auth(), "Content-Type": "application/json"})
    assert status == 200, body
    assert backlight(d) == 77, backlight(d)
    state_where(lambda s: s["brightness"] == 30)
    status, _, body = d.request("POST", "/api/v1/display/blank", body=b"", headers=d.auth())
    assert status == 200, body
    state_where(lambda s: s["display_on"] is False)
    status, _, body = d.request("POST", "/api/v1/display/wake", body=b"", headers=d.auth())
    assert status == 200, body
    state_where(lambda s: s["display_on"] is True and s["brightness"] == 30)

    # cmd/blank powers the backlight down with bl_power (never brightness 0,
    # which is bright on rk28_bl); cmd/wake powers it back up at the level.
    command("blank", "PRESS")
    wait_for("cmd/blank on bl_power", lambda: backlight(d, "bl_power") == 4, timeout=5)
    assert backlight(d) == 77, backlight(d)
    state_where(lambda s: s["display_on"] is False and s["brightness"] == 30)
    # A brightness while blank is stored for wake and keeps the screen dark.
    command("brightness", "50%")
    state_where(lambda s: s["display_on"] is False and s["brightness"] == 50)
    assert backlight(d, "bl_power") == 4 and backlight(d) == 77, (backlight(d, "bl_power"), backlight(d))
    command("wake", "PRESS")
    wait_for("cmd/wake on bl_power", lambda: backlight(d, "bl_power") == 0, timeout=5)
    assert backlight(d) == 128, backlight(d)
    state_where(lambda s: s["display_on"] is True and s["brightness"] == 50)
    # cmd/brightness 0 (raw or percent) becomes 1, never 0.
    command("brightness", "0")
    wait_for("cmd/brightness 0 becomes 1", lambda: backlight(d) == 1, timeout=5)
    command("brightness", "30%")
    wait_for("cmd/brightness 30%", lambda: backlight(d) == 77, timeout=5)
    command("brightness", "0%")
    wait_for("cmd/brightness 0% becomes 1", lambda: backlight(d) == 1, timeout=5)
    errors = [m for m in obs.drain() if m[0] == f"{base}/event/error"]
    assert not errors, errors


def test_reboot_allowed(d, obs, base):
    device_id = d.device_id()
    reboot_config = f"homeassistant/button/{device_id}/reboot/config"
    assert not os.path.exists(d.reboot_marker)
    status, doc = put_config(d, {"allow_reboot_cmd": True})
    assert status == 200 and doc["allow_reboot_cmd"] is True, doc
    obs.expect(reboot_config, lambda p, r: p and json.loads(p)["command_topic"] == f"{base}/cmd/reboot", timeout=10)
    wait_for("reconnect after the change", lambda: d.mqtt_state()["connected"])
    obs.drain()
    obs.publish(f"{base}/cmd/reboot", "PRESS")
    wait_for("the --reboot-cmd marker", lambda: os.path.exists(d.reboot_marker), timeout=5)
    errors = [m for m in obs.drain() if m[0] == f"{base}/event/error"]
    assert not errors, errors
    os.unlink(d.reboot_marker)

    # Off again: the Reboot button leaves Home Assistant, and the command is refused.
    status, doc = put_config(d, {"allow_reboot_cmd": False})
    assert status == 200 and doc["allow_reboot_cmd"] is False, doc
    obs.expect(reboot_config, lambda p, r: p == "", timeout=10)
    wait_for("reconnect after the change", lambda: d.mqtt_state()["connected"])
    obs.drain()
    obs.publish(f"{base}/cmd/reboot", "PRESS")
    p, _ = obs.expect(f"{base}/event/error", lambda p, r: json.loads(p)["command"] == "reboot", timeout=5)
    assert json.loads(p)["error"] == "command_disabled", p
    time.sleep(1.5)
    assert not os.path.exists(d.reboot_marker), "cmd/reboot ran after allow_reboot_cmd went back off"


def test_lwt(d, obs, base):
    obs.drain()
    d.kill()
    payload, _ = obs.expect(f"{base}/availability", lambda p, r: p == "offline", timeout=10)
    snap = retained_snapshot(obs.broker, until=lambda s: s.get(f"{base}/availability") == "offline")
    assert snap.get(f"{base}/availability") == "offline", "the will is not retained"
    d.start()
    obs.expect(f"{base}/availability", lambda p, r: p == "online")


def test_broker_down_and_back(d, obs, broker, base):
    broker.stop()
    wait_for("tt7d to notice the broker is gone", lambda: not d.mqtt_state()["connected"])
    for seed in (21, 22, 23):  # the display keeps working, and quickly
        pixels = test_e2e.random_image(seed)
        png = test_e2e.png_bytes(test_e2e.LOGICAL_W, test_e2e.LOGICAL_H, pixels)
        t0 = time.monotonic()
        status, _, body = d.put_frame(png)
        took = time.monotonic() - t0
        assert status == 200, body
        assert took < 5, f"frame PUT took {took:.1f} s with the broker down"
        assert d.fb_bytes() == test_e2e.expected_fb(pixels), "frame not drawn with the broker down"
    m = d.mqtt_state()
    assert m["connected"] is False and m["last_error"] and m["dropped"] >= 3, m
    broker.start()
    wait_for("tt7d to reconnect", lambda: d.mqtt_state()["connected"], timeout=40)
    assert d.mqtt_state()["reconnects"] >= 1
    obs.expect(f"{base}/availability", lambda p, r: p == "online", timeout=20)
    snap = retained_snapshot(broker, until=lambda s: s.get(f"{base}/availability") == "online" and f"{base}/state" in s)
    assert snap.get(f"{base}/availability") == "online" and f"{base}/state" in snap, f"not re-announced: {snap}"


def test_config_api(d, broker_a, broker_b, workdir, base):
    # Authentication and validation.
    for headers in ({}, {"Authorization": "Bearer nope"}):
        status, _, body = d.request("GET", "/api/v1/config/mqtt", headers=headers)
        assert status == 401 and json.loads(body)["error"] == "unauthorized", (status, body)
    status, doc = put_config(d, {"host": "127.0.0.1"}, token=False)
    assert status == 401, doc
    status, doc = put_config(d, {"host": "broker.example.com"})
    assert status == 400 and doc["error"] == "invalid_config" and doc["field"] == "host", doc
    status, doc = put_config(d, {"telemetry_interval": 5})
    assert status == 409 and doc["error"] == "set_by_flag" and doc["field"] == "telemetry_interval", doc
    status, _, body = d.request("PUT", "/api/v1/config/mqtt", body=b"{}", headers={**d.auth(), "Content-Type": "text/plain"})
    assert status == 415, body

    before = d.get_json("/api/v1/config/mqtt", d.auth())
    assert before["set_by_flags"] == ["telemetry_interval"] and before["telemetry_interval"] == 2, before
    assert before["password_set"] is False and "password" not in before, before

    # A wrong password: refused by the broker, and said so in /state.
    status, doc = put_config(d, {"host": "127.0.0.1", "port": broker_b.port, "username": USER, "password": "wrong"})
    assert status == 200 and doc["config_revision"] == before["config_revision"] + 1, doc
    wait_for("the broker to refuse the wrong password",
             lambda: "refused" in (d.mqtt_state()["last_error"] or ""), timeout=10)

    # The right one, through tools/mqtt-setup.sh, with curl's argv recorded.
    obs_a = Observer(broker_a, name="obs-a")
    obs_b = Observer(broker_b, USER, PASSWORD, name="obs-b")
    pwfile = os.path.join(workdir, "mqtt-password")
    with open(pwfile, "w") as f:
        f.write(PASSWORD + "\n")
    token_file = os.path.join(workdir, "token")
    with open(token_file, "w") as f:
        f.write(d.token() + "\n")
    shim = os.path.join(workdir, "shim")
    os.makedirs(shim, exist_ok=True)
    argv_log = os.path.join(workdir, "curl-argv.log")
    real_curl = subprocess.run(["sh", "-c", "command -v curl"], capture_output=True, text=True).stdout.strip()
    with open(os.path.join(shim, "curl"), "w") as f:
        f.write(f'#!/bin/sh\nprintf "%s\\n" "$*" >> "{argv_log}"\nexec "{real_curl}" "$@"\n')
    os.chmod(os.path.join(shim, "curl"), 0o755)
    out = subprocess.run([os.path.join(ROOT, "tools", "mqtt-setup.sh"), f"127.0.0.1:{d.port}",
                          "--broker", f"127.0.0.1:{broker_b.port}", "--user", USER, "--password-file", pwfile],
                         env={**os.environ, "TT7_TOKEN_FILE": token_file, "PATH": shim + ":" + os.environ["PATH"]},
                         capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, f"mqtt-setup.sh failed: {out.stdout} {out.stderr}"
    doc = json.loads(out.stdout)
    assert doc["host"] == "127.0.0.1" and doc["port"] == broker_b.port and doc["password_set"] is True, doc
    assert doc["username"] == USER and doc["ha_discovery"] is True and doc["enabled"] is True, doc

    obs_b.expect(f"{base}/availability", lambda p, r: p == "online", timeout=20)
    wait_for("tt7d on broker B", lambda: d.mqtt_state()["broker"] == f"127.0.0.1:{broker_b.port}"
             and d.mqtt_state()["connected"])
    snap_b = retained_snapshot(broker_b, USER, PASSWORD, until=lambda s: any(t.startswith("homeassistant/sensor/") for t in s))
    assert any(t.startswith("homeassistant/sensor/") for t in snap_b), "no discovery on the new broker"
    # The old broker was told: offline, and no discovery left behind.
    snap_a = retained_snapshot(broker_a, until=lambda s: s.get(f"{base}/availability") == "offline")
    assert snap_a.get(f"{base}/availability") == "offline", snap_a
    assert not any(t.startswith("homeassistant/") for t in snap_a), [t for t in snap_a if t.startswith("homeassistant/")]
    obs_a.close()

    # Secrets: never in GET, the log, argv, or mqtt.conf; the password file is 0600.
    got = d.request("GET", "/api/v1/config/mqtt", headers=d.auth())[2].decode()
    state = d.request("GET", "/api/v1/state")[2].decode()
    with open(os.path.join(d.data, "mqtt.conf")) as f:
        conf = f.read()
    with open(argv_log) as f:
        argv = f.read()
    with open(f"/proc/{d.proc.pid}/cmdline", "rb") as f:
        cmdline = f.read().decode("utf-8", "replace")
    for where, text in (("GET config", got), ("GET state", state), ("tt7d log", d.log()), ("mqtt.conf", conf),
                        ("curl argv", argv), ("tt7d argv", cmdline)):
        assert PASSWORD not in text and "c0rrect" not in text, f"the password appears in {where}"
    assert d.token() not in argv, "the token appears in curl's argv"
    assert argv.strip(), "the curl shim recorded nothing"
    assert os.stat(os.path.join(d.data, "mqtt-password")).st_mode & 0o777 == 0o600
    assert "host=127.0.0.1" in conf and f"port={broker_b.port}" in conf and "telemetry_interval=10" in conf, conf

    # HA discovery off: the entities are removed from broker B.
    status, doc = put_config(d, {"ha_discovery": False})
    assert status == 200 and doc["ha_discovery"] is False, doc
    obs_b.expect(f"homeassistant/sensor/{d.device_id()}/battery/config", lambda p, r: p == "", timeout=10)
    wait_for("reconnect after the change", lambda: d.mqtt_state()["connected"])
    snap_b = retained_snapshot(broker_b, USER, PASSWORD)
    assert not any(t.startswith("homeassistant/") for t in snap_b), [t for t in snap_b if t.startswith("homeassistant/")]
    obs_b.close()

    # And it all survives a restart: mqtt.conf + password file are read back.
    d.stop()
    d.start()
    wait_for("reconnect after restart", lambda: d.mqtt_state()["connected"], timeout=20)
    assert d.mqtt_state()["broker"] == f"127.0.0.1:{broker_b.port}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--daemon", required=True, help="host-built tt7d binary")
    args = ap.parse_args()
    binary = os.path.abspath(args.daemon)

    with tempfile.TemporaryDirectory(prefix="tt7d-mqtt-") as workdir:
        broker_a = Broker(workdir, "broker-a")
        broker_b = Broker(workdir, "broker-b", users={USER: PASSWORD})
        d = Tt7d(binary, workdir, extra_args=["--mqtt-telemetry-interval", "2"])
        os.makedirs(d.data, exist_ok=True)
        with open(os.path.join(d.data, "mqtt.conf"), "w") as f:
            f.write(f"# written by the test\nhost=127.0.0.1\nport={broker_a.port}\nkeepalive=5\n")
        steps, obs = [], None
        try:
            broker_a.start()
            broker_b.start()
            obs = Observer(broker_a)
            obs.broker = broker_a
            d.start()
            base = f"tt7/{d.device_id()}"
            steps.append("availability online (retained), boot event, state JSON, /state mqtt")
            snap = test_connect_state_and_boot(d, obs, base)
            steps.append("HA discovery only for available capabilities")
            test_discovery(snap, d.device_id(), base)
            steps.append("event/frame, state on change, commands, retained commands ignored")
            test_frames_and_commands(d, obs, base)
            steps.append("cmd/brightness (percent, raw), cmd/blank, cmd/wake on the backlight; HTTP changes in state")
            test_display_commands(d, obs, base)
            steps.append("allow_reboot_cmd: HA Reboot button, cmd/reboot runs --reboot-cmd; off again: refused")
            test_reboot_allowed(d, obs, base)
            steps.append("kill tt7d: the broker delivers the retained LWT offline")
            test_lwt(d, obs, base)
            steps.append("broker down: frames still shown; broker back: tt7d reconnects")
            test_broker_down_and_back(d, obs, broker_a, base)
            steps.append("PUT /api/v1/config/mqtt: auth, validation, switch brokers, secrets stay secret")
            test_config_api(d, broker_a, broker_b, workdir, base)
        except Exception as e:  # noqa: BLE001 - report which step failed, with the logs
            print(f"FAIL test_mqtt_e2e: {steps[-1] if steps else 'start'}: {type(e).__name__}: {e}", file=sys.stderr)
            if d.proc:
                d.stop()
            for path in (d.log_path, broker_a.log_path, broker_b.log_path):
                if os.path.exists(path):
                    with open(path, "rb") as f:
                        tail = f.read().decode("utf-8", "replace").splitlines()[-25:]
                    print(f"---- {os.path.basename(path)} (last 25 lines) ----", *tail, sep="\n", file=sys.stderr)
            return 1
        finally:
            if obs:
                obs.close()
            d.stop()
            broker_a.stop()
            broker_b.stop()
        log = d.log()
        bad = [line for line in log.splitlines() if re.search(r"AddressSanitizer|runtime error|LeakSanitizer", line)]
        if bad:
            print("FAIL test_mqtt_e2e: sanitizer findings in the tt7d log:", *bad[:10], sep="\n", file=sys.stderr)
            return 1
        for s in steps:
            print(f"  ok   mqtt: {s}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
