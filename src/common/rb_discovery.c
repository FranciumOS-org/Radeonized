/* SPDX-License-Identifier: MIT */
/* AMD IP discovery table parser. See rb_discovery.h. */
#include "rb_discovery.h"

/* Little-endian loads that never read past `len`. The caller checks the range
 * first with in_bounds(); these only assemble bytes (no unaligned access). */
static uint16_t ld16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int in_bounds(uint32_t off, uint32_t size, uint32_t len)
{
    return off <= len && size <= len - off;
}

static uint16_t byte_sum(const uint8_t *p, uint32_t n)
{
    uint16_t s = 0;
    for (uint32_t i = 0; i < n; i++)
        s = (uint16_t)(s + p[i]);
    return s;
}

static void zero(void *p, uint32_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++)
        b[i] = 0;
}

/* binary_header (v0/v1): signature u32, major u16, minor u16, checksum u16,
 * size u16, then 6 x table_info{offset u16, checksum u16, size u16, pad u16}.
 * binary_header_v2: the same 12 bytes, then num_tables u16, pad u16, then the
 * table_info list. */
#define BHDR_FIXED        12u
#define TABLE_INFO_SIZE   8u
enum { T_IP = 0, T_GC, T_HARVEST, T_VCN, T_MALL, T_NPS, T_TOTAL };

struct tinfo { uint16_t offset, checksum; };

static int table_info(const uint8_t *bin, uint32_t len, uint16_t major, unsigned id, struct tinfo *ti)
{
    uint32_t list, count;
    if (major == 2) {
        if (!in_bounds(BHDR_FIXED, 4, len))
            return -1;
        count = ld16(bin + BHDR_FIXED);
        list = BHDR_FIXED + 4;
    } else {
        count = T_TOTAL;
        list = BHDR_FIXED;
    }
    if (id >= count || !in_bounds(list + id * TABLE_INFO_SIZE, TABLE_INFO_SIZE, len))
        return -1;
    ti->offset = ld16(bin + list + id * TABLE_INFO_SIZE);
    ti->checksum = ld16(bin + list + id * TABLE_INFO_SIZE + 2);
    return 0;
}

/* One signed table, checked as amdgpu_discovery_table_check does: a zero
 * offset means "absent" and passes. size_off/size_w locate the size field
 * inside the table header (0 width: fixed `fixed_size`). */
static void check_table(const uint8_t *bin, uint32_t len, uint16_t major, unsigned id,
                        uint32_t want_sig, uint32_t size_off, uint32_t size_w,
                        uint32_t fixed_size, int check_sum,
                        uint32_t f_sig, uint32_t f_sum, uint32_t *flags)
{
    struct tinfo ti;
    uint32_t size;
    if (table_info(bin, len, major, id, &ti) || ti.offset == 0)
        return;
    if (!in_bounds(ti.offset, size_off + (size_w ? size_w : 4), len)) {
        *flags |= RB_DISC_F_TABLE_BOUNDS;
        return;
    }
    if (ld32(bin + ti.offset) != want_sig) {
        *flags |= f_sig;
        return;
    }
    if (!check_sum)
        return;
    /* Linux reads the GC size as le16 of a u32 field, the IP table size as
     * le16, VCN as le32; mirror that. */
    if (size_w == 2)
        size = ld16(bin + ti.offset + size_off);
    else if (size_w == 4)
        size = ld32(bin + ti.offset + size_off);
    else
        size = fixed_size;
    if (!in_bounds(ti.offset, size, len)) {
        *flags |= RB_DISC_F_TABLE_BOUNDS;
        return;
    }
    if (byte_sum(bin + ti.offset, size) != ti.checksum)
        *flags |= f_sum;
}

/* ip_discovery_header: signature u32 @0, version u16 @4, size u16 @6,
 * id u32 @8, num_dies u16 @12, die_info[16]{die_id u16, die_offset u16} @14,
 * then a u16 whose bit 0 is base_addr_64_bit (version 4). 80 bytes. */
#define IHDR_SIZE      80u
#define IHDR_DIE_INFO  14u
#define IHDR_FLAGS     78u
#define IP_ENTRY_FIXED 8u   /* hw_id u16, inst u8, nbase u8, major, minor, rev, (sub_rev|variant<<4) */

