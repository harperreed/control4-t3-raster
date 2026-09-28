#!/usr/bin/env python3
# ABOUTME: End-to-end test of web update: the real host-built tt7d takes bundles made by tools/make-bundle.sh
# ABOUTME: over real HTTP and installs them into a temp /data/tt7; also refusals, signing, rollback, pruning.
"""Usage: tt7d/test_update_e2e.py --daemon build/host/tt7d --payload-dir build   (run by `make test-update`)

Nothing is mocked. The daemon under test is the host build. Bundles come from
tools/make-bundle.sh with the panel's own ARM build/tt7d and build/tt7probe as
payload, the files a real update ships: the host tt7d cannot be the payload,
because its sanitizer build is larger than the 4 MiB bundle limit even
stripped. The payload is only installed and hashed here, never run. The update
root is a temp dir laid out like /data/tt7. After an install
or rollback tt7d exits with status 75, which tt7-app answers by starting over;
here the test starts the daemon again itself, with TT7_RELEASE set as tt7-app
sets it. Hostile tars are built with Python's tarfile.
"""
import argparse
import hashlib
import io
import json
import re
import os
import stat
import subprocess
import sys
import tarfile
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import test_e2e as e2e  # noqa: E402 - the Daemon wrapper

MAKE_BUNDLE = os.path.join(ROOT, "tools", "make-bundle.sh")
EXIT_RESTART = 75
PAYLOAD = ["bin/tt7d", "bin/tt7probe", "bin/tt7-ntp-hook", "app"]


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


class Env:
    def __init__(self, binary, payload_dir, workdir):
        self.binary = binary
        self.workdir = workdir
        self.bins = payload_dir
        self.bundles = os.path.join(workdir, "bundles")
        os.makedirs(self.bundles)

    def bundle(self, build, version=None, sign=None):
        out = os.path.join(self.bundles, f"{build}{'-signed' if sign else ''}.tar")
        cmd = [MAKE_BUNDLE, "--bin-dir", self.bins, "--build", build, "--out", out]
        if version:
            cmd += ["--version", version]
        if sign:
            cmd += ["--sign", sign]
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        with open(out, "rb") as f:
            return f.read()


PANELS = []


class Panel:
    """One tt7d with its own update root, restartable as tt7-app would restart it."""

    def __init__(self, env, name, extra=()):
        PANELS.append(self)
        self.dir = os.path.join(env.workdir, name)
        os.makedirs(self.dir)
        self.root = os.path.join(self.dir, "tt7")
        os.makedirs(self.root)
        # Flags in extra come after Daemon's own, so they win (e.g. --proc-root).
        args = ["--update-root", self.root, "--update-confirm-after", "1"] + list(extra)
        self.d = e2e.Daemon(env.binary, self.dir, args)
        self.release = ""

    def start(self, release=""):
        self.release = release
        os.environ["TT7_RELEASE"] = release
        try:
            self.d.start()
        finally:
            os.environ.pop("TT7_RELEASE", None)

    def stop(self):
        self.d.stop()

    def expect_restart_exit(self):
        """After a 202, tt7d exits with 75 about a second later."""
        try:
            rc = self.d.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            raise Fail("tt7d did not exit after the update")
        check(rc == EXIT_RESTART, f"tt7d exit status {rc}, want {EXIT_RESTART}")
        self.d.proc = None

    def auth(self):
        return {"Authorization": "Bearer " + self.d.token()}

    def put(self, body, token=True, ctype="application/x-tar"):
        h = {"Content-Type": ctype} if ctype else {}
        if token:
            h.update(self.auth())
        status, headers, raw = self.d.request("PUT", "/api/v1/system/update", body=body, headers=h)
        return status, json.loads(raw) if raw else None

    def rollback(self, token=True):
        h = self.auth() if token else {}
        status, _, raw = self.d.request("POST", "/api/v1/system/update/rollback", body=b"", headers=h)
        return status, json.loads(raw) if raw else None

    def status(self):
        status, _, raw = self.d.request("GET", "/api/v1/system/update")
        check(status == 200, f"GET /system/update: {status} {raw[:200]!r}")
        return json.loads(raw)

    def pointer(self, name):
        try:
            with open(os.path.join(self.root, "update", name)) as f:
                return f.read().strip()
        except FileNotFoundError:
            return None

    def releases(self):
        d = os.path.join(self.root, "releases")
        return sorted(os.listdir(d)) if os.path.isdir(d) else []

    def snapshot(self):
        """Everything under the update root, to prove a refusal changed nothing."""
        out = {}
        for dirpath, dirnames, filenames in os.walk(self.root):
            for n in dirnames + filenames:
                p = os.path.join(dirpath, n)
                st = os.lstat(p)
                out[os.path.relpath(p, self.root)] = (st.st_mode, st.st_size, st.st_mtime_ns)
        return out


