#!/usr/bin/env python3
"""Pull an AMD GPU's VBIOS image out of an ACPI VFCT table, the way Linux's
amdgpu_acpi_vfct_bios() does (drivers/gpu/drm/amd/amdgpu/amdgpu_bios.c),
and check it as check_atom_bios() does.

    python3 tools/vfct.py VFCT.aml                     # list the images
    python3 tools/vfct.py VFCT.aml 1002:1681 out.rom   # write the matching image

VFCT layout (atombios.h): a 36-byte ACPI header "VFCT", TableUUID[16],
VBIOSImageOffset u32 at 0x34, Lib1ImageOffset, Reserved[4]; at
VBIOSImageOffset a chain of {VFCT_IMAGE_HEADER (28 bytes: PCIBus, PCIDevice,
PCIFunction u32; VendorID, DeviceID, SSVID, SSID u16; Revision, ImageLength
u32), image bytes}. Linux takes the first image whose vendor and device match
(and prefers a matching bus/device/function); so does this.

Checks (Linux): size >= 0x49, signature 0x55 0xAA, a non-zero BIOS header
pointer at 0x48 with "ATOM" (or "MOTA") 4 bytes past it. Extra: the ACPI
table checksum, and the option-ROM checksum over byte 2 * 512 bytes (Apple's
cosGetVbiosRomImage only checks 0x55AA; the sum is reported, not enforced).
"""
import struct
import sys

HDR = struct.Struct("<IIIHHHHII")   # VFCT_IMAGE_HEADER, 28 bytes


class VfctError(Exception):
    pass


def parse(tbl):
    if len(tbl) < 0x4C:
        raise VfctError(f"table too short ({len(tbl)} bytes)")
    sig, length = tbl[:4], struct.unpack_from("<I", tbl, 4)[0]
    if sig != b"VFCT":
        raise VfctError(f"signature {sig!r}, not VFCT")
    if length > len(tbl) or length < 0x4C:
        raise VfctError(f"header length {length}, file {len(tbl)} bytes")
    tbl = tbl[:length]
    acpi_sum = sum(tbl) & 0xFF
    off = struct.unpack_from("<I", tbl, 0x34)[0]
    images = []
    while off < length:
        if off + HDR.size > length:
            raise VfctError("image header truncated")
        bus, dev, fn, vid, did, ssvid, ssid, rev, ilen = HDR.unpack_from(tbl, off)
        off += HDR.size
        if off + ilen > length:
            raise VfctError("image truncated")
        images.append({"bus": bus, "dev": dev, "fn": fn, "vendor": vid, "device": did,
                       "ssvid": ssvid, "ssid": ssid, "rev": rev, "data": tbl[off:off + ilen]})
        off += ilen
        if ilen == 0:
            break
    return acpi_sum, images


def check_atom(rom):
    """Linux check_atom_bios(); returns (ok, why)."""
    if len(rom) < 0x4A:
        return False, "image shorter than 0x4A bytes"
    if rom[0] != 0x55 or rom[1] != 0xAA:
        return False, f"signature {rom[0]:#04x} {rom[1]:#04x}, not 55 AA"
    start = rom[0x48] | (rom[0x49] << 8)
    if not start:
        return False, "no BIOS header pointer at 0x48"
    if len(rom) < start + 8:
        return False, "BIOS header past the end"
    if rom[start + 4:start + 8] not in (b"ATOM", b"MOTA"):
        return False, f"no ATOM magic at {start + 4:#x}"
    return True, "ATOMBIOS"


def rom_checksum(rom):
    size = rom[2] * 512 if len(rom) > 2 else 0
    if not size or size > len(rom):
        return None, size
    return sum(rom[:size]) & 0xFF, size


def pick(images, vendor, device, bus=None, dev=0, fn=0):
    cands = [i for i in images if i["vendor"] == vendor and i["device"] == device and i["data"]]
    exact = [i for i in cands if i["dev"] == dev and i["fn"] == fn and (bus is None or i["bus"] == bus)]
    return (exact or cands or [None])[0]


def describe(i):
    ok, why = check_atom(i["data"])
    s, size = rom_checksum(i["data"])
    return (f"{i['vendor']:04x}:{i['device']:04x} sub {i['ssvid']:04x}:{i['ssid']:04x} rev {i['rev']:#x} "
            f"at {i['bus']}:{i['dev']}.{i['fn']}, {len(i['data'])} bytes, {why if ok else 'BAD: ' + why}, "
            f"ROM size {size}, checksum {'n/a' if s is None else ('ok' if s == 0 else hex(s))}")


def main(argv):
    if len(argv) not in (2, 4):
        print(__doc__, file=sys.stderr)
        return 2
    tbl = open(argv[1], "rb").read()
    try:
        acpi_sum, images = parse(tbl)
    except VfctError as e:
        print(f"{argv[1]}: {e}", file=sys.stderr)
        return 1
    print(f"{argv[1]}: VFCT, {len(images)} image(s), ACPI checksum {'ok' if acpi_sum == 0 else 'BAD'}")
    for i in images:
        print("  " + describe(i))
    if len(argv) == 2:
        return 0
    vendor, device = (int(x, 16) for x in argv[2].split(":"))
    img = pick(images, vendor, device)
    if not img:
        print(f"no image for {vendor:04x}:{device:04x}", file=sys.stderr)
        return 1
    ok, why = check_atom(img["data"])
    if not ok:
        print(f"image for {vendor:04x}:{device:04x} fails Linux's check: {why}", file=sys.stderr)
        return 1
    with open(argv[3], "wb") as f:
        f.write(img["data"])
    print(f"wrote {len(img['data'])} bytes to {argv[3]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
