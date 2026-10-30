#!/usr/bin/env python3
"""The "XHCI interrupter 0" OpenCore kernel patches, and the XHCIPoke switch.

    python3 tools/xhci_intr0.py verify                 # check the table against
                                                       # this Mac's AppleUSBXHCI
    python3 tools/xhci_intr0.py CONFIG status
    python3 tools/xhci_intr0.py CONFIG add             # add/refresh the patches
    python3 tools/xhci_intr0.py CONFIG remove
    python3 tools/xhci_intr0.py CONFIG poke on|off     # XHCIPoke.kext Enabled

The problem, measured on this machine: the five xHCI controllers each expose a
single MSI vector, so only interrupter 0 can raise an interrupt. AppleUSBXHCI
puts transfer events on interrupters 1 and 2, which nothing can signal here, so
completions are picked up by a ~2 s watchdog instead. XHCIPoke.kext works
around it by polling every controller every 10 ms; these patches fix it at the
source by forcing every Transfer TRB's Interrupter Target field to 0.

Each entry's Find was derived from the kext on this Mac by
`tools/re/xhci_intr0_derive.py`, which also proves every Find is unique in the
kext and that no site outside the transfer-ring builders is touched. `verify`
re-runs that check against the loaded binary, so a patch set that would not
apply is refused instead of being written into the EFI and silently skipped.

The macOS 15 set lives on in the config under its own MinKernel/MaxKernel; the
compiler moved three of the nine sites in macOS 26, which is why the Darwin 24
patches went inert on the upgrade rather than misapplying.
"""
import os
import plistlib
import subprocess
import sys

IDENT = "com.apple.driver.usb.AppleUSBXHCI"
TAG = "XHCI interrupter 0 (Darwin 25)"
MIN_KERNEL = "25.0.0"
MAX_KERNEL = "25.99.99"
POKE = "XHCIPoke.kext"

# name, Find, Replace. Derived 2026-10-30 from macOS 26.7.2 (25G332)
# AppleUSBXHCI at __TEXT 0xffffff8002f15000; the offsets in the names are from
# each function's own entry, so they survive a slide.
SITES = (
    ("Request::prepare+0x646  mov $1<<22, trb.status",
     "c744080800004000c744080c21040000", "c744080800000000c744080c21040000"),
    ("Request::prepare+0x8a2  mov $1<<22, %ecx",
     "b9000040004c89adc0feffff", "b9000000004c89adc0feffff"),
    ("Request::prepare+0xb0b  mov $1<<22, trb.status",
     "c744180800004000498b0424", "c744180800000000498b0424"),
    ("Request::prepare+0xb5c  mov $1<<22, trb.status",
     "c744080800004000c744080c00180000", "c744080800000000c744080c00180000"),
    ("Request::addTD+0x48     shl $22, interrupter -> xor",
     "41c1e4164489643208", "4531e4904489643208"),
    ("IsochRequest::prepare+0x6e5  mov $2<<22, trb.status",
     "c7441108000080008944110c", "c7441108000000008944110c"),
    ("IsochRequest::prepare+0x9f6  or  $2<<22, %eax",
     "0d0000800089441908", "0d0000000089441908"),
    ("IsochRequest::prepare+0xb6d  mov $2<<22, trb.status",
     "c744080800008000c744080c00180000", "c744080800000000c744080c00180000"),
    ("SparseRequest::prepare+0x127 mov $1<<22, %r8d",
     "41b8000040004489443e08", "41b8000000004489443e08"),
)

KC = "/System/Library/KernelCollections/BootKernelExtensions.kc"


class ConfigError(Exception):
    pass


def entry(name, find, repl):
    return {
        "Arch": "x86_64", "Base": "", "Comment": f"{TAG} | {name}", "Count": 1,
        "Enabled": True, "Find": bytes.fromhex(find), "Identifier": IDENT, "Limit": 0,
        "Mask": b"", "MaxKernel": MAX_KERNEL, "MinKernel": MIN_KERNEL,
        "Replace": bytes.fromhex(repl), "ReplaceMask": b"", "Skip": 0,
    }


def patch_list(cfg):
    try:
        lst = cfg["Kernel"]["Patch"]
    except (KeyError, TypeError):
        raise ConfigError("no Kernel -> Patch array; is this an OpenCore config.plist?")
    if not isinstance(lst, list):
        raise ConfigError("Kernel -> Patch is not an array")
    return lst


def ours(lst):
    return [i for i, p in enumerate(lst)
            if isinstance(p, dict) and str(p.get("Comment", "")).startswith(TAG)]


def add(cfg):
    lst = patch_list(cfg)
    for i in reversed(ours(lst)):               # re-add fresh, so every field is right
        del lst[i]
    for name, find, repl in SITES:
        lst.append(entry(name, find, repl))
    return (f"Kernel -> Patch: {len(SITES)} '{TAG}' patches, "
            f"MinKernel {MIN_KERNEL} MaxKernel {MAX_KERNEL}")


def remove(cfg):
    lst = patch_list(cfg)
    hits = ours(lst)
    for i in reversed(hits):
        del lst[i]
    return f"Kernel -> Patch: removed {len(hits)} '{TAG}' patch(es)"