def tar_bytes(members):
    """members: (name, bytes or None, tarinfo type)."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for name, data, kind in members:
            ti = tarfile.TarInfo(name)
            ti.type = kind
            if kind == tarfile.SYMTYPE or kind == tarfile.LNKTYPE:
                ti.linkname = "/etc/passwd"
            if data is not None:
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data))
            else:
                t.addfile(ti)
    return buf.getvalue()


def members_of(bundle):
    with tarfile.open(fileobj=io.BytesIO(bundle)) as t:
        return [(m.name, t.extractfile(m).read(), tarfile.REGTYPE) for m in t.getmembers()]


def expect_error(got, status, code, what):
    st, doc = got
    check(st == status and doc and doc.get("error") == code, f"{what}: want {status} {code}, got {st} {doc}")
    return doc


# ---- tests ------------------------------------------------------------------------------


def test_bundle_is_plain_ustar(env):
    """BusyBox tar (the panel's) extracts what make-bundle.sh makes."""
    b = env.bundle("fmt-check")
    out = os.path.join(env.workdir, "bb-extract")
    os.makedirs(out)
    tar_path = os.path.join(env.bundles, "fmt-check.tar")
    subprocess.run(["busybox", "tar", "-xf", tar_path, "-C", out], check=True)
    names = sorted(os.path.relpath(os.path.join(dp, f), out) for dp, _, fs in os.walk(out) for f in fs)
    check(names == sorted(["manifest.json"] + PAYLOAD), f"busybox tar extracts: {names}")
    with tarfile.open(fileobj=io.BytesIO(b)) as t:
        types = {m.name: m.type for m in t.getmembers()}
    check(all(v == tarfile.REGTYPE for v in types.values()), f"regular files only, no directory entries: {types}")
    m = json.load(open(os.path.join(out, "manifest.json")))
    check(m["format"] == "tt7-bundle" and m["build"] == "fmt-check", f"manifest: {m}")
    for f in m["files"]:
        check(sha256_file(os.path.join(out, f["path"])) == f["sha256"], f"manifest hash of {f['path']}")


def test_refusals(env, p):
    before = p.snapshot()
    good = env.bundle("e2e-v1")

    expect_error(p.put(good, token=False), 401, "unauthorized", "no token")
    st, _, _ = p.d.request("PUT", "/api/v1/system/update", body=good,
                           headers={"Authorization": "Bearer nope", "Content-Type": "application/x-tar"})
    check(st == 401, f"wrong token: {st}")
    doc = expect_error(p.put(good, ctype="application/octet-stream"), 415, "unsupported_media_type", "content type")
    check(doc.get("supported") == ["application/x-tar"], f"415 names the type: {doc}")
    expect_error(p.put(b"x" * (4 * 1024 * 1024 + 1)), 413, "payload_too_large", "over 4 MiB")

    # One byte of bin/tt7d changed, manifest untouched.
    ms = members_of(good)
    tampered = [(n, (d[:100] + bytes([d[100] ^ 1]) + d[101:]) if n == "bin/tt7d" else d, k) for n, d, k in ms]
    doc = expect_error(p.put(tar_bytes(tampered)), 400, "hash_mismatch", "bad hash")
    check(doc.get("member") == "bin/tt7d" and len(doc.get("expected", "")) == 64 and doc["expected"] != doc["computed"],
          f"hash_mismatch names the file and both hashes: {doc}")

    doc = expect_error(p.put(tar_bytes(ms + [("../evil", b"x", tarfile.REGTYPE)])), 400, "unsafe_path", "traversal")
    check(doc.get("member") == "../evil", f"names the member: {doc}")
    expect_error(p.put(tar_bytes(ms + [("/etc/evil", b"x", tarfile.REGTYPE)])), 400, "unsafe_path", "absolute")
    expect_error(p.put(tar_bytes(ms + [("bin/sh", b"x", tarfile.REGTYPE)])), 400, "unknown_file", "unknown file")
    no_app = [m for m in ms if m[0] != "app"]
    expect_error(p.put(tar_bytes(no_app)), 400, "missing_file", "listed file missing")
    swapped = [m for m in ms if m[0] != "app"] + [("app", None, tarfile.SYMTYPE)]
    expect_error(p.put(tar_bytes(swapped)), 400, "bad_member_type", "symlink")
    hard = [m for m in ms if m[0] != "app"] + [("app", None, tarfile.LNKTYPE)]
    expect_error(p.put(tar_bytes(hard)), 400, "bad_member_type", "hard link")
    dev = [m for m in ms if m[0] != "app"] + [("app", None, tarfile.CHRTYPE)]
    expect_error(p.put(tar_bytes(dev)), 400, "bad_member_type", "device")
    bad_json = [(n, b"{not json" if n == "manifest.json" else d, k) for n, d, k in ms]
    expect_error(p.put(tar_bytes(bad_json)), 400, "invalid_manifest", "bad manifest JSON")
    expect_error(p.put(b"\0" * 1024), 400, "no_manifest", "an empty tar")
    expect_error(p.put(b"definitely not a tar" * 40), 400, "invalid_bundle", "garbage")

    expect_error(p.rollback(token=False), 401, "unauthorized", "rollback without token")
    expect_error(p.rollback(), 409, "no_previous", "rollback with nothing installed")
    check(p.snapshot() == before, "refusals changed nothing under the update root")
    check(os.path.exists(os.path.join(os.path.dirname(p.root), "evil")) is False, "no file escaped the root")

    s = p.status()
    check(s["current"] is None and s["previous"] is None and s["trial"] is None, f"still nothing installed: {s}")
    check(s["last_error"]["error"] == "no_previous" and s["last_error"]["time"], f"last refusal kept: {s['last_error']}")
    check(s["history"] == [] and s["last_result"] is None, "refusals are not written to flash")
    check(s["running"]["release"] is None and s["running"]["build"], f"running: the image's build: {s['running']}")
    check(s["policy"]["signature_required"] is False and s["policy"]["max_bytes"] == 4 * 1024 * 1024, f"policy {s['policy']}")


def check_installed(p, rid, bundle):
    rdir = os.path.join(p.root, "releases", rid)
    manifest = None
    for n, d, _ in members_of(bundle):
        if n == "manifest.json":
            manifest = json.loads(d)
    for f in manifest["files"]:
        path = os.path.join(rdir, f["path"])
        check(os.path.isfile(path) and not os.path.islink(path), f"{rid}: {f['path']} is a regular file")
        check(sha256_file(path) == f["sha256"], f"{rid}: {f['path']} hash matches the manifest")
        check(stat.S_IMODE(os.stat(path).st_mode) == 0o755, f"{rid}: {f['path']} mode 0755")
    check(json.load(open(os.path.join(rdir, "manifest.json")))["build"] == rid, f"{rid}: manifest kept")
    check(not [n for n in os.listdir(os.path.join(p.root, "releases")) if n.startswith(".")], "no staging dir left")


def test_install_confirm_rollback(env, p):
    v1 = env.bundle("e2e-v1")
    t0 = time.monotonic()
    st, doc = p.put(v1)
    check(st == 202 and doc["release"] == "e2e-v1" and doc["restarting"] is True, f"install v1: {st} {doc}")
    check(doc["previous"] is None and doc["signed"] is False and isinstance(doc["install_ms"], int), f"reply: {doc}")
    st2, doc2 = p.put(v1)
    check(st2 == 409 and doc2["error"] == "restart_pending", f"a second upload before the restart: {st2} {doc2}")
    p.expect_restart_exit()
    print(f"         (install reply {doc['install_ms']} ms; upload to exit {time.monotonic() - t0:.2f} s)")
    check_installed(p, "e2e-v1", v1)
    check(p.pointer("current") == "e2e-v1" and p.pointer("previous") is None, "current flipped to v1")
    check(p.pointer("trial") == "e2e-v1 0", f"v1 under trial: {p.pointer('trial')!r}")

    # tt7-app starts v1 (it would count a start: "e2e-v1 1"); tt7d confirms it once healthy.
    p.start(release="e2e-v1")
    s = p.status()
    check(s["running"]["release"] == "e2e-v1" and s["current"]["release"] == "e2e-v1", f"status: {s}")
    deadline = time.monotonic() + 10
    while p.pointer("trial") is not None and time.monotonic() < deadline:
        time.sleep(0.1)
    check(p.pointer("trial") is None, "tt7d confirmed v1 (deleted update/trial)")
    s = p.status()
    check(s["trial"] is None and s["last_result"]["event"] == "confirmed", f"confirmed: {s['last_result']}")
    check(s["current"]["version"] and s["current"]["build"] == "e2e-v1", f"current: {s['current']}")

    expect_error(p.put(v1), 409, "already_current", "the same build again")

    v2 = env.bundle("e2e-v2")
    st, doc = p.put(v2)
    check(st == 202 and doc["release"] == "e2e-v2" and doc["previous"] == "e2e-v1", f"install v2: {st} {doc}")
    p.expect_restart_exit()
    check_installed(p, "e2e-v2", v2)
    check(p.pointer("current") == "e2e-v2" and p.pointer("previous") == "e2e-v1", "current v2, previous v1")

    # A tt7d whose release is not the one under trial must not confirm it.
    p.start(release="e2e-v1")
    time.sleep(1.5)
    check(p.pointer("trial") == "e2e-v2 0", "a tt7d of another release leaves v2's trial alone")
    st, doc = p.rollback()
    check(st == 202 and doc["release"] == "e2e-v1" and doc["previous"] == "e2e-v2", f"rollback: {st} {doc}")
    p.expect_restart_exit()
    check(p.pointer("current") == "e2e-v1" and p.pointer("previous") == "e2e-v2", "rollback flipped the pointers")
    check(p.pointer("trial") == "e2e-v1 0", "the rollback target runs under trial")
    p.start(release="e2e-v1")
    s = p.status()
    events = [h["event"] for h in s["history"]]
    check(events[:4] == ["installed", "confirmed", "installed", "rollback_requested"], f"history: {events}")
    check(s["previous"]["release"] == "e2e-v2", f"previous: {s['previous']}")


def test_prune(env, p):
    for v in ("e2e-v3", "e2e-v4", "e2e-v5"):
        st, doc = p.put(env.bundle(v))
        check(st == 202, f"install {v}: {st} {doc}")
        p.expect_restart_exit()
        p.start(release=v)
    rel = p.releases()
    check(len(rel) == 3, f"at most 3 releases kept: {rel}")
    check(p.pointer("current") == "e2e-v5" and p.pointer("previous") == "e2e-v4", "pointers")
    check("e2e-v5" in rel and "e2e-v4" in rel, f"current and previous kept: {rel}")
    check(p.status()["releases"] == sorted(rel), "GET lists the releases")


def test_limits(env):
    # Memory: a meminfo with 10 MiB free.
    proc = os.path.join(env.workdir, "proc-low")
    os.makedirs(proc)
    with open(os.path.join(proc, "meminfo"), "w") as f:
        f.write("MemTotal: 1048576 kB\nMemFree: 8000 kB\nBuffers: 1000 kB\nCached: 1000 kB\n")
    p = Panel(env, "lowmem", ["--proc-root", proc])
    p.start()
    try:
        doc = expect_error(p.put(env.bundle("mem")), 503, "insufficient_memory", "low RAM")
        check(doc["available_bytes"] == 10000 * 1024, f"reports what it saw: {doc}")
        check(p.releases() == [], "nothing written")
    finally:
        p.stop()

    # /data: ask for more free space than any disk has.
    p = Panel(env, "fulldisk", ["--update-min-free-bytes", str(1 << 60)])
    p.start()
    try:
        expect_error(p.put(env.bundle("disk")), 507, "insufficient_storage", "full /data")
        check(p.releases() == [], "nothing written")
    finally:
        p.stop()

    # Downgrades: allowed by default, refused with the flag.
    old = env.bundle("old-one", version="0.0.1")
    p = Panel(env, "downgrade", ["--update-refuse-downgrade"])
    p.start()
    try:
        doc = expect_error(p.put(old), 409, "downgrade_refused", "downgrade with the flag")
        check(doc["version"] == "0.0.1" and doc["running_version"], f"names both versions: {doc}")
    finally:
        p.stop()
    p = Panel(env, "downgrade-ok")
    p.start()
    try:
        st, doc = p.put(old)
        check(st == 202, f"downgrade without the flag: {st} {doc}")
        p.expect_restart_exit()
    finally:
        p.stop()


def test_signed(env):
    keys = os.path.join(env.workdir, "keys")
    os.makedirs(keys)
    k1, k2 = os.path.join(keys, "k1.pem"), os.path.join(keys, "k2.pem")
    for k in (k1, k2):
        subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519", "-out", k], check=True, capture_output=True)
    pub = subprocess.run([MAKE_BUNDLE, "--pubkey-of", k1], check=True, capture_output=True, text=True).stdout.strip()
    check(len(pub) == 64, f"pubkey hex: {pub!r}")
    p = Panel(env, "signed")
    os.makedirs(p.d.data, exist_ok=True)
    with open(os.path.join(p.d.data, "update-pubkey"), "w") as f:
        f.write(pub + "\n")
    p.start()
    try:
        check(p.status()["policy"]["signature_required"] is True, "a configured key requires signatures")
        expect_error(p.put(env.bundle("s1")), 403, "signature_required", "unsigned")
        expect_error(p.put(env.bundle("s1", sign=k2)), 403, "invalid_signature", "the wrong key")
        signed = env.bundle("s1", sign=k1)
        # Same files and signature, one byte of the manifest changed (a space added).
        ms = members_of(signed)
        edited = [(n, d.replace(b"{", b"{ ", 1) if n == "manifest.json" else d, k) for n, d, k in ms]
        expect_error(p.put(tar_bytes(edited)), 403, "invalid_signature", "manifest edited after signing")
        check(p.releases() == [], "nothing installed so far")
        st, doc = p.put(signed)
        check(st == 202 and doc["signed"] is True, f"signed with the right key: {st} {doc}")
        p.expect_restart_exit()
    finally:
        p.stop()

    p = Panel(env, "badkey")
    os.makedirs(p.d.data, exist_ok=True)
    with open(os.path.join(p.d.data, "update-pubkey"), "w") as f:
        f.write("not a key\n")
    p.start()
    try:
        expect_error(p.put(env.bundle("s2", sign=k1)), 500, "invalid_pubkey", "an unreadable key refuses everything")
    finally:
        p.stop()


def test_info_lists_auth(p):
    st, _, raw = p.d.request("GET", "/api/v1/info")
    req = json.loads(raw)["auth"]["required_for"]
    check("PUT /api/v1/system/update" in req and "POST /api/v1/system/update/rollback" in req, f"auth list: {req}")


def check_logs():
    """No sanitizer findings in any daemon's log (exit 75 included), and never the token."""
    for p in PANELS:
        with open(p.d.log_path, "rb") as f:
            log = f.read().decode("utf-8", "replace")
        bad = [ln for ln in log.splitlines() if re.search(r"AddressSanitizer|runtime error|LeakSanitizer", ln)]
        check(not bad, f"sanitizer findings in {p.d.log_path}: {bad[:5]}")
        token = p.d.token()
        check(token not in log, f"the token appears in {p.d.log_path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--daemon", required=True, help="host-built tt7d binary")
    ap.add_argument("--payload-dir", required=True, help="where make-bundle.sh finds tt7d and tt7probe (build: the ARM builds)")
    a = ap.parse_args()
    binary = os.path.abspath(a.daemon)
    with tempfile.TemporaryDirectory(prefix="tt7d-update-e2e-") as workdir:
        env = Env(binary, os.path.abspath(a.payload_dir), workdir)
        steps = [
            ("make-bundle.sh output is plain ustar that BusyBox tar extracts", lambda: test_bundle_is_plain_ustar(env)),
        ]
        p = Panel(env, "main")
        steps += [
            ("start", lambda: p.start()),
            ("/info lists the update endpoints under auth", lambda: test_info_lists_auth(p)),
            ("refusals: auth, type, size, hash, traversal, links, devices, manifest; nothing changes",
             lambda: test_refusals(env, p)),
            ("install v1 -> 202, restart; confirm; v2; rollback flips back", lambda: test_install_confirm_rollback(env, p)),
            ("pruning keeps 3 releases, never current or previous", lambda: test_prune(env, p)),
            ("stop", lambda: p.stop()),
            ("limits: RAM, /data free space, downgrade flag", lambda: test_limits(env)),
            ("signatures: required once a key is set; wrong key, edited manifest, bad key file",
             lambda: test_signed(env)),
            ("no sanitizer findings and no token in any tt7d log", check_logs),
        ]
        try:
            for name, fn in steps:
                fn()
                print(f"  ok   {name}")
        except Fail as e:
            print(f"  FAIL {name}: {e}")
            log = os.path.join(p.dir, "tt7d.log")
            if os.path.exists(log):
                print("--- tt7d log (main) ---")
                print(open(log).read()[-3000:])
            p.stop()
            sys.exit(1)
        finally:
            p.stop()
    print("test_update_e2e: all passed")


if __name__ == "__main__":
    main()
