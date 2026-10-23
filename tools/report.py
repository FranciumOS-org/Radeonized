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

# FEATURE_*_BIT of docs/linux-ref/smu_v13_0_1_pmfw.h
SMU_FEATURES = {
    0: "CCLK_DPM",
    1: "FAN_CONTROLLER",
    2: "DATA_CALCULATION",
    3: "PPT",
    4: "TDC",
    5: "THERMAL",
    6: "FIT",
    7: "EDC",
    8: "PLL_POWER_DOWN",
    9: "ULV",
    10: "VDDOFF",
    11: "VCN_DPM",
    12: "CSTATE_BOOST",
    13: "FCLK_DPM",
    14: "SOCCLK_DPM",
    15: "MP0CLK_DPM",
    16: "LCLK_DPM",
    17: "SHUBCLK_DPM",
    18: "DCFCLK_DPM",
    19: "GFX_DPM",
    20: "DS_GFXCLK",
    21: "DS_SOCCLK",
    22: "DS_LCLK",
    23: "DS_DCFCLK",
    24: "DS_SHUBCLK",
    25: "GFX_TEMP_VMIN",
    26: "ZSTATES",
    27: "WHISPER_MODE",
    28: "DS_FCLK",
    29: "DS_SMNCLK",
    30: "DS_MP1CLK",
    31: "DS_MP0CLK",
    32: "CLK_LOW_POWER",
    33: "FUSE_PG",
    34: "GFX_DEM",
    35: "PSI",
    36: "PROCHOT",
    37: "CPUOFF",
    38: "STAPM",
    39: "S0I3",
    40: "DF_LIGHT_CSTATE",
    41: "PERF_LIMIT",
    42: "CORE_DLDO",
    43: "RSMU_LOW_POWER",
    44: "SMN_LOW_POWER",
    45: "THM_LOW_POWER",
    46: "SMUIO_LOW_POWER",
    47: "MP1_LOW_POWER",
    48: "DS_VCN",
    49: "CPPC",
    50: "CPPC_PREFERRED_CORES",
    51: "SMART_SHIFT",
    52: "DF_CSTATES",
    53: "MSMU_LOW_POWER",
    54: "SOC_VOLTAGE_MON",
    55: "ATHUB_PG",
    56: "VDDOFF_ECO",
    57: "ZSTATES_ECO",
    58: "CC6",
    59: "DS_UMCCLK",
    60: "DS_HSPCLK",
}


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
        # ioreg prints 32-bit OSNumbers as signed integers. 2oq: skip the
        # keys published as 64-bit, where masking to 32 bits would corrupt a
        # large value rather than repair a sign.
        wide = {"VSyncNanoseconds"}
        for k, v in p.items():
            if isinstance(v, int) and v < 0 and k not in wide:
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
            if i == 0 and ctl & 1:
                # 2oq: the detailed timing a published IOFramebuffer needs.
                # Blank/sync registers pack start in [15:0] and end in [31:16]
                # (dcn_3_1_2_sh_mask.h: OTG_H_BLANK_START, OTG_H_BLANK_END,
                # OTG_H_SYNC_A_START, OTG_H_SYNC_A_END).
                hb, vb = reg(regs, "OTG0_H_BLANK_START_END"), reg(regs, "OTG0_V_BLANK_START_END")
                hs, vs_ = reg(regs, "OTG0_H_SYNC_A"), reg(regs, "OTG0_V_SYNC_A")
                hsc, vsc = reg(regs, "OTG0_H_SYNC_A_CNTL"), reg(regs, "OTG0_V_SYNC_A_CNTL")
                pitch = reg(regs, "HUBP0_SURFACE_PITCH")
                lohi = lambda v: (v & 0xFFFF, (v >> 16) & 0xFFFF)
                if hb is not None and vb is not None:
                    print(f"  blank  H {lohi(hb)}  V {lohi(vb)}   (start, end)")
                if hs is not None and vs_ is not None:
                    pol = lambda c: "negative" if c is not None and c & 1 else "positive"
                    print(f"  sync   H {lohi(hs)} {pol(hsc)}  V {lohi(vs_)} {pol(vsc)}")
                if pitch is not None:
                    # DCSURF_SURFACE_PITCH: PITCH in [15:0], in pixels - 1
                    print(f"  pitch  {(pitch & 0xFFFF) + 1} px")

        # 2or: the backlight PWM, to say whether brightness is reachable.
        # dcn_3_1_2_sh_mask.h: BL_PWM_CNTL.BL_ACTIVE_INT_FRAC_CNT is [31:16],
        # BL_PWM_PERIOD_CNTL.BL_PWM_PERIOD is [31:16] and _BITCNT is [8:6].
        blc, blp = reg(regs, "BL_PWM_CNTL"), reg(regs, "BL_PWM_PERIOD_CNTL")
        if blc is not None and blp is not None:
            duty, period = (blc >> 16) & 0xFFFF, (blp >> 16) & 0xFFFF
            en = "enabled" if blc & (1 << 1) else "DISABLED"
            pct = f"{100.0 * duty / period:.1f}%" if period else "n/a"
            print(f"\nbacklight PWM: {en}, duty {duty}/{period} = {pct}")
            if not period:
                print("  period 0 - firmware did not set up this PWM; the panel "
                      "is driven some other way (ACPI, or PWRSEQ1)")

    # 2oq: the refresh rate, timed in the kext from OTG0's frame counter.
    # This is what the boot framebuffer cannot tell macOS (IOFB0Hz = Yes).
    state = p.get("VSyncState")
    if state:
        mhz = p.get("VSyncMilliHertz") or 0
        if mhz:
            print(f"\nrefresh: {mhz / 1000:.3f} Hz "
                  f"({p.get('VSyncFrames')} frames in {p.get('VSyncNanoseconds')} ns)")
        else:
            print(f"\nrefresh: NOT measured - {state} "
                  f"(frame counter {p.get('VSyncCounterFirst')} -> {p.get('VSyncCounterLast')})")

    smu = p.get("SMU")
    if smu:
        mask = lambda v: v & 0xFFFFFFFF if isinstance(v, int) else v
        print("\n== SMU queries")
        print(f"result: {smu['Result']}; MP1 {smu['MP1Version'] >> 16}.{(smu['MP1Version'] >> 8) & 0xff}."
              f"{smu['MP1Version'] & 0xff}; mailbox before: msg {mask(smu['InitialMessage']):#x} "
              f"arg {mask(smu['InitialArgument']):#x} resp {mask(smu['InitialResponse']):#x}")
        for line in smu.get("Messages", []):
            print("  " + line)
        if "SmuVersion" in smu:
            v = mask(smu["SmuVersion"])
            print(f"SMU firmware: program {v >> 24}, version {(v >> 16) & 0xff}.{(v >> 8) & 0xff}.{v & 0xff} ({v:#010x})")
        if "DriverIfVersion" in smu:
            iv = mask(smu["DriverIfVersion"])
            print(f"driver interface version {iv} (Linux yellow_carp expects 4: "
                  f"{'match' if iv == 4 else 'MISMATCH'})")
        if "EnabledFeatures" in smu:
            f = smu["EnabledFeatures"] & 0xFFFFFFFFFFFFFFFF
            on = [SMU_FEATURES.get(b, f"bit{b}") for b in range(64) if f >> b & 1]
            print(f"enabled features {f:#018x} ({len(on)}):")
            for i in range(0, len(on), 4):
                print("  " + ", ".join(on[i:i + 4]))
            print(f"graphics clock DPM (GFX_DPM): {'on' if f >> 19 & 1 else 'off'}")
        print(f"mailbox writes {smu['Writes']}, refused {smu['RefusedWrites']}"
              + ("  <-- should be 0!" if smu["RefusedWrites"] else ""))

    print(f"\nsaved: {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