static int parse_ips(const uint8_t *bin, uint32_t len, uint32_t ihdr_off, struct rb_disc *d)
{
    uint32_t i, j, k;
    d->ip_table_version = ld16(bin + ihdr_off + 4);
    d->num_dies = ld16(bin + ihdr_off + 12);
    d->base_addr_64_bit = (uint8_t)(bin[ihdr_off + IHDR_FLAGS] & 1);
    if (d->ip_table_version < 4)
        d->base_addr_64_bit = 0;   /* the byte is padding before v4 */
    if (d->num_dies == 0 || d->num_dies > 16)
        return RB_DISC_E_DIE;

    for (i = 0; i < d->num_dies; i++) {
        uint32_t die_off = ld16(bin + ihdr_off + IHDR_DIE_INFO + i * 4 + 2);
        uint32_t ip_off, num_ips;
        if (!in_bounds(die_off, 4, len) || ld16(bin + die_off) != i)
            return RB_DISC_E_DIE;
        num_ips = ld16(bin + die_off + 2);
        ip_off = die_off + 4;
        for (j = 0; j < num_ips; j++) {
            uint32_t nbase, stride, entry_size;
            uint16_t hw_id;
            uint8_t inst;
            if (!in_bounds(ip_off, IP_ENTRY_FIXED, len))
                return RB_DISC_E_IP_BOUNDS;
            hw_id = ld16(bin + ip_off);
            inst = bin[ip_off + 2];
            nbase = bin[ip_off + 3];
            stride = d->base_addr_64_bit ? 8u : 4u;
            entry_size = IP_ENTRY_FIXED + nbase * stride;
            if (!in_bounds(ip_off, entry_size, len))
                return RB_DISC_E_IP_BOUNDS;

            /* amdgpu_discovery_validate_ip: HWIP_MAX_INSTANCE 48, HW_ID_MAX
             * 300 (amdgpu_ip.h). Linux skips these entries; so does this
             * parser, flagging it. */
            if (inst >= 48 || hw_id >= 300) {
                d->check_flags |= RB_DISC_F_IP_INVALID;
            } else if (d->nips >= RB_DISC_MAX_IPS) {
                d->check_flags |= RB_DISC_F_IPS_TRUNCATED;
            } else {
                struct rb_ip *ip = &d->ips[d->nips++];
                uint8_t b7 = bin[ip_off + 7];
                ip->hw_id = hw_id;
                ip->die = (uint16_t)i;
                ip->inst = inst;
                ip->nbase = (uint8_t)nbase;
                ip->major = bin[ip_off + 4];
                ip->minor = bin[ip_off + 5];
                ip->revision = bin[ip_off + 6];
                if (d->ip_table_version >= 3) {
                    ip->sub_revision = b7 & 0xf;
                    ip->variant = b7 >> 4;
                } else {
                    ip->harvest = b7 & 0xf;
                }
                if (nbase > RB_DISC_MAX_BASES)
                    d->check_flags |= RB_DISC_F_BASES_TRUNCATED;
                for (k = 0; k < nbase && k < RB_DISC_MAX_BASES; k++) {
                    const uint8_t *p = bin + ip_off + IP_ENTRY_FIXED + k * stride;
                    /* 64-bit: Linux keeps the low 32 bits, top two cleared. */
                    ip->base[k] = d->base_addr_64_bit ? (ld32(p) & 0x3FFFFFFFu) : ld32(p);
                }
            }
            ip_off += entry_size;
        }
    }
    return RB_DISC_OK;
}

static void parse_gc(const uint8_t *bin, uint32_t len, uint16_t major, struct rb_disc *d)
{
    struct tinfo ti;
    uint32_t o;
    if (table_info(bin, len, major, T_GC, &ti) || ti.offset == 0)
        return;
    o = ti.offset;
    /* gpu_info_header: table_id u32, major u16, minor u16, size u32 (12 B),
     * then the u32 fields shared by v1.0 - v1.3. */
    if (!in_bounds(o, 12, len) || ld32(bin + o) != RB_DISC_GC_TABLE_ID)
        return;
    d->gc.version_major = ld16(bin + o + 4);
    d->gc.version_minor = ld16(bin + o + 6);
    if (d->gc.version_major != 1) {
        d->gc.present = 2;
        return;
    }
    if (!in_bounds(o, 12 + RB_GC_V1_FIELDS * 4, len))
        return;
    d->gc.present = 1;
    for (uint32_t i = 0; i < RB_GC_V1_FIELDS; i++)
        d->gc.v1[i] = ld32(bin + o + 12 + i * 4);
}

