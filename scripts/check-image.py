#!/usr/bin/env python3
# ABOUTME: Host-side checks on the built TT7 boot image: round-trip, kernel identity, ramdisk contents.
# ABOUTME: Run by `make check`; prints every finding and exits 1 if any check fails.
"""Checks, in order:
  1. image size fits the boot partition, magic and embedded id are valid (bootimg.verify)
  2. unpacking with MMKeypad's mkbootimg.py gives a kernel byte-identical to the stock one
  3. the gzipped ramdisk fits the loader's initrd window from the parameter block
  4. the ramdisk: listing, root ownership, required files, no stray write bits
  5. authorized_keys is exactly the owner's key and no other public key is anywhere in the ramdisk
  6. `file` says static ARM EABI5 for every executable; the NAND module matches the kernel version;
     tt7-app starts tt7d and the input-only logger, and discovery only in the background; ntpd and its hook
"""
import argparse
import gzip
import hashlib
import os
import re
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bootimg  # noqa: E402

# parameter.txt: CMDLINE ... initrd=0x62000000,0x00800000
INITRD_WINDOW = 0x00800000

REQUIRED = {
    "init": "file",
    "bin/busybox": "file",
    "bin/sh": "link",
    "usr/sbin/dropbearmulti": "file",
    "usr/sbin/dropbear": "link",
    "usr/bin/dropbearkey": "link",
    "usr/bin/scp": "link",
    "usr/bin/tt7probe": "file",
    "usr/bin/tt7d": "file",
    "usr/bin/tt7-app": "file",
    "usr/bin/tt7-discover": "file",
    "usr/bin/tt7-wifi-start": "file",
    "usr/bin/tt7-ntp-hook": "file",
    "usr/sbin/wpa_supplicant": "file",
    "usr/sbin/wpa_cli": "file",
    "lib/modules/rk30xxnand_ko.ko": "file",
    "etc/passwd": "file",
    "etc/group": "file",
    "etc/shells": "file",
    "etc/profile": "file",
    "usr/share/udhcpc/default.script": "file",
    "root": "dir",
    "root/.ssh": "dir",
    "root/.ssh/authorized_keys": "file",
}

PUBKEY_RE = re.compile(rb"(ssh-(?:ed25519|rsa|dss)|ecdsa-sha2-nistp\d+|sk-[a-z0-9-]+@openssh\.com) AAAA[0-9A-Za-z+/]{20,}")


class Report:
    def __init__(self):
        self.failed = 0

    def ok(self, msg):
        print(f"  ok   {msg}")

    def fail(self, msg):
        self.failed += 1
        print(f"  FAIL {msg}")

    def check(self, cond, msg):
        (self.ok if cond else self.fail)(msg)
        return cond


def parse_newc(blob):
    """Yield (name, mode, uid, gid, data) for each entry of a newc cpio archive."""
    pos = 0
    while True:
        if blob[pos:pos + 6] != b"070701":
            raise ValueError(f"bad cpio magic at {pos}")
        f = [int(blob[pos + 6 + 8 * i:pos + 14 + 8 * i], 16) for i in range(13)]
        mode, uid, gid, filesize, namesize = f[1], f[2], f[3], f[6], f[11]
        name_start = pos + 110
        name = blob[name_start:name_start + namesize - 1].decode()
        data_start = (name_start + namesize + 3) & ~3
        data = blob[data_start:data_start + filesize]
        pos = (data_start + filesize + 3) & ~3
        if name == "TRAILER!!!":
            return
        yield name, mode, uid, gid, data


