#!/usr/bin/env python3
"""Put a machine's own VBIOS into an already-built RembrandtGPU.kext.

A release build bakes no VBIOS in (`make plugin RELEASE=1`), because the image
is the board vendor's, not ours. Instead the owner's own image goes into the
installed bundle's Info.plist, where IOKit hands it to RembrandtGPUPeer as its
`ATY,bin_image` property and the peer forwards it to the PCI device. Nothing
proprietary is ever redistributed: the only copy that moves is the one already
on the board this runs on.

    python3 tools/vbios_plist.py RembrandtGPU.kext VFCT.aml      # or a .rom
    python3 tools/vbios_plist.py RembrandtGPU.kext --check
    python3 tools/vbios_plist.py RembrandtGPU.kext --remove

A VFCT.aml is the ACPI table; the image for 1002:1681 or 1002:164d is taken
out of it the way Linux's amdgpu_acpi_vfct_bios() does. A .rom is used as
given. Either way the image must pass Linux's check_atom_bios().
"""
import os
import plistlib
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vfct  # noqa: E402

KEY = "ATY,bin_image"
PERSONALITY = "RembrandtGPUPeer"
IDS = ((0x1002, 0x1681), (0x1002, 0x164D))


def plist_path(bundle):
    p = os.path.join(bundle, "Contents", "Info.plist")
    if not os.path.exists(p):
        sys.exit(f"{bundle}: no Contents/Info.plist - is that a kext bundle?")
    return p


def load(path):
    with open(path, "rb") as f:
        pl = plistlib.load(f)
    pers = pl.get("IOKitPersonalities", {})
    if PERSONALITY not in pers:
        sys.exit(f"{path}: no '{PERSONALITY}' personality")
    return pl, pers[PERSONALITY]


def save(path, pl):
    with open(path, "wb") as f:
        plistlib.dump(pl, f, fmt=plistlib.FMT_XML, sort_keys=True)


def image_from(src):
    blob = open(src, "rb").read()
    if blob[:4] == b"VFCT":
        images = vfct.parse(blob)[1]
        for vid, did in IDS:
            img = vfct.pick(images, vid, did)
            if img:
                print(f"  VFCT image {vid:04x}:{did:04x}, {len(img['data'])} bytes")
                return img["data"]
        have = ", ".join(f"{i['VendorID']:04x}:{i['DeviceID']:04x}" for i in images)
        sys.exit(f"{src}: no Rembrandt image in the table (it carries {have})")
    return blob


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    bundle, arg = sys.argv[1], sys.argv[2]
    path = plist_path(bundle)
    pl, pers = load(path)

    if arg == "--check":
        got = pers.get(KEY)
        if not isinstance(got, bytes):
            print(f"{bundle}: no {KEY} - the peer will publish no VBIOS")
            return 1
        ok, why = vfct.check_atom(got)
        print(f"{bundle}: {KEY} {len(got)} bytes, {why}")
        return 0 if ok else 1

    if arg == "--remove":
        if pers.pop(KEY, None) is None:
            print(f"{bundle}: no {KEY} to remove")
            return 0
        save(path, pl)
        print(f"  DEL  {KEY} from {path}")
        return 0

    if not os.path.exists(arg):
        sys.exit(f"{arg} missing. Dump the table first: "
                 "'sudo tools/efi.sh sysreport-on', reboot, 'sudo tools/efi.sh vbios'")
    rom = image_from(arg)
    ok, why = vfct.check_atom(rom)
    if not ok:
        sys.exit(f"{arg}: {why}")
    pers[KEY] = rom
    save(path, pl)
    print(f"  SET  {KEY} = {len(rom)} bytes ({why}) in {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
