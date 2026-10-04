#!/usr/bin/env python3
"""Read the probe's results from the IORegistry, save them, and explain them.

    python3 tools/report.py            # after sudo tools/load.sh (it runs this too)

Writes logs/<timestamp>/probe.plist and discovery.bin, prints the IP table
through build/out/rbdump, and decodes the registers that answer the questions
of step 0: is the PSP up, does the SMU answer, where is the carve-out, and
(level 2) which display pipe is lit.
"""
import datetime
import os
import plistlib
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def reg(regs, name):
    v = regs.get(name, "")
    m = re.match(r"0x([0-9a-f]{8})", v)
    return int(m.group(1), 16) if m else None


def main():
    out = subprocess.run(["ioreg", "-a", "-r", "-c", "RembrandtProbe", "-d", "1"],
                         capture_output=True)
    if not out.stdout.strip():
        print("RembrandtProbe is not in the IORegistry: not loaded, or start() failed "
              "(see the kernel messages above).")
        return 1
    nodes = plistlib.loads(out.stdout)
    p = nodes[0].get("RBProbe") if nodes else None
    if p:
        # ioreg prints 32-bit OSNumbers as signed integers.
        for k, v in p.items():
            if isinstance(v, int) and v < 0:
                p[k] = v & 0xFFFFFFFF
    if not p:
        print("RembrandtProbe is attached but published no RBProbe results.")
        return 1

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = os.path.join(ROOT, "logs", stamp)
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(run_dir, "probe.plist"), "wb") as f:
        plistlib.dump({k: v for k, v in p.items()}, f)
    bin_path = None
    if p.get("DiscoveryBinary"):
        bin_path = os.path.join(run_dir, "discovery.bin")
        with open(bin_path, "wb") as f:
            f.write(p["DiscoveryBinary"])

    print(f"== RembrandtProbe report ({run_dir})")
    print(f"device {p['PCIVendor']:04x}:{p['PCIDevice']:04x} rev {p['PCIRevision']:02x}, "
          f"level {p['Level']}, result: {p['Result']}")
    print(f"carve-out (RCC_CONFIG_MEMSIZE): {p['MemsizeMB']} MB")
    print(f"discovery table: {p['DiscoverySource']}, offset {p['DiscoveryOffset']:#x}, "
          f"{p['DiscoverySize']} bytes, BAR0 cross-check: {p['BAR0CrossCheck']}")
    c33 = p["MP0_C2PMSG_33_fixed"]
    print(f"IFWI init (C2PMSG_33 bit 31): {'done' if c33 & 0x80000000 else 'NOT done'} ({c33:#x})")
    print(f"MM_INDEX writes {p['IndexWrites']}, refused writes {p['RefusedWrites']}"
          + ("  <-- should be 0!" if p["RefusedWrites"] else ""))

    if bin_path and os.path.exists(os.path.join(ROOT, "build/out/rbdump")):
        print("\n== IP discovery table", flush=True)
        subprocess.run([os.path.join(ROOT, "build/out/rbdump"), bin_path])

    regs = p.get("Registers", {})
    if regs:
        print("\n== registers")
        for k in sorted(regs):
            print(f"  {k:28} {regs[k]}")

        print("\n== what they say")
        v35, v81 = reg(regs, "MP0_C2PMSG_35"), reg(regs, "MP0_C2PMSG_81")
        if v35 == 0xFFFFFFFF:
            print("PSP C2PMSG_35 reads all-ones: not readable from the host here "
                  "(Linux only polls it when the secure OS is not running)")
        elif v35 is not None:
            print(f"PSP bootloader ready (C2PMSG_35 bit 31): {'yes' if v35 & 0x80000000 else 'no'}")
        if v81 is not None:
            print(f"PSP secure OS running (C2PMSG_81 != 0): {'yes' if v81 else 'no'} ({v81:#x}; "
                  "changes between runs, so not a version) - Linux skips loading the PSP's OS when set")
        v90 = reg(regs, "MP1_C2PMSG_90")
        if v90 is not None:
            meaning = {0: "no response yet / busy", 1: "OK", 0xFF: "failed",
                       0xFE: "unknown command", 0xFD: "command rejected (prereq)",
                       0xFC: "command rejected (busy)"}.get(v90, "other")
            print(f"SMU last response (C2PMSG_90): {v90:#x} = {meaning}; "
                  f"last message {reg(regs, 'MP1_C2PMSG_66')}, argument {reg(regs, 'MP1_C2PMSG_82')}")
        fb_base, fb_top = reg(regs, "MMMC_VM_FB_LOCATION_BASE"), reg(regs, "MMMC_VM_FB_LOCATION_TOP")
        fb_off = reg(regs, "MMMC_VM_FB_OFFSET")
        if fb_base is not None and fb_top is not None:
            b, t = (fb_base & 0xFFFFFF) << 24, (fb_top & 0xFFFFFF) << 24
            print(f"carve-out in GPU address space: {b:#x} - {t + (1 << 24) - 1:#x} "
                  f"({((t - b) >> 20) + 16} MB)")
        if fb_off is not None:
            print(f"carve-out in system RAM (FB_OFFSET): {(fb_off & 0xFFFFFF) << 24:#x}")
        cntl, sc0 = reg(regs, "DMCUB_CNTL"), reg(regs, "DMCUB_SCRATCH0")
        if cntl is not None:
            # dcn_3_1_2_sh_mask.h; SCRATCH0 is union dmub_fw_boot_status (dmub_cmd.h)
            print(f"display microcontroller (DMCUB): {'enabled' if cntl & 0x10000 else 'disabled'}"
                  f"{', idle-waiting' if cntl & 0x100000 else ''}")
        if sc0 is not None:
            bits = [n for b, n in [(0, "driver (DAL) firmware"), (1, "mailbox ready"),
                                   (2, "optimized init done"), (3, "restore required"),
                                   (7, "hw power init done")] if sc0 & (1 << b)]
            who = "the driver's" if sc0 & 1 else "the firmware's (GOP/VBIOS)"
            print(f"DMCUB boot status {sc0:#x}: {', '.join(bits) or 'none'} -> running {who} DMCUB image")
        for i in range(4):
            ctl = reg(regs, f"OTG{i}_CONTROL")
            if ctl is None:
                continue
            h, v = reg(regs, f"OTG{i}_H_TOTAL"), reg(regs, f"OTG{i}_V_TOTAL")
            lo, hi = reg(regs, f"HUBP{i}_PRIMARY_ADDR"), reg(regs, f"HUBP{i}_PRIMARY_ADDR_HI")
            on = "ON " if ctl & 1 else "off"
            extra = ""
            if ctl & 1 and h is not None and v is not None:
                extra = f" total {(h & 0x7FFF) + 1}x{(v & 0x7FFF) + 1}"
            if lo is not None and hi is not None:
                extra += f", HUBP{i} surface {((hi & 0xFFFF) << 32) | lo:#x}"
            cfg = reg(regs, f"HUBP{i}_SURFACE_CONFIG")
            if ctl & 1 and cfg is not None:
                fmt = {8: "ARGB8888", 9: "ABGR8888", 10: "ARGB2101010"}.get(cfg & 0x7F, f"format {cfg & 0x7F}")
                extra += f" ({fmt})"
            print(f"OTG{i} {on}{extra}")

    print(f"\nsaved: {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
