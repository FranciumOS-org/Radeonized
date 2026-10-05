#!/usr/bin/env python3
"""Tests for efi_config.py against a synthetic OpenCore config.

    python3 tools/efi_config_test.py
"""
import copy
import os
import plistlib
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import efi_config as ec  # noqa: E402

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "efi_config.py")


def kext(name, enabled=True):
    return {"Arch": "x86_64", "BundlePath": name, "Comment": "", "Enabled": enabled,
            "ExecutablePath": f"Contents/MacOS/{name[:-5]}", "MaxKernel": "", "MinKernel": "",
            "PlistPath": "Contents/Info.plist"}


def sample():
    return {
        "ACPI": {"Add": [], "Patch": [{"Find": b"\x01\x02", "Count": 0}]},
        "Kernel": {"Add": [kext("Lilu.kext"), kext("VirtualSMC.kext"), kext("WhateverGreen.kext")]},
        "NVRAM": {
            "Add": {ec.BOOT_GUID: {"boot-args": "debug=0x100 keepsyms=1 -wegnoegpu", "csr-active-config": b"\x03\x0a\x00\x00"}},
            "Delete": {ec.BOOT_GUID: ["boot-args", "csr-active-config"]},
        },
        "PlatformInfo": {"Generic": {"SystemSerialNumber": "X", "ROM": b"\0" * 6}},
    }


def names(cfg):
    return [e["BundlePath"] for e in cfg["Kernel"]["Add"]]


def run(path, *args):
    return subprocess.run([sys.executable, TOOL, path, *args], capture_output=True, text=True)


def main():
    fails = 0

    def check(cond, what):
        nonlocal fails
        print(("ok   " if cond else "FAIL ") + what)
        fails += not cond

    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "config.plist")
        orig = sample()
        with open(p, "wb") as f:
            plistlib.dump(orig, f, sort_keys=False)

        r = run(p, "add-kext")
        cfg = plistlib.load(open(p, "rb"))
        check(r.returncode == 0, "add-kext succeeds")
        check(names(cfg) == ["Lilu.kext", "RembrandtGPU.kext", "VirtualSMC.kext", "WhateverGreen.kext"],
              "entry lands right after Lilu")
        check(cfg["Kernel"]["Add"][1] == ec.ENTRY, "entry fields are the plugin's")
        rest = copy.deepcopy(cfg)
        del rest["Kernel"]["Add"][1]
        check(rest == orig, "nothing else changed (data, order, other keys)")
        check(list(cfg) == list(orig), "top-level key order kept")

        run(p, "add-kext")
        cfg = plistlib.load(open(p, "rb"))
        check(names(cfg).count("RembrandtGPU.kext") == 1, "add-kext twice keeps one entry")

        # An entry placed before Lilu (by hand) is moved after it.
        cfg["Kernel"]["Add"].insert(0, cfg["Kernel"]["Add"].pop(1))
        plistlib.dump(cfg, open(p, "wb"), sort_keys=False)
        check("BEFORE Lilu" in run(p, "status").stdout, "status flags wrong order")
        run(p, "add-kext")
        cfg = plistlib.load(open(p, "rb"))
        check(names(cfg)[:2] == ["Lilu.kext", "RembrandtGPU.kext"], "add-kext fixes the order")

        r = run(p, "bootarg", "add", "-rbgpuoff")
        cfg = plistlib.load(open(p, "rb"))
        check(cfg["NVRAM"]["Add"][ec.BOOT_GUID]["boot-args"] == "debug=0x100 keepsyms=1 -wegnoegpu -rbgpuoff",
              "bootarg add appends, keeps the others")
        run(p, "bootarg", "add", "-rbgpuoff")
        cfg = plistlib.load(open(p, "rb"))
        check(cfg["NVRAM"]["Add"][ec.BOOT_GUID]["boot-args"].count("-rbgpuoff") == 1, "bootarg add twice is a no-op")
        run(p, "bootarg", "remove", "-rbgpuoff")
        run(p, "remove-kext")
        cfg = plistlib.load(open(p, "rb"))
        check(cfg == orig, "remove-kext + bootarg remove restore the original exactly")

        # Refusals leave the file untouched.
        bad = sample()
        bad["Kernel"]["Add"][0]["Enabled"] = False
        plistlib.dump(bad, open(p, "wb"), sort_keys=False)
        before = open(p, "rb").read()
        r = run(p, "add-kext")
        check(r.returncode == 1 and open(p, "rb").read() == before, "no enabled Lilu: refused, file unchanged")
        check(not os.path.exists(p + ".rbgpu-new"), "no temp file left behind")

        nodelete = sample()
        del nodelete["NVRAM"]["Delete"]
        plistlib.dump(nodelete, open(p, "wb"), sort_keys=False)
        check("WARNING" in run(p, "status").stdout, "status warns when boot-args is not in NVRAM -> Delete")

        r = run(p, "frobnicate")
        check(r.returncode == 2, "unknown command refused")

    print("all passed" if not fails else f"{fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