def poke(cfg, on):
    try:
        lst = cfg["Kernel"]["Add"]
    except (KeyError, TypeError):
        raise ConfigError("no Kernel -> Add array")
    hits = [e for e in lst if isinstance(e, dict) and e.get("BundlePath") == POKE]
    if not hits:
        raise ConfigError(f"no {POKE} in Kernel -> Add")
    for e in hits:
        e["Enabled"] = on
        # the kext ships with MinKernel 24.0.0 from when it was written; leave
        # the range open so it stays available on whatever kernel is running
        if str(e.get("MinKernel", "")).startswith("24"):
            e["MinKernel"] = ""
    return f"Kernel -> Add: {POKE} Enabled = {on} ({len(hits)} entr{'y' if len(hits) == 1 else 'ies'})"


def status(cfg):
    out = []
    lst = patch_list(cfg)
    mine = ours(lst)
    out.append(f"'{TAG}': {len(mine)} of {len(SITES)} patches present"
               + (f", Enabled={sorted({lst[i].get('Enabled') for i in mine})}" if mine else ""))
    other = [p for p in lst if isinstance(p, dict)
             and str(p.get("Comment", "")).startswith("XHCI interrupter 0")
             and not str(p.get("Comment", "")).startswith(TAG)]
    if other:
        kern = sorted({(p.get("MinKernel"), p.get("MaxKernel")) for p in other})
        out.append(f"older interrupter-0 patches: {len(other)}, kernel ranges {kern}")
    for e in cfg.get("Kernel", {}).get("Add", []):
        if isinstance(e, dict) and e.get("BundlePath") == POKE:
            out.append(f"{POKE}: Enabled={e.get('Enabled')}, "
                       f"MinKernel={e.get('MinKernel')!r} MaxKernel={e.get('MaxKernel')!r}")
    return "\n".join(out)


def kext_bytes(path=KC):
    """The loaded AppleUSBXHCI as a flat image, extracted from the collection.
    A path that is not a kernel collection is read as it is, so the table can
    be checked against a saved kext or a synthetic image in the tests."""
    with open(path, "rb") as f:
        head = f.read(16)
    if len(head) < 16 or head[:4] != b"\xcf\xfa\xed\xfe" or head[12:16] != b"\x0c\x00\x00\x00":
        with open(path, "rb") as f:
            return f.read()
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "build", "re", "AppleUSBXHCI")
    out = os.path.normpath(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    r = subprocess.run([sys.executable, os.path.join(here, "re", "kc_extract.py"),
                        path, IDENT, out], capture_output=True, text=True)
    if r.returncode:
        raise ConfigError(f"could not extract {IDENT} from {path}: {r.stderr.strip()[:200]}")
    with open(out, "rb") as f:
        return f.read()


def verify(path=KC):
    """Every Find must occur exactly once, and every Replace must be the same
    length. Returns (ok, lines)."""
    data = kext_bytes(path)
    lines, ok = [], True
    for name, find, repl in SITES:
        f, r = bytes.fromhex(find), bytes.fromhex(repl)
        n = data.count(f)
        good = n == 1 and len(f) == len(r) and f != r
        ok &= good
        lines.append(f"  {'ok  ' if good else 'FAIL'} count={n} len={len(f)}/{len(r)}  {name}")
    # and the patched form must NOT already be present: that would mean we are
    # reading an already-patched image and cannot tell a hit from a miss
    for name, find, repl in SITES:
        if data.count(bytes.fromhex(repl)):
            ok = False
            lines.append(f"  FAIL the PATCHED bytes are already in the kext: {name}")
    return ok, lines


def main(argv):
    if len(argv) >= 2 and argv[1] == "verify":
        ok, lines = verify(argv[2] if len(argv) > 2 else KC)
        print(f"{len(SITES)} patches against {argv[2] if len(argv) > 2 else KC}:")
        print("\n".join(lines))
        print("all patch sites found exactly once" if ok else
              "REFUSED: the table does not match this kernel - re-derive with "
              "tools/re/xhci_intr0_derive.py")
        return 0 if ok else 1
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    path, cmd = argv[1], argv[2]
    with open(path, "rb") as f:
        cfg = plistlib.load(f)
    try:
        if cmd == "status":
            print(status(cfg))
            return 0
        if cmd == "add":
            ok, lines = verify()
            if not ok:
                print("\n".join(lines), file=sys.stderr)
                print("refusing to add patches that do not match this kernel; "
                      "re-derive with tools/re/xhci_intr0_derive.py", file=sys.stderr)
                return 1
            msg = add(cfg)
        elif cmd == "remove":
            msg = remove(cfg)
        elif cmd == "poke" and len(argv) == 4 and argv[3] in ("on", "off"):
            msg = poke(cfg, argv[3] == "on")
        else:
            print(__doc__, file=sys.stderr)
            return 2
    except ConfigError as e:
        print(f"config.plist: {e}; nothing changed", file=sys.stderr)
        return 1
    tmp = path + ".rbgpu-new"
    with open(tmp, "wb") as f:
        plistlib.dump(cfg, f, fmt=plistlib.FMT_XML, sort_keys=False)
        f.flush()
        os.fsync(f.fileno())
    with open(tmp, "rb") as f:
        if plistlib.load(f) != cfg:
            os.remove(tmp)
            print("config.plist: read-back mismatch; nothing changed", file=sys.stderr)
            return 1
    os.replace(tmp, path)
    print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
