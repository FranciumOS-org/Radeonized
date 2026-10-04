/* SPDX-License-Identifier: MIT */
/*
 * rbdump: print an AMD IP discovery binary.
 *
 *   build/out/rbdump logs/<run>/discovery.bin
 *
 * Uses src/common/rb_discovery.c, the same parser the kext runs.
 */
#include <stdio.h>
#include <stdint.h>

#include "rb_discovery.h"

static const char *gc_names[RB_GC_V1_FIELDS] = {
    "num_se", "num_wgp0_per_sa", "num_wgp1_per_sa", "num_rb_per_se",
    "num_gl2c", "num_gprs", "num_max_gs_thds", "gs_table_depth",
    "gsprim_buff_depth", "parameter_cache_depth", "double_offchip_lds_buffer",
    "wave_size", "max_waves_per_simd", "max_scratch_slots_per_cu", "lds_size",
    "num_sc_per_se", "num_sa_per_se", "num_packer_per_sc", "num_gl2a",
};

int main(int argc, char **argv)
{
    static uint8_t buf[1 << 16];
    static struct rb_disc d;
    FILE *fp;
    size_t n;
    int e;

    if (argc != 2) {
        fprintf(stderr, "usage: rbdump <discovery.bin>\n");
        return 2;
    }
    fp = fopen(argv[1], "rb");
    if (!fp) {
        perror(argv[1]);
        return 1;
    }
    n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);

    e = rb_disc_parse(buf, (uint32_t)n, &d);
    printf("binary v%u.%u, %u bytes; IP table v%u, %u die(s)%s\n",
           d.bin_version_major, d.bin_version_minor, d.bin_size,
           d.ip_table_version, d.num_dies, d.base_addr_64_bit ? ", 64-bit bases" : "");
    if (e != RB_DISC_OK) {
        printf("parse failed: %s\n", rb_disc_strerror(e));
        return 1;
    }
    if (d.check_flags)
        printf("check flags 0x%x (Linux would reject this table)\n", d.check_flags);

    printf("\n%-10s %4s %4s  %-10s  %s\n", "IP", "id", "inst", "version", "bases (dword)");
    for (uint32_t i = 0; i < d.nips; i++) {
        const struct rb_ip *ip = &d.ips[i];
        char ver[24];
        if (d.ip_table_version >= 3)
            snprintf(ver, sizeof(ver), "%u.%u.%u.%u", ip->major, ip->minor, ip->revision, ip->sub_revision);
        else
            snprintf(ver, sizeof(ver), "%u.%u.%u", ip->major, ip->minor, ip->revision);
        printf("%-10s %4u %4u  %-10s ", rb_hwid_name(ip->hw_id), ip->hw_id, ip->inst, ver);
        for (uint32_t k = 0; k < ip->nbase && k < RB_DISC_MAX_BASES; k++)
            printf(" %#x", ip->base[k]);
        if (ip->harvest)
            printf("  harvest=%#x", ip->harvest);
        printf("\n");
    }

    if (d.gc.present == 1) {
        printf("\nGC info v%u.%u\n", d.gc.version_major, d.gc.version_minor);
        for (int i = 0; i < RB_GC_V1_FIELDS; i++)
            printf("  %-26s %u\n", gc_names[i], d.gc.v1[i]);
    } else if (d.gc.present == 2) {
        printf("\nGC info v%u.%u (not decoded)\n", d.gc.version_major, d.gc.version_minor);
    }

    if (d.nharvest) {
        printf("\nharvested (fused off):\n");
        for (uint32_t i = 0; i < d.nharvest; i++)
            printf("  %s(%u) instance %u\n", rb_hwid_name(d.harvest[i].hw_id),
                   d.harvest[i].hw_id, d.harvest[i].inst);
    }
    return 0;
}