static void parse_harvest(const uint8_t *bin, uint32_t len, uint16_t major, struct rb_disc *d)
{
    struct tinfo ti;
    if (table_info(bin, len, major, T_HARVEST, &ti) || ti.offset == 0)
        return;
    /* harvest_table: header{signature u32, version u32}, list[32]{hw_id u16, inst u8, pad u8}. */
    if (!in_bounds(ti.offset, 8 + RB_DISC_MAX_HARVEST * 4, len) ||
        ld32(bin + ti.offset) != RB_DISC_HARVEST_SIGNATURE)
        return;
    for (uint32_t i = 0; i < RB_DISC_MAX_HARVEST; i++) {
        const uint8_t *e = bin + ti.offset + 8 + i * 4;
        uint16_t hw_id = ld16(e);
        if (hw_id == 0)
            break;   /* Linux stops at the first empty slot */
        d->harvest[d->nharvest].hw_id = hw_id;
        d->harvest[d->nharvest].inst = e[2];
        d->nharvest++;
    }
}

int rb_disc_parse(const uint8_t *bin, uint32_t len, struct rb_disc *d)
{
    struct tinfo ti;
    uint16_t major;
    int r;

    if (!bin || !d)
        return RB_DISC_E_ARG;
    zero(d, sizeof(*d));
    if (len < BHDR_FIXED)
        return RB_DISC_E_SHORT;
    if (ld32(bin) != RB_DISC_BINARY_SIGNATURE)
        return RB_DISC_E_SIGNATURE;

    major = ld16(bin + 4);
    d->bin_version_major = major;
    d->bin_version_minor = ld16(bin + 6);
    d->bin_size = ld16(bin + 10);
    if (major > 2)
        return RB_DISC_E_VERSION;
    if (d->bin_size < BHDR_FIXED || d->bin_size > len)
        return RB_DISC_E_SIZE;
    len = d->bin_size;   /* nothing past binary_size belongs to the table */

    /* Byte sum of everything after the checksum field, up to binary_size. */
    if (byte_sum(bin + 10, len - 10) != ld16(bin + 8))
        d->check_flags |= RB_DISC_F_BINARY_CHECKSUM;

    if (table_info(bin, len, major, T_IP, &ti) || ti.offset == 0 ||
        !in_bounds(ti.offset, IHDR_SIZE, len))
        return RB_DISC_E_NO_IP_TABLE;
    if (ld32(bin + ti.offset) != RB_DISC_TABLE_SIGNATURE)
        return RB_DISC_E_IP_SIGNATURE;

    check_table(bin, len, major, T_IP, RB_DISC_TABLE_SIGNATURE, 6, 2, 0, 1,
                RB_DISC_F_TABLE_BOUNDS, RB_DISC_F_IP_CHECKSUM, &d->check_flags);
    check_table(bin, len, major, T_GC, RB_DISC_GC_TABLE_ID, 8, 2, 0, 1,
                RB_DISC_F_GC_SIGNATURE, RB_DISC_F_GC_CHECKSUM, &d->check_flags);
    check_table(bin, len, major, T_HARVEST, RB_DISC_HARVEST_SIGNATURE, 0, 0,
                8 + RB_DISC_MAX_HARVEST * 4, 1,
                RB_DISC_F_HARVEST_SIG, RB_DISC_F_HARVEST_CHECKSUM, &d->check_flags);
    check_table(bin, len, major, T_VCN, RB_DISC_VCN_TABLE_ID, 8, 4, 0, 1,
                RB_DISC_F_VCN_SIGNATURE, RB_DISC_F_VCN_CHECKSUM, &d->check_flags);
    check_table(bin, len, major, T_MALL, RB_DISC_MALL_TABLE_ID, 8, 4, 0, 0,
                RB_DISC_F_MALL_SIGNATURE, 0, &d->check_flags);

    r = parse_ips(bin, len, ti.offset, d);
    if (r != RB_DISC_OK)
        return r;
    parse_gc(bin, len, major, d);
    parse_harvest(bin, len, major, d);
    return RB_DISC_OK;
}

