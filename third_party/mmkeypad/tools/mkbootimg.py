#!/usr/bin/env python3
"""Minimal AOSP boot.img (v0, no DTB) pack/unpack for the T3's stock kernel.

The T3's boot.img turned out to be a plain, standard AOSP header (magic
"ANDROID!", no Rockchip-specific wrapper) -- confirmed by hand-parsing
reference/t3-control4/firmware/extracted/boot.orig during Phase 0. This tool
formalizes that parsing so we can repack a new ramdisk against the
*unmodified* kernel image without needing any third-party tool.

Usage:
  mkbootimg.py unpack <boot.img> <out_dir>
  mkbootimg.py pack --kernel K --ramdisk R --header-from <boot.orig> -o <out.img>
"""
import argparse
import hashlib
import struct
import sys

MAGIC = b"ANDROID!"
HEADER_LEN = 8 + 4 * 10 + 16 + 512  # magic + 10 uint32s + name + cmdline (+ id[8] below)
ID_LEN = 20  # SHA1 digest; header reserves 32 bytes (id[8] uint32) but only 20 are used


def pages(n, page_size):
    return (n + page_size - 1) // page_size


def parse_header(data):
    if data[0:8] != MAGIC:
        raise SystemExit(f"not an AOSP boot.img (magic={data[0:8]!r})")
    kernel_size, kernel_addr = struct.unpack_from("<II", data, 8)
    ramdisk_size, ramdisk_addr = struct.unpack_from("<II", data, 16)
    second_size, second_addr = struct.unpack_from("<II", data, 24)
    tags_addr, page_size = struct.unpack_from("<II", data, 32)
    unused, os_version = struct.unpack_from("<II", data, 40)
    name = data[48:64]
    cmdline = data[64:64 + 512]
    return {
        "kernel_size": kernel_size, "kernel_addr": kernel_addr,
        "ramdisk_size": ramdisk_size, "ramdisk_addr": ramdisk_addr,
        "second_size": second_size, "second_addr": second_addr,
        "tags_addr": tags_addr, "page_size": page_size,
        "name": name, "cmdline": cmdline,
    }


def cmd_unpack(args):
    with open(args.boot_img, "rb") as f:
        data = f.read()
    h = parse_header(data)
    print(f"kernel_size={h['kernel_size']} ramdisk_size={h['ramdisk_size']} "
          f"page_size={h['page_size']}")

    kernel_off = h["page_size"]
    kernel_pages = pages(h["kernel_size"], h["page_size"])
    ramdisk_off = kernel_off + kernel_pages * h["page_size"]

    import os
    os.makedirs(args.out_dir, exist_ok=True)
    with open(f"{args.out_dir}/kernel.img", "wb") as f:
        f.write(data[kernel_off:kernel_off + h["kernel_size"]])
    with open(f"{args.out_dir}/ramdisk.cpio.gz", "wb") as f:
        f.write(data[ramdisk_off:ramdisk_off + h["ramdisk_size"]])
    print(f"wrote {args.out_dir}/kernel.img and ramdisk.cpio.gz")


def compute_id(kernel, ramdisk, second, second_size, tags_addr, page_size, name, cmdline):
    """Reproduces this device's boot.img `id` field (SHA1) -- confirmed by
    matching the original boot.orig's id byte-for-byte during Phase 1
    bring-up. This is the AOSP/Rockchip mkbootimg algorithm circa ~2013+:
    SHA1(kernel + kernel_size + ramdisk + ramdisk_size + second + second_size
         + tags_addr + page_size + unused[2] + name[16] + cmdline[512])
    -- i.e. the classic 3-part hash PLUS a trailing update over the 544-byte
    header suffix (tags_addr through cmdline), which is the part that's easy
    to miss since most "classic AOSP mkbootimg" references online predate it."""
    h = hashlib.sha1()
    h.update(kernel)
    h.update(struct.pack("<I", len(kernel)))
    h.update(ramdisk)
    h.update(struct.pack("<I", len(ramdisk)))
    h.update(second)
    h.update(struct.pack("<I", second_size))
    h.update(struct.pack("<IIII", tags_addr, page_size, 0, 0))
    h.update(name)
    h.update(cmdline)
    return h.digest()[:ID_LEN]


def cmd_pack(args):
    with open(args.header_from, "rb") as f:
        orig = f.read()
    h = parse_header(orig)

    with open(args.kernel, "rb") as f:
        kernel = f.read()
    with open(args.ramdisk, "rb") as f:
        ramdisk = f.read()

    page_size = h["page_size"]

    header = bytearray(HEADER_LEN)
    header[0:8] = MAGIC
    struct.pack_into("<II", header, 8, len(kernel), h["kernel_addr"])
    struct.pack_into("<II", header, 16, len(ramdisk), h["ramdisk_addr"])
    struct.pack_into("<II", header, 24, 0, h["second_addr"])
    struct.pack_into("<II", header, 32, h["tags_addr"], page_size)
    struct.pack_into("<II", header, 40, 0, 0)
    header[48:64] = h["name"]
    header[64:64 + 512] = h["cmdline"]

    boot_id = compute_id(kernel, ramdisk, b"", 0, h["tags_addr"], page_size,
                          h["name"], h["cmdline"])
    header += boot_id  # bytes 576:596 -- id[8] field only needs the 20 SHA1 bytes,
                        # the remaining 12 reserved bytes stay zero same as original

    def pad(buf):
        n = pages(len(buf), page_size) * page_size
        return buf + b"\x00" * (n - len(buf))

    out = bytearray()
    out += pad(bytes(header))
    out += pad(kernel)
    out += pad(ramdisk)

    # Pad to the original boot.orig file size (the full NAND partition span)
    # so it writes back to the same LBA range with rkdeveloptool.
    if len(out) < len(orig):
        out += b"\x00" * (len(orig) - len(out))
    elif len(out) > len(orig):
        raise SystemExit(
            f"new boot.img ({len(out)} bytes) exceeds the boot partition "
            f"size ({len(orig)} bytes) -- ramdisk/kernel too large")

    with open(args.out, "wb") as f:
        f.write(out)
    print(f"wrote {args.out} ({len(out)} bytes, partition-sized)")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_unpack = sub.add_parser("unpack")
    p_unpack.add_argument("boot_img")
    p_unpack.add_argument("out_dir")
    p_unpack.set_defaults(func=cmd_unpack)

    p_pack = sub.add_parser("pack")
    p_pack.add_argument("--kernel", required=True)
    p_pack.add_argument("--ramdisk", required=True)
    p_pack.add_argument("--header-from", required=True,
                         help="original boot.img/boot.orig to copy header fields + pad size from")
    p_pack.add_argument("-o", "--out", required=True)
    p_pack.set_defaults(func=cmd_pack)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
