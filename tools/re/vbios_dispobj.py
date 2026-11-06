#!/usr/bin/env python3
"""Parse the VBIOS display-object-info table out of a 680M ROM dump.

2E1. The DMUB DIG transmitter command's `phyid` and `hpdsel` fields are not
readable from any register - 2BF established that by exhaustion - they come
from this table. Every structure and every translation below cites the
vendored file it was read from; nothing here is recalled.

  docs/linux-ref/atomfirmware.h       atom_rom_header_v2_2 (:243),
                                      atom_master_list_of_data_tables_v2_1
                                      (:390), display_object_info_table_v1_4
                                      (:1037), atom_display_object_path_v2
                                      (:1011), atom_hpd_int_record (:837),
                                      atom_i2c_record (:830),
                                      OFFSET_TO_ATOM_ROM_HEADER_POINTER (:217)
  grph_object_id.h (upstream)         objid layout: id bits 0-7, enum_id
                                      bits 8-11, type bits 12-15
                                      (dal_graphics_object_id_to_uint)
  link_factory.c (upstream)           translate_encoder_to_transmitter(), and
                                      `link->hpd_src = pin_id - 1`
  command_table_helper2_dce112.c:63   hpd_sel_to_atom: hpd_src N -> N+1
  command_table_helper.c:297          phy_id_to_atom: identity on UNIPHY A..G

So the two payload bytes are, for a given connector:
    phyid  = transmitter index (A=0 .. G=6)
    hpdsel = hpd_int_record.pin_id          (because pin_id-1 is hpd_src and
                                             hpd_sel_to_atom adds one back)

Usage: tools/re/vbios_dispobj.py firmware/680m-vbios.rom [--cmdtable]

--cmdtable prints the atom COMMAND table instead, which is what decides the
transmitter payload layout. 2E2 reads it:

  atom.c:amdgpu_atom_parse_cmd_header is
      idx  = CU16(cmd_table + 4 + 2*index)      cmd_table = CU16(rom+0x1E)
      frev = CU8(idx + 2)   crev = CU8(idx + 3)
  with index from GET_INDEX_INTO_MASTER_TABLE(command, dig1transmittercontrol),
  i.e. the field's position in atom_master_list_of_command_functions_v2_1,
  which is 76 (atomfirmware.h:272 counted field by field; digxencodercontrol
  is 4, setpixelclock 12, enabledisppowergating 13).
"""

import struct
import sys

# grph_object_id.h: enum object_type
OBJ_TYPE = {0: "UNKNOWN", 1: "GPU", 2: "ENCODER", 3: "CONNECTOR",
            4: "ROUTER", 5: "GENERIC"}

# grph_object_id.h: enum connector_id
CONNECTOR_ID = {0: "UNKNOWN", 1: "SINGLE_LINK_DVII", 2: "DUAL_LINK_DVII",
                3: "SINGLE_LINK_DVID", 4: "DUAL_LINK_DVID", 5: "VGA",
                12: "HDMI_TYPE_A", 14: "LVDS", 16: "PCIE",
                18: "HARDCODE_DVI", 19: "DISPLAY_PORT", 20: "EDP",
                21: "MXM", 22: "WIRELESS", 23: "MIRACAST", 24: "USBC",
                100: "VIRTUAL"}

# ObjectID.h (drivers/gpu/drm/amd/amdgpu/ObjectID.h): the ATOM encoder
# object ids that actually appear in a ROM. These are NOT dc's
# `enum encoder_id` values - bios_parser_common.c:encoder_id_from_bios_object_id
# is a translation SWITCH, not a cast, and dc's enum is a dense 0..21 list
# while ATOM's is sparse. Reading dc's enum as if it were the ROM encoding
# makes every UNIPHY an unknown encoder.
ATOM_ENCODER_ID = {0x1E: "INTERNAL_UNIPHY", 0x1F: "INTERNAL_KLDSCP_LVTMA",
                   0x20: "INTERNAL_UNIPHY1", 0x21: "INTERNAL_UNIPHY2",
                   0x25: "INTERNAL_UNIPHY3"}