def kind(mode):
    if stat.S_ISDIR(mode):
        return "dir"
    if stat.S_ISLNK(mode):
        return "link"
    if stat.S_ISREG(mode):
        return "file"
    return "other"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True)
    ap.add_argument("--stock", required=True, help="the unit's stock boot partition dump")
    ap.add_argument("--pubkey", required=True, help="the only key allowed in authorized_keys")
    args = ap.parse_args()
    r = Report()

    print(f"[1] boot image {args.image}")
    ok, notes = bootimg.verify(args.image)
    for n in notes:
        print(f"  {n}")
    if not ok:
        r.failed += 1
    with open(args.image, "rb") as f:
        print(f"     sha256 {hashlib.sha256(f.read()).hexdigest()}")

    print("[2] round-trip against stock")
    ok_stock, notes = bootimg.verify(args.stock)
    r.check(ok_stock, f"stock image {args.stock} still has a valid embedded id")
    stock, ours = bootimg.load(args.stock), bootimg.load(args.image)
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([sys.executable, bootimg.mkbootimg.__file__, "unpack", args.image, tmp],
                       check=True, stdout=subprocess.DEVNULL)
        with open(os.path.join(tmp, "kernel.img"), "rb") as f:
            unpacked_kernel = f.read()
        with open(os.path.join(tmp, "ramdisk.cpio.gz"), "rb") as f:
            unpacked_ramdisk = f.read()
    r.check(unpacked_kernel == stock["kernel"],
            f"mkbootimg.py unpack: kernel identical to stock ({len(unpacked_kernel)} B, "
            f"sha256 {hashlib.sha256(unpacked_kernel).hexdigest()[:16]}...)")
    r.check(unpacked_ramdisk == ours["ramdisk"], "mkbootimg.py unpack: ramdisk matches the header's section")
    for field in ("kernel_addr", "ramdisk_addr", "second_addr", "tags_addr", "page_size", "name", "cmdline"):
        r.check(ours["header"][field] == stock["header"][field], f"header {field} same as stock")

    print("[3] ramdisk size")
    r.check(len(ours["ramdisk"]) <= INITRD_WINDOW,
            f"gzipped ramdisk {len(ours['ramdisk'])} B <= initrd window {INITRD_WINDOW} B (parameter block)")

    print("[4] ramdisk contents (mode uid:gid size name)")
    entries = {}
    for name, mode, uid, gid, data in parse_newc(gzip.decompress(ours["ramdisk"])):
        entries[name] = (mode, uid, gid, data)
        target = f" -> {data.decode()}" if stat.S_ISLNK(mode) else ""
        if not (stat.S_ISLNK(mode) and data == b"/bin/busybox"):  # 400 applet links: summarised below
            print(f"     {stat.filemode(mode)} {uid}:{gid} {len(data):>8} {name}{target}")
    applets = sum(1 for m, _, _, d in entries.values() if stat.S_ISLNK(m) and d == b"/bin/busybox")
    print(f"     ... plus {applets} BusyBox applet links -> /bin/busybox")
    not_root = [n for n, (_, u, g, _) in entries.items() if u != 0 or g != 0]
    r.check(not not_root, f"all {len(entries)} entries owned by 0:0" + (f" (not: {not_root[:5]})" if not_root else ""))
    for name, want in REQUIRED.items():
        got = kind(entries[name][0]) if name in entries else "missing"
        r.check(got == want, f"{name} is a {want}" + ("" if got == want else f" (got {got})"))
    writable = [n for n, (m, _, _, _) in entries.items() if not stat.S_ISLNK(m) and m & (stat.S_IWGRP | stat.S_IWOTH)]
    r.check(not writable, "no group/world-writable entries" + (f" (found {writable[:5]})" if writable else ""))
    for name, want in (("root", 0o700), ("root/.ssh", 0o700), ("root/.ssh/authorized_keys", 0o600)):
        if name in entries:
            r.check(stat.S_IMODE(entries[name][0]) == want, f"{name} mode {want:o}")
    r.check(not any("dev-keys" in n for n in entries), "no MMKeypad dev-keys path in the ramdisk")

    print("[5] SSH keys")
    with open(args.pubkey, "rb") as f:
        owner = f.read()
    ak = entries.get("root/.ssh/authorized_keys", (0, 0, 0, b""))[3]
    r.check(ak == owner, f"authorized_keys is byte-identical to {args.pubkey}")
    owner_key = PUBKEY_RE.search(owner)
    stray = []
    for name, (mode, _, _, data) in entries.items():
        if not stat.S_ISREG(mode):
            continue
        for m in PUBKEY_RE.finditer(data):
            if owner_key is None or m.group(0) != owner_key.group(0):
                stray.append(name)
    r.check(not stray, "no public key other than the owner's anywhere in the ramdisk"
            + (f" (found in {sorted(set(stray))})" if stray else ""))

    print("[6] binaries")
    with tempfile.TemporaryDirectory() as tmp:
        for name, (mode, _, _, data) in sorted(entries.items()):
            if not stat.S_ISREG(mode) or not data.startswith(b"\x7fELF"):
                continue
            path = os.path.join(tmp, name.replace("/", "_"))
            with open(path, "wb") as f:
                f.write(data)
            desc = subprocess.run(["file", "-b", path], check=True, capture_output=True, text=True).stdout.strip()
            if name.endswith(".ko"):
                r.check("ARM, EABI5" in desc and "relocatable" in desc, f"{name}: {desc}")
            else:
                r.check("ARM, EABI5" in desc and "statically linked" in desc, f"{name}: {desc}")
    init = entries.get("init", (0, 0, 0, b""))[3]
    r.check(b"/data/tt7/usb-watchdog.log" in init, "init includes the USB link watchdog")
    r.check(b"tt7-usb-probe" in init, "init includes the RNDIS tx-stall remedy")
    r.check(b"pool.ntp.org" not in init and b"ntpd" not in init,
            "init starts no ntpd of its own (tt7-app owns NTP, with the sync hook)")
    app = entries.get("usr/bin/tt7-app", (0, 0, 0, b""))[3]
    r.check(b"tt7d --data-dir" in app and b"tt7probe log" in app and b"tt7probe run" not in app,
            "tt7-app runs tt7d and the input-only logger, not the test pattern")
    # SPEC 38: tt7d (and its fallback clock) must not wait for discovery. Every
    # tt7-discover call is a background job, and the tt7d loop is the last thing.
    discover_calls = re.findall(rb"^.*\btt7-discover \"\$out\".*$", app, re.M)
    r.check(bool(discover_calls) and all(re.match(rb"\s*\(tt7-discover .*\) &$", c) for c in discover_calls),
            "tt7-app runs discovery in the background, so tt7d starts before it finishes")
    r.check(app.rstrip().endswith(b"done") and app.rfind(b"tt7d --data-dir") > app.rfind(b"tt7-discover"),
            "tt7-app ends in the tt7d loop")
    r.check(b"exec ntpd -n -S \"$hook\"" in app and b"command -v tt7-ntp-hook" in app,
            "tt7-app starts ntpd with the tt7-ntp-hook sync hook")
    ntpd = entries.get("usr/sbin/ntpd")
    r.check(bool(ntpd) and stat.S_ISLNK(ntpd[0]) and ntpd[3] == b"/bin/busybox", "ntpd is a BusyBox applet in the ramdisk")
    busybox = entries.get("bin/busybox", (0, 0, 0, b""))[3]
    # run_script() puts freq_drift_ppm in the hook's env (usage text is compressed, so not searchable).
    r.check(b"freq_drift_ppm" in busybox, "BusyBox ntpd has the -S hook (run_script)")
    hook = entries.get("usr/bin/tt7-ntp-hook", (0, 0, 0, b""))[3]
    r.check(b"/run/tt7/ntp-synced" in hook, "tt7-ntp-hook writes /run/tt7/ntp-synced")
    tt7d = entries.get("usr/bin/tt7d", (0, 0, 0, b""))[3]
    r.check(b"/api/v1/frame" in tt7d, "tt7d serves /api/v1/frame")
    ko = entries.get("lib/modules/rk30xxnand_ko.ko", (0, 0, 0, b""))[3]
    vermagic = re.search(rb"vermagic=(\S+)", ko)
    release = re.search(rb"Linux version (\S+)", stock["kernel"])
    r.check(bool(vermagic and release and vermagic.group(1) == release.group(1)),
            f"NAND module vermagic {vermagic.group(1).decode() if vermagic else '?'} == kernel release "
            f"{release.group(1).decode() if release else '?'}")

    print()
    if r.failed:
        print(f"check-image: {r.failed} check(s) FAILED")
        sys.exit(1)
    print("check-image: all checks passed")


if __name__ == "__main__":
    main()
