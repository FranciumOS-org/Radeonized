#!/usr/bin/env python3
"""Tests for vfct.py on synthetic VFCT tables.

    python3 tools/vfct_test.py
"""
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vfct  # noqa: E402

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vfct.py")


def rom(tag, size_blocks=2, atom=True):
    r = bytearray(size_blocks * 512)
    r[0], r[1], r[2] = 0x55, 0xAA, size_blocks
    r[0x48:0x4A] = struct.pack("<H", 0x100)
    if atom:
        r[0x104:0x108] = b"ATOM"
    r[0x200:0x200 + len(tag)] = tag
    r[-1] = (-sum(r[:-1])) & 0xFF          # option-ROM checksum
    return bytes(r)


def table(images, length_delta=0, sig=b"VFCT"):
    body = b""
    for (bus, dev, fn, vid, did, data) in images:
        body += vfct.HDR.pack(bus, dev, fn, vid, did, 0x1043, 0x14C3, 0, len(data)) + data
    hdr = bytearray(0x4C)
    hdr[0:4] = sig
    struct.pack_into("<I", hdr, 0x34, 0x4C)
    t = bytearray(hdr + body)
    struct.pack_into("<I", t, 4, len(t) + length_delta)
    t[9] = 0
    t[9] = (-sum(t)) & 0xFF                # ACPI checksum byte
    return bytes(t)


def main():
    fails = 0

    def check(c, what):
        nonlocal fails
        print(("ok   " if c else "FAIL ") + what)
        fails += not c

    dgpu, igpu = rom(b"dGPU"), rom(b"680M", 4)
    t = table([(3, 0, 0, 0x1002, 0x73BF, dgpu), (54, 0, 0, 0x1002, 0x1681, igpu)])
    s, imgs = vfct.parse(t)
    check(s == 0 and len(imgs) == 2, "two images, ACPI checksum ok")
    i = vfct.pick(imgs, 0x1002, 0x1681)
    check(i is not None and i["data"] == igpu and i["bus"] == 54, "picks 1002:1681")
    check(vfct.check_atom(i["data"]) == (True, "ATOMBIOS"), "680M image passes Linux's check")
    check(vfct.rom_checksum(i["data"]) == (0, 2048), "option-ROM checksum and size")
    check(vfct.pick(imgs, 0x1002, 0x1638) is None, "no image for another device")

    # Same device twice: the one at device 0 function 0 wins.
    t2 = table([(54, 1, 0, 0x1002, 0x1681, rom(b"wrong")), (54, 0, 0, 0x1002, 0x1681, igpu)])
    check(vfct.pick(vfct.parse(t2)[1], 0x1002, 0x1681)["data"] == igpu, "prefers device 0 function 0")

    bad = bytearray(igpu); bad[0x104:0x108] = b"XXXX"
    check(not vfct.check_atom(bytes(bad))[0], "missing ATOM magic is rejected")
    bad = bytearray(igpu); bad[1] = 0
    check(not vfct.check_atom(bytes(bad))[0], "bad 55AA signature is rejected")
    bad = bytearray(igpu); bad[0x48:0x4A] = b"\0\0"
    check(not vfct.check_atom(bytes(bad))[0], "null header pointer is rejected")
    check(not vfct.check_atom(igpu[:0x40])[0], "short image is rejected")

    for what, tt in (("truncated image", table([(54, 0, 0, 0x1002, 0x1681, igpu)])[:-10]),
                     ("length past the file", table([(54, 0, 0, 0x1002, 0x1681, igpu)], length_delta=100)),
                     ("wrong signature", table([(54, 0, 0, 0x1002, 0x1681, igpu)], sig=b"DSDT")),
                     ("too short", b"VFCT" + b"\0" * 20)):
        try:
            vfct.parse(tt)
            check(False, f"{what} raises")
        except vfct.VfctError:
            check(True, f"{what} raises")
    # A header whose ImageLength runs past the table end.
    tt = bytearray(table([(54, 0, 0, 0x1002, 0x1681, igpu)]))
    struct.pack_into("<I", tt, 0x4C + 24, len(igpu) + 1)
    try:
        vfct.parse(bytes(tt)); check(False, "image length past the end raises")
    except vfct.VfctError:
        check(True, "image length past the end raises")

    with tempfile.TemporaryDirectory() as d:
        p, out = os.path.join(d, "VFCT.aml"), os.path.join(d, "out.rom")
        open(p, "wb").write(t)
        r = subprocess.run([sys.executable, TOOL, p, "1002:1681", out], capture_output=True, text=True)
        check(r.returncode == 0 and open(out, "rb").read() == igpu, "CLI writes the 680M image")
        r = subprocess.run([sys.executable, TOOL, p, "1002:1638", out + "2"], capture_output=True, text=True)
        check(r.returncode == 1 and not os.path.exists(out + "2"), "CLI refuses a missing device")
        open(p, "wb").write(table([(54, 0, 0, 0x1002, 0x1681, rom(b"x", atom=False))]))
        r = subprocess.run([sys.executable, TOOL, p, "1002:1681", out + "3"], capture_output=True, text=True)
        check(r.returncode == 1 and not os.path.exists(out + "3"), "CLI refuses a non-ATOM image")

    print("all passed" if not fails else f"{fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
