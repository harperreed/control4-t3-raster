#!/usr/bin/env python3
"""Build a newc (SVR4, no-CRC) cpio initramfs from a rootfs dir, forcing every
entry to uid/gid 0 and deterministic ordering.

Why not `find | cpio -o -H newc`: that stamps entries with the *host's* file
ownership (uid 501 on this Mac). dropbear then refuses to read
/root/.ssh/authorized_keys because it's "not owned by root", and various
other root-owned-file assumptions break. Forcing 0:0 here fixes that and
makes the image build reproducible regardless of who/where it's built.

Usage: mkcpio.py <rootfs_dir> <out.cpio>   (gzip separately)
"""
import os
import stat
import sys

TRAILER = "TRAILER!!!"


def emit_header(out, ino, mode, nlink, size, namesize, devmajor=0, devminor=0,
                rdevmajor=0, rdevminor=0):
    # newc: 070701 + 13 x 8-hex fields, uid/gid forced to 0, mtime 0
    fields = [
        ino, mode, 0, 0, nlink, 0, size,
        devmajor, devminor, rdevmajor, rdevminor, namesize, 0,
    ]
    out.write(b"070701")
    for f in fields:
        out.write(b"%08X" % (f & 0xFFFFFFFF))


def pad4(out, n):
    if n % 4:
        out.write(b"\x00" * (4 - (n % 4)))


def add_entry(out, ino, name, mode, nlink, data=b"", rdevmajor=0, rdevminor=0):
    name_bytes = name.encode() + b"\x00"
    emit_header(out, ino, mode, nlink, len(data), len(name_bytes),
                rdevmajor=rdevmajor, rdevminor=rdevminor)
    out.write(name_bytes)
    pad4(out, 6 + 13 * 8 + len(name_bytes))
    if data:
        out.write(data)
        pad4(out, len(data))


def main():
    rootfs, outpath = sys.argv[1], sys.argv[2]
    entries = []
    for dirpath, dirnames, filenames in os.walk(rootfs):
        dirnames.sort()
        for name in sorted(dirnames) + sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, rootfs)
            entries.append((rel, full))
    entries.sort()

    ino = 1
    with open(outpath, "wb") as out:
        for rel, full in entries:
            st = os.lstat(full)
            m = st.st_mode
            if stat.S_ISLNK(m):
                target = os.readlink(full).encode()
                add_entry(out, ino, rel, m, 1, data=target)
            elif stat.S_ISDIR(m):
                add_entry(out, ino, rel, m, 2)
            elif stat.S_ISREG(m):
                with open(full, "rb") as f:
                    data = f.read()
                add_entry(out, ino, rel, m, 1, data=data)
            else:
                # skip devices/fifos/sockets -- devtmpfs provides /dev nodes
                continue
            ino += 1
        # trailer
        add_entry(out, 0, TRAILER, 0, 1)
    print(f"wrote {outpath} ({os.path.getsize(outpath)} bytes, {ino - 1} entries, all uid/gid=0)")


if __name__ == "__main__":
    main()