# ObjectID.h:118-122
ATOM_CONNECTOR_ID = {0x01: "SINGLE_LINK_DVI_I", 0x02: "DUAL_LINK_DVI_I",
                     0x03: "SINGLE_LINK_DVI_D", 0x04: "DUAL_LINK_DVI_D",
                     0x05: "VGA", 0x0C: "HDMI_TYPE_A", 0x0E: "LVDS",
                     0x12: "HARDCODE_DVI", 0x13: "DISPLAYPORT",
                     0x14: "eDP", 0x15: "MXM", 0x17: "USBC"}

# ObjectID.h:34-41
ATOM_OBJ_TYPE = {0x0: "NONE", 0x1: "GPU", 0x2: "ENCODER", 0x3: "CONNECTOR",
                 0x4: "ROUTER", 0x6: "DISPLAY_PATH", 0x7: "GENERIC"}

# link_factory.c:323 translate_encoder_to_transmitter(), transcribed exactly.
# Key is (encoder_id, enum_id); value is the transmitter letter, whose index
# IS the atom phyid (command_table_helper.c:297 is the identity map, and
# atombios.h:1496 defines ATOM_PHY_ID_UNIPHYA..G as 0..6).
# Keyed on (ATOM encoder object id, enum_id) - the ROM's own encoding -
# with the dc enum folded in, so the two translations are composed once:
#   ObjectID.h          UNIPHY 0x1E, UNIPHY1 0x20, UNIPHY2 0x21, UNIPHY3 0x25
#   bios_parser_common  -> ENCODER_ID_INTERNAL_UNIPHY / UNIPHY1 / 2 / 3
#   link_factory.c:323  -> TRANSMITTER_UNIPHY_A..G
ENC_TO_TRANSMITTER = {
    (0x1E, 1): "A", (0x1E, 2): "B",
    (0x20, 1): "C", (0x20, 2): "D",
    (0x21, 1): "E", (0x21, 2): "F",
    (0x25, 1): "G",
}
TRANSMITTER_INDEX = {c: i for i, c in enumerate("ABCDEFG")}

ATOM_I2C_RECORD_TYPE = 1          # atomfirmware.h enum, :808-821
ATOM_HPD_INT_RECORD_TYPE = 2
ATOM_ENCODER_CAP_RECORD_TYPE = 20
ATOM_RECORD_END_TYPE = 0xFF

# atomfirmware.h:858-869 enum atom_encoder_caps_def
ENCODER_CAPS = [(0x001, "MST_EN/HBR2"), (0x002, "HBR2_EN"),
                (0x004, "HDMI6Gbps_EN"), (0x008, "HBR3_EN"),
                (0x010, "DP2"), (0x020, "UHBR10_EN"),
                (0x040, "UHBR13_5_EN"), (0x080, "UHBR20_EN"),
                (0x100, "USB_C_TYPE")]


def decode_objid(v):
    return v & 0xFF, (v >> 8) & 0xF, (v >> 12) & 0xF


# atom_master_list_of_command_functions_v2_1 (atomfirmware.h:272), only the
# entries this project has a reason to ask about. Counted field by field from
# the vendored struct; the internal cmd_functionN slots are unnamed there and
# are printed by index.
CMD_INDEX = {0: "asic_init", 4: "digxencodercontrol", 10: "setengineclock",
             11: "setmemoryclock", 12: "setpixelclock",
             13: "enabledisppowergating", 34: "blankcrtc", 35: "enablecrtc",
             41: "getsmuclockinfo", 76: "dig1transmittercontrol"}


