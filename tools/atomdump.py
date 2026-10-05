#!/usr/bin/env python3
"""Summarise an AMD ATOM firmware VBIOS image (atomfirmware.h layouts):
ROM header, boot message, and every data / command table present with its
size and format.content revision.

    python3 tools/atomdump.py firmware/680m-vbios.rom
"""
import struct
import sys

DATA_TABLES = [
    "utilitypipeline", "multimedia_info", "smc_dpm_info", "sw_datatable3", "firmwareinfo",
    "sw_datatable5", "lcd_info", "sw_datatable7", "smu_info", "sw_datatable9", "sw_datatable10",
    "vram_usagebyfirmware", "gpio_pin_lut", "sw_datatable13", "gfx_info", "powerplayinfo",
    "sw_datatable16", "sw_datatable17", "sw_datatable18", "sw_datatable19", "sw_datatable20",
    "sw_datatable21", "displayobjectinfo", "indirectioaccess", "umc_info", "sw_datatable25",
    "sw_datatable26", "dce_info", "vram_info", "sw_datatable29", "integratedsysteminfo",
    "asic_profiling_info", "voltageobject_info", "sw_datatable33", "sw_datatable34",
]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def cstr(b, o, n=120):
    e = b.find(b"\0", o, o + n)
    return b[o:e if e >= 0 else o + n].decode("latin-1").strip()


def table_header(b, o):
    if not o or o + 4 > len(b):
        return None
    return u16(b, o), b[o + 2], b[o + 3]


def main():
    rom = open(sys.argv[1], "rb").read()
    assert rom[:2] == b"\x55\xaa", "no 55AA"
    h = u16(rom, 0x48)
    size, frev, crev = table_header(rom, h)
    print(f"ROM {len(rom)} bytes, ATOM ROM header at {h:#x} v{frev}.{crev} ({size} bytes), magic {rom[h+4:h+8]!r}")
    (seg, pmode, cfgname, crc, bootmsg, int10, pcidev, iobase, ssvid, ssid, pciinfo,
     master_cmd, master_data) = struct.unpack_from("<13H", rom, h + 8)
    print(f"  subsystem {ssvid:04x}:{ssid:04x}, PCI info at {pciinfo:#x}")
    print(f"  boot message: {cstr(rom, bootmsg)!r}")
    if size >= 0x26 + 4:
        print(f"  PSP directory offset: {struct.unpack_from('<I', rom, h + 0x22)[0]:#x}")
    # Part number / version strings as Linux's atom_get_vbios_* read them: the
    # first strings after the ROM header area.
    for label, off in (("config file name", cfgname),):
        if off:
            print(f"  {label}: {cstr(rom, off)!r}")
    pn_off = 0x80
    print(f"  string at 0x80: {cstr(rom, pn_off)!r}")
    print(f"  string at 0x94 (Linux: version at 0x94 area): {cstr(rom, 0x94)!r}")

    print(f"\ndata tables (master list at {master_data:#x}):")
    msize, mf, mc = table_header(rom, master_data)
    n = (msize - 4) // 2
    for i in range(min(n, len(DATA_TABLES))):
        off = u16(rom, master_data + 4 + 2 * i)
        if not off:
            continue
        th = table_header(rom, off)
        print(f"  {i:2d} {DATA_TABLES[i]:24s} at {off:#06x}  {th[0]:5d} bytes  v{th[1]}.{th[2]}")
    csize, _, _ = table_header(rom, master_cmd)
    ncmd = (csize - 4) // 2
    present = sum(1 for i in range(ncmd) if u16(rom, master_cmd + 4 + 2 * i))
    print(f"\ncommand tables: {present} of {ncmd} present (master list at {master_cmd:#x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
