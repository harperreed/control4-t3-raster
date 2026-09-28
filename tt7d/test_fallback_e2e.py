#!/usr/bin/env python3
# ABOUTME: End-to-end test of the fallback clock (SPEC 41.1): the real host-built tt7d on a file-backed framebuffer,
# ABOUTME: driven over HTTP through boot, NTP sync marker, frames, heartbeats, timeouts and a restart.
"""Usage: tt7d/test_fallback_e2e.py --daemon build/host/tt7d   (run by `make test-e2e`)

Nothing is mocked. The NTP sync marker is written by the image's real ntpd -S hook
(probe/tt7-ntp-hook.sh), run as ntpd runs it. The fallback timeout is 3 s so the test
can watch it trip. The expected framebuffer contents are computed here from
logical pixels (test_e2e.expected_fb), never by asking the daemon.
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_e2e as e2e  # noqa: E402 - the Daemon wrapper and frame helpers

TIMEOUT_S = 3
HOOK = os.path.join(os.path.dirname(e2e.HERE), "probe", "tt7-ntp-hook.sh")


def state(d):
    return e2e.jbody(*d.request("GET", "/api/v1/state")[::2], 200)


def wait_for(what, pred, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return
        time.sleep(0.1)
    raise AssertionError(f"timed out after {timeout} s waiting for: {what}")


def heartbeat(d, token=True):
    return d.api("POST", "/api/v1/heartbeat", token=token, body=b"")


def preview_matches_screen(d):
    """GET /frame and /frame/image describe the clock face, and the image is exactly what the fb shows."""
    meta = e2e.jbody(*d.request("GET", "/api/v1/frame")[::2], 200)
    assert re.fullmatch(r"fallback-clock-\d+", meta["frame_id"]), meta
    assert meta["persisted"] is False and meta["received_at"] is None and meta["content_type"] == "image/png", meta
    status, headers, image = d.request("GET", "/api/v1/frame/image")
    assert status == 200 and headers["content-type"] == "image/png", (status, headers)
    assert hashlib.sha256(image).hexdigest() == meta["sha256"] and len(image) == meta["bytes"], meta
    pixels, channels = e2e.unfiltered_png_pixels(image)
    assert d.fb_bytes() == e2e.expected_fb(pixels, channels), "the preview PNG is not what the framebuffer shows"
    return meta


def test_boot_setting_clock(d):
    wait_for("the fallback clock on the framebuffer", lambda: d.fb_bytes() != bytes(e2e.FB_BYTES), 5)
    s = state(d)
    fb = s["fallback"]
    assert fb["active"] is True and fb["reason"] == "no_frame_since_boot" and fb["timeout_s"] == TIMEOUT_S, fb
    assert re.fullmatch(r"\d{4}-\d\d-\d\dT.*Z", fb["since"]), fb
    assert s["clock"] == {"synced": False, "synced_at": None, "timezone": "CST6CDT,M3.2.0,M11.1.0",
                          "format": "24h"}, s["clock"]
    assert s["display"]["frame_id"].startswith("fallback-clock-"), s["display"]
    assert s["frames"]["accepted"] == 0, "the fallback clock is not a server frame"
    preview_matches_screen(d)
    return d.fb_bytes()


def ntp_hook(marker, log, action, stratum):
    """Run the image's real ntpd -S hook the way BusyBox ntpd does: action in argv, stratum/offset in the env."""
    env = dict(os.environ, TT7_NTP_MARKER=marker, TT7_NTP_LOG=log, stratum=str(stratum), offset="1.5e9",
               freq_drift_ppm="0", poll_interval="32")
    subprocess.run(["sh", HOOK, action], env=env, check=True, timeout=10)