def cmdtable(rom):
    """The atom COMMAND table, which decides the transmitter payload layout."""
    rom_hdr = struct.unpack_from("<H", rom, 0x48)[0]
    # atom.h ATOM_ROM_CMD_PTR 0x1E / ATOM_ROM_DATA_PTR 0x20, which are
    # masterhwfunction_offset and masterdatatable_offset of
    # atom_rom_header_v2_2 counted field by field.
    cmd = struct.unpack_from("<H", rom, rom_hdr + 0x1E)[0]
    size, frev, crev = struct.unpack_from("<HBB", rom, cmd)
    n = (size - 4) // 2
    print(f"declared image size {rom[2] * 512} bytes, file {len(rom)} bytes"
          + ("" if rom[2] * 512 == len(rom) else "   !! TRUNCATED"))
    print(f"cmd_table 0x{cmd:04x}  header size {size}  rev {frev}.{crev}"
          f"  -> {n} entries")
    ents = struct.unpack_from(f"<{n}H", rom, cmd + 4)
    print()
    for i, off in enumerate(ents):
        name = CMD_INDEX.get(i)
        if off == 0:
            if name:
                print(f"  {i:3d} {name:<24s} ABSENT - master table entry is 0,"
                      f" so amdgpu_atom_parse_cmd_header returns false and"
                      f" crev stays 0")
            continue
        tsize, tfrev, tcrev = struct.unpack_from("<HBB", rom, off)
        print(f"  {i:3d} {name or '(internal)':<24s} table 0x{off:04x}"
              f"  size {tsize:<5d} frev {tfrev}  crev {tcrev}")
    print()
    print("A zero entry means there is no atom bytecode for that function on")
    print("this board, so the driver's crev is 0 and command_table2.c's")
    print("switch falls to the *_fallback function. For the transmitter that")
    print("fallback is four lines and hardcodes v1_7 over DMUB.")
    return 0


