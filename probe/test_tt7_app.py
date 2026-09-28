#!/usr/bin/env python3
# ABOUTME: Tests tt7-app.sh's release selection and trial/rollback functions under BusyBox sh, in a temp root.
# ABOUTME: Sources the real script with TT7_APP_LIB=1, so its functions run without starting any daemon.
"""Usage: probe/test_tt7_app.py [--shell 'busybox sh']   (run by `make test-update`)

The fallback chain under test: a new release under trial, then the previous
release (also under trial), then the image's own build. Files are real files in
a temporary root laid out like /data/tt7; nothing is mocked. tt7d's side of the
protocol (it deletes update/trial once healthy) is played here by deleting the
file, which is exactly what tt7d does (tt7d/update.c); the real daemon doing it
is covered in tt7d/test_update_e2e.py.
"""
import argparse
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "tt7-app.sh")
failures = 0


def check(cond, msg):
    global failures
    if cond:
        print(f"  ok   {msg}")
    else:
        failures += 1
        print(f"  FAIL {msg}")


ROOTS = []


class Root:
    def __init__(self, shell):
        self.shell = shell
        self.dir = tempfile.mkdtemp(prefix="tt7-app-test-")
        ROOTS.append(self.dir)
        os.makedirs(os.path.join(self.dir, "update"))

    def release(self, rid, runnable=True):
        d = os.path.join(self.dir, "releases", rid)
        os.makedirs(os.path.join(d, "bin"), exist_ok=True)
        for f in ("app", "bin/tt7d"):
            with open(os.path.join(d, f), "w") as fh:
                fh.write("#!/bin/sh\n")
            os.chmod(os.path.join(d, f), 0o755 if runnable else 0o644)

    def put(self, name, text):
        with open(os.path.join(self.dir, "update", name), "w") as f:
            f.write(text)

    def get(self, name):
        try:
            with open(os.path.join(self.dir, "update", name)) as f:
                return f.read().strip()
        except FileNotFoundError:
            return None

    def history(self):
        return self.get("history") or ""

    def sh(self, call):
        """Run one function of tt7-app.sh; returns (exit status, stdout)."""
        cmd = f"TT7_APP_LIB=1 . {shlex.quote(SCRIPT)} && {call}"
        p = subprocess.run(self.shell + ["-c", cmd], capture_output=True, text=True, timeout=20)
        if "not found" in p.stderr or "syntax error" in p.stderr:
            print(p.stderr, file=sys.stderr)
        return p.returncode, p.stdout.strip()

    def select(self):
        return self.sh(f"select_release {shlex.quote(self.dir)}")[1]

    def after_exit(self, rid, rc):
        return self.sh(f"after_tt7d_exit {shlex.quote(self.dir)} {shlex.quote(rid)} {rc}")[1]


def test_no_release(shell):
    r = Root(shell)
    check(r.select() == "", "no current release: the image's own build (empty)")
    r.put("trial", "ghost 1\n")
    check(r.select() == "" and r.get("trial") is None, "a trial with no current release is dropped")


def test_new_release_confirmed(shell):
    r = Root(shell)
    r.release("old")
    r.release("new")
    r.put("current", "new\n")
    r.put("previous", "old\n")
    r.put("trial", "new 0\n")
    check(r.select() == "new", "new release under trial is selected")
    check(r.get("trial") == "new 1", f"its first start is counted: {r.get('trial')!r}")
    check(r.sh(f"trial_pending {shlex.quote(r.dir)} new")[0] == 0, "trial_pending: yes, for new")
    check(r.sh(f"trial_pending {shlex.quote(r.dir)} old")[0] != 0, "trial_pending: no, for another release")
    os.remove(os.path.join(r.dir, "update", "trial"))  # what tt7d does once healthy
    check(r.sh(f"trial_pending {shlex.quote(r.dir)} new")[0] != 0, "confirmed: no trial pending")
    check(r.select() == "new" and r.get("trial") is None, "after a reboot a confirmed release runs, no new trial")
    check(r.after_exit("new", 1) == "again", "a confirmed release that crashes is just restarted")
    check(r.get("current") == "new" and r.get("previous") == "old", "pointers untouched")


