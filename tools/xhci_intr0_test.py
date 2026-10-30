#!/usr/bin/env python3
"""Tests for xhci_intr0.py: the config edits against a synthetic OpenCore
config, and `verify` against synthetic kext images.

    python3 tools/xhci_intr0_test.py
"""
import os
import plistlib
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xhci_intr0 as xi  # noqa: E402

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xhci_intr0.py")

OLD = "XHCI interrupter 0 | IO request prepare+0x65f"


def sample():
    return {
        "Kernel": {
            "Add": [
                {"BundlePath": "Lilu.kext", "Enabled": True, "MinKernel": ""},
                {"BundlePath": "XHCIPoke.kext", "Enabled": True, "MinKernel": "24.0.0",
                 "MaxKernel": ""},
            ],
            "Patch": [
                {"Comment": "something else", "Identifier": "com.apple.driver.AppleHDA",
                 "Enabled": True, "Find": b"\x01\x02", "Replace": b"\x03\x04"},
                {"Comment": OLD, "Identifier": xi.IDENT, "Enabled": True,
                 "MinKernel": "24.0.0", "MaxKernel": "24.99.99",
                 "Find": b"\xc7D\x08\x08\x00\x00@\x00", "Replace": b"\xc7D\x08\x08\x00\x00\x00\x00"},
            ],
        },
    }


def image(sites=xi.SITES, dup=None, include_patched=None):
    """A blob holding each Find once, with filler between them."""
    out = bytearray()
    for name, find, repl in sites:
        out += b"\x90" * 32 + bytes.fromhex(find)
        if dup == name:
            out += b"\x90" * 32 + bytes.fromhex(find)
        if include_patched == name:
            out += b"\x90" * 32 + bytes.fromhex(repl)
    return bytes(out)


def main():
    fails = 0

    def check(cond, what):
        nonlocal fails
        print(("ok   " if cond else "FAIL ") + what)
        fails += not cond

    with tempfile.TemporaryDirectory() as d:
        # --- verify ---------------------------------------------------------
        good = os.path.join(d, "good.bin")
        open(good, "wb").write(image())
        ok, _ = xi.verify(good)
        check(ok, "verify accepts an image with every Find exactly once")

        dup = os.path.join(d, "dup.bin")
        open(dup, "wb").write(image(dup=xi.SITES[0][0]))
        ok, lines = xi.verify(dup)
        check(not ok and any("count=2" in l for l in lines),
              "verify REFUSES a Find that occurs twice (OpenCore's Count 1 would be ambiguous)")

        short = os.path.join(d, "short.bin")
        open(short, "wb").write(image(sites=xi.SITES[1:]))
        ok, lines = xi.verify(short)
        check(not ok and any("count=0" in l for l in lines),
              "verify REFUSES a Find that is absent (the OS moved the site)")

        pre = os.path.join(d, "pre.bin")
        open(pre, "wb").write(image(include_patched=xi.SITES[0][0]))
        ok, lines = xi.verify(pre)
        check(not ok and any("already in the kext" in l for l in lines),
              "verify REFUSES an image that already contains the patched bytes")

        check(all(len(bytes.fromhex(f)) == len(bytes.fromhex(r)) and f != r
                  for _, f, r in xi.SITES),
              "every Replace is the same length as its Find and differs from it")
        check(all(bytes.fromhex(r).count(b"\x00\x00\x40\x00") == 0
                  and bytes.fromhex(r).count(b"\x00\x00\x80\x00") == 0
                  for n, f, r in xi.SITES if "shl" not in n),
              "no Replace still carries an interrupter 1 or 2 immediate")

        # --- config edits ---------------------------------------------------
        p = os.path.join(d, "config.plist")
        with open(p, "wb") as f:
            plistlib.dump(sample(), f, sort_keys=False)

        r = subprocess.run([sys.executable, TOOL, p, "add"], capture_output=True, text=True)
        cfg = plistlib.load(open(p, "rb"))
        lst = cfg["Kernel"]["Patch"]
        check(r.returncode == 0 and len(xi.ours(lst)) == len(xi.SITES),
              f"add puts {len(xi.SITES)} patches in Kernel -> Patch")
        check(any(p0.get("Comment") == OLD for p0 in lst),
              "add leaves the older Darwin 24 patches alone")
        check(all(lst[i]["MinKernel"] == xi.MIN_KERNEL and lst[i]["MaxKernel"] == xi.MAX_KERNEL
                  and lst[i]["Identifier"] == xi.IDENT and lst[i]["Count"] == 1
                  and lst[i]["Enabled"] is True for i in xi.ours(lst)),
              "every added patch is Darwin 25 only, Count 1, Enabled, on AppleUSBXHCI")

        subprocess.run([sys.executable, TOOL, p, "add"], capture_output=True, text=True)
        cfg = plistlib.load(open(p, "rb"))
        check(len(xi.ours(cfg["Kernel"]["Patch"])) == len(xi.SITES),
              "add twice is still one set (idempotent)")

        subprocess.run([sys.executable, TOOL, p, "poke", "off"], capture_output=True, text=True)
        cfg = plistlib.load(open(p, "rb"))
        pk = [e for e in cfg["Kernel"]["Add"] if e["BundlePath"] == xi.POKE][0]
        check(pk["Enabled"] is False and pk["MinKernel"] == "",
              "poke off disables XHCIPoke.kext and clears its stale MinKernel")

        subprocess.run([sys.executable, TOOL, p, "poke", "on"], capture_output=True, text=True)
        cfg = plistlib.load(open(p, "rb"))
        pk = [e for e in cfg["Kernel"]["Add"] if e["BundlePath"] == xi.POKE][0]
        check(pk["Enabled"] is True, "poke on re-enables XHCIPoke.kext")

        r = subprocess.run([sys.executable, TOOL, p, "remove"], capture_output=True, text=True)
        cfg = plistlib.load(open(p, "rb"))
        lst = cfg["Kernel"]["Patch"]
        check(r.returncode == 0 and not xi.ours(lst) and len(lst) == 2,
              "remove takes out only our patches")

        r = subprocess.run([sys.executable, TOOL, p, "status"], capture_output=True, text=True)
        check("0 of 9" in r.stdout and "older interrupter-0 patches: 1" in r.stdout,
              "status reports our patches and the older set separately")

        bad = os.path.join(d, "notoc.plist")
        with open(bad, "wb") as f:
            plistlib.dump({"Kernel": {"Add": []}}, f)
        r = subprocess.run([sys.executable, TOOL, bad, "add"], capture_output=True, text=True)
        check(r.returncode == 1 and "nothing changed" in r.stderr,
              "a config with no Kernel -> Patch is refused, nothing changed")

    print(("FAILED: %d" % fails) if fails else "all xhci_intr0 tests passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