def main():
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        sys.exit(__doc__)
    rom = open(sys.argv[1], "rb").read()
    if len(sys.argv) == 3:
        if sys.argv[2] != "--cmdtable":
            sys.exit(__doc__)
        return cmdtable(rom)

    # atomfirmware.h:217 OFFSET_TO_ATOM_ROM_HEADER_POINTER = 0x48
    rom_hdr = struct.unpack_from("<H", rom, 0x48)[0]
    sig = rom[rom_hdr + 4:rom_hdr + 8]
    print(f"rom header   0x{rom_hdr:04x}  signature {sig!r}")
    if sig != b"ATOM":
        sys.exit("not an atombios image: signature is not 'ATOM'")

    # atom_rom_header_v2_2: table_header is 4 bytes, then atom_bios_string[4],
    # then 12 uint16 before masterdatatable_offset at +0x20 from the string.
    # Counted field by field from atomfirmware.h:243.
    off = rom_hdr + 4 + 4
    names = ["bios_segment_address", "protectedmodeoffset",
             "configfilenameoffset", "crc_block_offset",
             "vbios_bootupmessageoffset", "int10_offset", "pcibusdevinitcode",
             "iobaseaddress", "subsystem_vendor_id", "subsystem_id",
             "pci_info_offset", "masterhwfunction_offset",
             "masterdatatable_offset"]
    fields = dict(zip(names, struct.unpack_from("<13H", rom, off)))
    mdt = fields["masterdatatable_offset"]
    print(f"master data  0x{mdt:04x}")

    # atom_master_data_table_v2_1: 4-byte header, then the list. displayobjectinfo
    # is the 23rd uint16 of atom_master_list_of_data_tables_v2_1 (index 22).
    doi = struct.unpack_from("<H", rom, mdt + 4 + 22 * 2)[0]
    print(f"displayobjectinfo 0x{doi:04x}")

    # atom_common_table_header: format_revision, content_revision are the
    # last two bytes of the 4-byte header (structure_size is the first u16).
    size, fmt_rev, cont_rev = struct.unpack_from("<HBB", rom, doi)
    supported, n_path, _ = struct.unpack_from("<HBB", rom, doi + 4)
    print(f"  table size {size}  rev {fmt_rev}.{cont_rev}  "
          f"supporteddevices 0x{supported:04x}  paths {n_path}")
    if cont_rev != 4:
        print(f"  !! this parser transcribes display_object_info_table_v1_4; "
              f"content revision is {cont_rev}, REFUSING", file=sys.stderr)
        return 2

    print()
    for i in range(n_path):
        p = doi + 8 + i * 16          # atom_display_object_path_v2 is 16 bytes
        (disp_objid, rec_off, enc_objid, ext_enc_objid,
         enc_rec_off, ext_rec_off, device_tag, prio, _r) = \
            struct.unpack_from("<7H2B", rom, p)

        cid, cenum, ctype = decode_objid(disp_objid)
        eid, eenum, etype = decode_objid(enc_objid)

        conn = ATOM_CONNECTOR_ID.get(cid, f"?0x{cid:02x}") if ctype == 3 \
            else f"{ATOM_OBJ_TYPE.get(ctype, ctype)}:0x{cid:02x}"
        enc = ATOM_ENCODER_ID.get(eid, f"?0x{eid:02x}")
        tx = ENC_TO_TRANSMITTER.get((eid, eenum))

        print(f"path {i}: connector {conn} enum {cenum}  "
              f"(objid 0x{disp_objid:04x}, device_tag 0x{device_tag:04x}, "
              f"priority {prio})")
        print(f"         encoder   {enc} enum {eenum} "
              f"(objid 0x{enc_objid:04x})"
              + (f"  -> TRANSMITTER_UNIPHY_{tx}, atom phyid "
                 f"{TRANSMITTER_INDEX[tx]}" if tx else
                 "  -> TRANSMITTER_UNKNOWN"))
        if ext_enc_objid:
            print(f"         ext encoder objid 0x{ext_enc_objid:04x}")

        # Records live at disp_recordoffset + the object-info table offset
        # (bios_parser2.c:get_hpd_record).
        off = doi + rec_off
        pin_id = None
        i2c = []
        while True:
            rtype, rsize = rom[off], rom[off + 1]
            if rtype == ATOM_RECORD_END_TYPE or rsize == 0:
                break
            if rtype == ATOM_HPD_INT_RECORD_TYPE and rsize >= 4:
                pin_id = rom[off + 2]
            if rtype == ATOM_I2C_RECORD_TYPE and rsize >= 4:
                i2c.append((rom[off + 2], rom[off + 3]))
            off += rsize

        if pin_id is None:
            print("         hpd       no ATOM_HPD_INT_RECORD")
        else:
            # link_factory.c:603 link->hpd_src = pin_id - 1;
            # command_table_helper2_dce112.c:63 hpd_sel_to_atom adds 1 back.
            print(f"         hpd       pin_id {pin_id}  -> hpd_src "
                  f"{pin_id - 1} (DCN HPD block instance {pin_id - 1})"
                  f"  -> atom hpdsel {pin_id}")
        for iid, slave in i2c:
            print(f"         i2c       id 0x{iid:02x} slave 0x{slave:02x}"
                  f"  (low nibble {iid & 0xf} = DDC/AUX engine instance)")

        # bios_parser2.c:get_encoder_cap_record walks encoder_recordoffset,
        # same base. DP_IS_USB_C lives here, NOT in the connector object id.
        caps = None
        off = doi + enc_rec_off
        while True:
            rtype, rsize = rom[off], rom[off + 1]
            if rtype == ATOM_RECORD_END_TYPE or rsize == 0:
                break
            if rtype == ATOM_ENCODER_CAP_RECORD_TYPE and rsize >= 6:
                caps = struct.unpack_from("<I", rom, off + 2)[0]
            off += rsize
        if caps is None:
            print("         enc caps  no ATOM_ENCODER_CAP_RECORD")
        else:
            names = [n for bit, n in ENCODER_CAPS if caps & bit]
            print(f"         enc caps  0x{caps:08x}  "
                  + (" ".join(names) if names else "(none set)"))
            print(f"         DP_IS_USB_C "
                  f"{'1' if caps & 0x100 else '0'}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