def test_crash_chain(shell):
    r = Root(shell)
    r.release("old")
    r.release("new")
    r.put("current", "new\n")
    r.put("previous", "old\n")
    r.put("trial", "new 0\n")
    check(r.select() == "new", "chain: new selected (start 1)")
    check(r.after_exit("new", 139) == "again", "chain: first crash under trial -> start it once more")
    check(r.get("trial") == "new 2", f"chain: second start counted: {r.get('trial')!r}")
    check(r.after_exit("new", 139) == "rollback", "chain: second crash -> roll back")
    check(r.select() == "old", "chain: previous release selected")
    check(r.get("current") == "old" and r.get("previous") is None, "chain: current=old, previous cleared")
    check(r.get("trial") == "old 1", f"chain: previous runs under trial too: {r.get('trial')!r}")
    check("rolled_back new to=old" in r.history(), f"chain: history records it: {r.history()!r}")
    check(r.after_exit("old", 1) == "again", "chain: previous crashes once")
    check(r.after_exit("old", 1) == "rollback", "chain: previous crashes twice")
    check(r.select() == "", "chain: the image's own build is left")
    check(r.get("current") is None and r.get("trial") is None, "chain: no current, no trial")
    check("rolled_back old to=image" in r.history(), "chain: history records the fall to the image")
    check(os.path.isdir(os.path.join(r.dir, "releases", "new")), "chain: failed releases are kept on disk (pruning is tt7d's)")


def test_restart_request(shell):
    r = Root(shell)
    r.release("new")
    r.put("current", "new\n")
    r.put("trial", "new 1\n")
    check(r.after_exit("new", 75) == "restart", "exit 75 asks for a fresh start")
    check(r.get("trial") == "new 1", "a restart request is not a failed start")
    check(r.after_exit("", 75) == "restart" and r.after_exit("", 1) == "again", "the image's build: restart / again")


def test_broken_app_script(shell):
    # A release whose app dies before starting tt7d never reaches after_tt7d_exit;
    # init respawns the entry script, and each start is counted by select_release.
    # The third start must already run the previous release: init quarantines
    # /data/tt7/app after the third quick exit (init.c OTA_MAX_FAILS).
    r = Root(shell)
    r.release("old")
    r.release("new")
    r.put("current", "new\n")
    r.put("previous", "old\n")
    r.put("trial", "new 0\n")
    got = [r.select() for _ in range(3)]
    check(got == ["new", "new", "old"], f"broken app: new, new, then old: {got}")


def test_unusable_current(shell):
    r = Root(shell)
    r.release("old")
    r.put("current", "gone\n")
    r.put("previous", "old\n")
    check(r.select() == "old", "current names a missing release -> previous")
    check("rolled_back gone to=old" in r.history(), "logged")
    r2 = Root(shell)
    r2.release("noexec", runnable=False)
    r2.put("current", "noexec\n")
    check(r2.select() == "" and r2.get("current") is None, "not executable, no previous -> the image's build")
    r3 = Root(shell)
    r3.put("current", "../../etc\n")
    check(r3.select() == "" and r3.get("current") is None, "a pointer that is not a release id is not followed")


def test_stale_trial(shell):
    r = Root(shell)
    r.release("a")
    r.put("current", "a\n")
    r.put("trial", "b 1\n")
    check(r.select() == "a" and r.get("trial") is None, "a trial for another release is dropped")
    r.put("trial", "garbage\n")
    check(r.select() == "a" and r.get("trial") is None, "an unreadable trial is dropped")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shell", default="busybox sh", help="the shell to run tt7-app.sh under (the panel runs BusyBox ash)")
    args = ap.parse_args()
    shell = shlex.split(args.shell)
    try:
        for t in (test_no_release, test_new_release_confirmed, test_crash_chain, test_restart_request,
                  test_broken_app_script, test_unusable_current, test_stale_trial):
            t(shell)
    finally:
        for d in ROOTS:
            shutil.rmtree(d, ignore_errors=True)
    if failures:
        print(f"test_tt7_app: {failures} failure(s)")
        sys.exit(1)
    print("test_tt7_app: all passed")


if __name__ == "__main__":
    main()