def test_sync_marker_shows_the_clock(d, marker, unsynced_fb):
    log = os.path.join(os.path.dirname(marker), "ntp.log")
    ntp_hook(marker, log, "periodic", 16)
    ntp_hook(marker, log, "unsync", 16)
    assert not os.path.exists(marker), "stratum 16 without a step is not synchronized"
    time.sleep(6)  # longer than tt7d's 5 s check: it must still say "Setting clock"
    assert d.fb_bytes() == unsynced_fb and state(d)["clock"]["synced"] is False
    ntp_hook(marker, log, "step", 16)  # ntpd resets its stratum to 16 just before a step's hook
    with open(marker) as f:
        assert re.fullmatch(r"synced \d+ step stratum=16 offset=1.5e9\n", f.read())
    with open(log) as f:
        assert [line.split()[2] for line in f] == ["unsync", "step"], "the hook logs only rare events"
    wait_for("a redraw once the clock is synchronized", lambda: d.fb_bytes() != unsynced_fb, 8)
    s = state(d)
    assert s["clock"]["synced"] is True and s["clock"]["synced_at"].endswith("Z"), s["clock"]
    assert s["fallback"]["active"] is True, s["fallback"]
    preview_matches_screen(d)


def test_heartbeat_during_fallback(d):
    e2e.check_error(heartbeat(d, token=False), 401, "unauthorized")
    e2e.check_error(d.request("GET", "/api/v1/heartbeat"), 405, "method_not_allowed")
    before = d.fb_bytes()
    doc = e2e.jbody(*heartbeat(d)[::2], 200)
    assert doc["fallback"]["active"] is True and doc["fallback"]["reason"] == "no_frame_since_boot", doc
    assert d.fb_bytes() == before, "a heartbeat alone has no frame to show"


def test_frame_replaces_the_clock(d):
    pixels = e2e.random_image(41)
    png = e2e.png_bytes(e2e.LOGICAL_W, e2e.LOGICAL_H, pixels)
    doc = e2e.jbody(*d.put_frame(png, **{"X-Frame-ID": "fb-1", "X-Persist": "true"})[::2], 200)
    assert doc["frame_id"] == "fb-1" and doc["persisted"] is True, doc
    assert d.fb_bytes() == e2e.expected_fb(pixels), "the frame did not replace the clock"
    s = state(d)
    assert s["fallback"] == {"active": False, "reason": None, "timeout_s": TIMEOUT_S, "since": None}, s["fallback"]
    assert s["display"]["frame_id"] == "fb-1", s["display"]
    return png, pixels


def test_heartbeats_keep_the_frame(d, expected):
    for _ in range(2 * TIMEOUT_S):
        e2e.jbody(*heartbeat(d)[::2], 200)
        time.sleep(1)
    assert d.fb_bytes() == expected, "heartbeats every second did not keep the frame up"
    assert state(d)["fallback"]["active"] is False


def test_duplicate_frames_keep_the_frame(d, png, expected):
    for _ in range(2 * TIMEOUT_S):
        doc = e2e.jbody(*d.put_frame(png, **{"X-Frame-ID": "fb-dup"})[::2], 200)
        assert doc["deduplicated"] is True, doc
        time.sleep(1)
    assert d.fb_bytes() == expected, "re-sent frames did not keep the frame up"


def test_timeout_brings_the_clock_back(d, expected):
    wait_for("the fallback clock after the timeout", lambda: d.fb_bytes() != expected, TIMEOUT_S + 3)
    s = state(d)
    assert s["fallback"]["active"] is True and s["fallback"]["reason"] == "server_timeout", s["fallback"]
    assert s["display"]["frame_id"].startswith("fallback-clock-"), s["display"]
    preview_matches_screen(d)


def test_patch_during_fallback(d):
    """The clock covers the frame, so it is no base for regions: 409 names the clock, and nothing changes."""
    before = d.fb_bytes()
    small = e2e.png_bytes(8, 8, e2e.random_image(42, w=8, h=8))
    doc = e2e.check_error(e2e.patch_frame(d, e2e.regions_body([(0, 0, 8, 8, small)]), "fb-dup"), 409, "base_mismatch")
    assert re.fullmatch(r"fallback-clock-\d+", doc["current_frame_id"]), doc
    assert d.fb_bytes() == before, "a PATCH drew over the fallback clock"
    assert state(d)["fallback"]["active"] is True, "a refused PATCH counted as a heartbeat"


def test_not_persisted(d, png):
    with open(os.path.join(d.data, "last-frame.png"), "rb") as f:
        assert f.read() == png, "the fallback clock replaced last-frame.png"
    with open(os.path.join(d.data, "last-frame.id")) as f:
        assert f.read().split() == [hashlib.sha256(png).hexdigest(), "fb-1"], "last-frame.id changed"