const struct rb_ip *rb_disc_find(const struct rb_disc *d, uint16_t hw_id, uint8_t inst)
{
    for (uint32_t i = 0; d && i < d->nips; i++)
        if (d->ips[i].hw_id == hw_id && d->ips[i].inst == inst)
            return &d->ips[i];
    return 0;
}

int rb_disc_reg(const struct rb_disc *d, uint16_t hw_id, uint8_t inst,
                uint8_t base_idx, uint32_t reg, uint32_t *out_dw)
{
    const struct rb_ip *ip = rb_disc_find(d, hw_id, inst);
    if (!ip || base_idx >= ip->nbase || base_idx >= RB_DISC_MAX_BASES || !out_dw)
        return -1;
    *out_dw = ip->base[base_idx] + reg;
    return 0;
}

const char *rb_disc_strerror(int err)
{
    switch (err) {
    case RB_DISC_OK:             return "ok";
    case RB_DISC_E_ARG:          return "bad argument";
    case RB_DISC_E_SHORT:        return "buffer shorter than the binary header";
    case RB_DISC_E_SIGNATURE:    return "bad binary signature";
    case RB_DISC_E_VERSION:      return "unknown binary header version";
    case RB_DISC_E_SIZE:         return "binary_size out of range";
    case RB_DISC_E_NO_IP_TABLE:  return "no IP discovery table";
    case RB_DISC_E_IP_SIGNATURE: return "bad IP discovery table signature";
    case RB_DISC_E_DIE:          return "bad die list";
    case RB_DISC_E_IP_BOUNDS:    return "IP entry out of bounds";
    default:                     return "unknown error";
    }
}

/* soc15_hw_ip.h; the IDs a Rembrandt-class APU or this project cares about. */
const char *rb_hwid_name(uint16_t id)
{
    switch (id) {
    case 1:   return "MP1";      case 2:   return "MP2";
    case 3:   return "THM";      case 4:   return "SMUIO";
    case 5:   return "FUSE";     case 6:   return "CLKA";
    case 10:  return "PWR";      case 11:  return "GC";
    case 12:  return "UVD/VCN";  case 13:  return "AUDIO_AZ";
    case 14:  return "ACP";      case 15:  return "DCI";
    case 16:  return "DCO";      case 17:  return "XDMA";
    case 18:  return "DCEAZ";    case 19:  return "SDPMUX";
    case 20:  return "NTB";      case 21:  return "VPE";
    case 22:  return "UMSCH";    case 24:  return "IOHC";
    case 28:  return "L2IMU";    case 32:  return "VCE";
    case 34:  return "MMHUB";    case 35:  return "ATHUB";
    case 36:  return "DBGU_NBIO"; case 37: return "DFX";
    case 38:  return "DBGU0";    case 39:  return "DBGU1";
    case 40:  return "OSSSYS";   case 41:  return "HDP";
    case 42:  return "SDMA0";    case 43:  return "SDMA1";
    case 44:  return "ISP";      case 45:  return "DBGU_IO";
    case 46:  return "DF";       case 47:  return "CLKB";
    case 48:  return "FCH";      case 49:  return "DFX_DAP";
    case 66:  return "WAFLC";    case 67:  return "FCH_USB_PD";
    case 68:  return "SDMA2";    case 69:  return "SDMA3";
    case 70:  return "PCIE";     case 80:  return "PCS";
    case 89:  return "DDCL";     case 90:  return "SST";
    case 91:  return "LSDMA";    case 100: return "IOAGR";
    case 108: return "NBIF";     case 124: return "IOAPIC";
    case 128: return "SYSTEMHUB"; case 144: return "NTBCCP";
    case 150: return "UMC";      case 168: return "SATA";
    case 170: return "USB";      case 176: return "CCXSEC";
    case 200: return "XGMI";     case 216: return "XGBE";
    case 255: return "MP0";      case 271: return "DMU";
    case 272: return "DIO";      case 274: return "DAZ";
    case 294: return "ATU";      case 295: return "AIGC";
    default:  return "?";
    }
}
