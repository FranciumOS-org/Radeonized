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
        "Misc": {"Debug": {"AppleDebug": True, "SysReport": False, "Target": 67}},
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

        # 2xw: `bootarg strip` removes exactly the rb_defaults.h table
        with open(p, "wb") as f:
            cfg = sample()
            cfg["NVRAM"]["Add"][ec.BOOT_GUID]["boot-args"] = (
                "keepsyms=1 -rbgpufbhubp wdt=-1 rbgpumtlkscope=* -rbgpunofbaty "
                "-wegnoegpu -rbgpuschedtick -rbgpunogates -rbgpugates "
                "rbgpuschedtickus=2000 -rbgpustock -rbgpuagdcearly")
            plistlib.dump(cfg, f, sort_keys=False)
        r = run(p, "bootarg", "strip")
        after = plistlib.load(open(p, "rb"))["NVRAM"]["Add"][ec.BOOT_GUID]["boot-args"].split()
        check(r.returncode == 0 and "-rbgpufbhubp" not in after
              and "-rbgpuschedtick" not in after and "-rbgpunogates" not in after,
              "strip removes exactly the default-on tokens, the negative-named ones too")
        check("keepsyms=1" in after and "wdt=-1" in after and "-wegnoegpu" in after,
              "strip keeps everything that is not ours, wdt=-1 above all (2hh)")
        check("-rbgpunofbaty" in after and "-rbgpustock" in after
              and "-rbgpugates" in after,
              "strip keeps every DISARM, including the positive form of a "
              "negative-named default (-rbgpugates)")
        check("rbgpuschedtickus=2000" in after and "rbgpumtlkscope=*" in after,
              "strip keeps tunables that carry a value")
        check("-rbgpuagdcearly" in after,
              "strip keeps an opt-in experiment it did not put there")
        check("check them" in r.stdout and "rbgpumtlkscope" in r.stdout,
              "strip REPORTS every rbgpu token it kept")
        r2 = run(p, "bootarg", "strip")
        check(r2.returncode == 0 and "nothing of ours left" in r2.stdout,
              "a second strip is a no-op and says so")
        check(len(ec.default_tokens()) == 45,
              "the table is read from src/common/rb_defaults.h, not copied here")

        with open(p, "wb") as f:
            plistlib.dump(sample(), f, sort_keys=False)

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

        plistlib.dump(sample(), open(p, "wb"), sort_keys=False)
        r = run(p, "sysreport", "on")
        cfg = plistlib.load(open(p, "rb"))
        check(r.returncode == 0 and cfg["Misc"]["Debug"]["SysReport"] is True, "sysreport on")
        check(cfg["Misc"]["Debug"]["Target"] == 67 and cfg["Misc"]["Debug"]["AppleDebug"] is True,
              "sysreport leaves the other Debug keys")
        check("SysReport: True" in run(p, "status").stdout, "status shows SysReport")
        run(p, "sysreport", "off")
        check(plistlib.load(open(p, "rb")) == sample(), "sysreport off restores the original exactly")
        nodbg = sample(); del nodbg["Misc"]["Debug"]["SysReport"]
        plistlib.dump(nodbg, open(p, "wb"), sort_keys=False)
        before = open(p, "rb").read()
        r = run(p, "sysreport", "on")
        check(r.returncode == 1 and open(p, "rb").read() == before, "no SysReport key: refused, unchanged")
        check(run(p, "sysreport", "maybe").returncode == 2, "sysreport needs on or off")

        r = run(p, "frobnicate")
        check(r.returncode == 2, "unknown command refused")

    print("all passed" if not fails else f"{fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