def test_duplicate_during_fallback_redraws(d, png, expected):
    doc = e2e.jbody(*d.put_frame(png, **{"X-Frame-ID": "fb-again"})[::2], 200)
    assert doc["deduplicated"] is False, "the clock covered the frame, so the same PNG must be drawn again"
    assert d.fb_bytes() == expected, "the re-sent frame did not replace the clock"
    assert state(d)["fallback"]["active"] is False


def test_restored_frame_then_clock(d, expected):
    d.stop()
    with open(d.fb, "wb") as f:
        f.write(bytes(e2e.FB_BYTES))
    d.extra_args += ["--clock-format", "12", "--tz", "UTC0"]
    d.start()
    assert d.fb_bytes() == expected, "the persisted frame was not restored"
    s = state(d)
    assert s["fallback"]["active"] is False, "a restored frame counts as a frame at boot"
    assert s["clock"]["format"] == "12h" and s["clock"]["timezone"] == "UTC0", s["clock"]
    wait_for("the clock after the restored frame's timeout", lambda: d.fb_bytes() != expected, TIMEOUT_S + 3)
    assert state(d)["fallback"]["reason"] == "server_timeout"


def test_bad_flags(binary):
    for args in (["--tz", "America/Chicago"], ["--clock-format", "13"], ["--fallback-timeout", "-1"]):
        r = subprocess.run([binary, *args], capture_output=True, timeout=10, check=False)
        assert r.returncode == 2 and b"bad option or value" in r.stderr, (args, r.returncode, r.stderr[-200:])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--daemon", required=True, help="host-built tt7d binary")
    binary = os.path.abspath(ap.parse_args().daemon)

    with tempfile.TemporaryDirectory(prefix="tt7d-fallback-e2e-") as workdir:
        marker = os.path.join(workdir, "ntp-synced")
        d = e2e.Daemon(binary, workdir, ["--fallback-timeout", str(TIMEOUT_S), "--ntp-marker", marker])
        steps = []
        try:
            steps.append("bad --tz, --clock-format, --fallback-timeout are refused")
            test_bad_flags(binary)
            d.start()
            steps.append("boot: fallback clock, 'setting clock' (no sync marker), preview = screen")
            unsynced = test_boot_setting_clock(d)
            steps.append("ntpd hook: stratum 16 is not synced; a step writes the marker; the clock shows the time")
            test_sync_marker_shows_the_clock(d, marker, unsynced)
            steps.append("heartbeat during the fallback: auth, stays on the clock")
            test_heartbeat_during_fallback(d)
            steps.append("a frame replaces the clock at once")
            png, pixels = test_frame_replaces_the_clock(d)
            expected = e2e.expected_fb(pixels)
            steps.append("heartbeats within the timeout keep the frame")
            test_heartbeats_keep_the_frame(d, expected)
            steps.append("re-sent (deduplicated) frames keep the frame")
            test_duplicate_frames_keep_the_frame(d, png, expected)
            steps.append("silence for the timeout brings the clock back")
            test_timeout_brings_the_clock_back(d, expected)
            steps.append("a PATCH during the fallback is refused with 409 base_mismatch")
            test_patch_during_fallback(d)
            steps.append("the fallback clock is never persisted")
            test_not_persisted(d, png)
            steps.append("the same frame again during the fallback is drawn again")
            test_duplicate_during_fallback_redraws(d, png, expected)
            steps.append("restart: restored frame for one timeout, then the clock; 12h and --tz flags")
            test_restored_frame_then_clock(d, expected)
        except Exception as e:  # noqa: BLE001 - report which step failed, with the daemon log
            print(f"FAIL test_fallback_e2e: {steps[-1] if steps else 'start'}: {type(e).__name__}: {e}",
                  file=sys.stderr)
            d.stop()
            if os.path.exists(d.log_path):
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
            print("FAIL test_fallback_e2e: sanitizer findings in the tt7d log:", *bad[:10], sep="\n", file=sys.stderr)
            return 1
        if log.count("showing the fallback clock") > 4:
            print("FAIL test_fallback_e2e: the fallback logs more than its transitions:\n" + log, file=sys.stderr)
            return 1
        for s in steps:
            print(f"  ok   fallback e2e: {s}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
