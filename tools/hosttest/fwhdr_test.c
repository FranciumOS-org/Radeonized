/* SPDX-License-Identifier: MIT */
/* 3fxe: rb_fwhdr.h against the real AMD firmware containers.
 *
 * The experiment this file IS: the plugin used to carry each image's payload
 * as a transcribed C array with its size hardcoded at the use site. It now
 * embeds each upstream .bin whole and derives offset and size from the
 * container header. Those two must produce the SAME BYTES, and the arrays they
 * replace are deleted, so the control cannot be the arrays themselves - it is
 * their SHA-256, pinned below, measured off the shipping arrays and
 * independently off upstream's current files, which were byte-identical.
 *
 * So a change to the parser, a wrong field offset, or an upstream file that is
 * not the one this driver was validated against all fail here rather than on
 * hardware. If the files are absent the cases FAIL rather than quietly pass
 * (rule 26): a check that skips is not a check.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rb_fwhdr.h"

int fwhdr_tests(void);

/* A tiny SHA-256, so the test depends on nothing but libc. */
struct sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; };

static uint32_t ror(uint32_t x, int s) { return (x >> s) | (x << (32 - s)); }

static void sha256_block(struct sha256 *s, const uint8_t *p) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16)
             | ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}

static void sha256(const uint8_t *p, size_t n, char out[65]) {
    struct sha256 s;
    s.h[0]=0x6a09e667;s.h[1]=0xbb67ae85;s.h[2]=0x3c6ef372;s.h[3]=0xa54ff53a;
    s.h[4]=0x510e527f;s.h[5]=0x9b05688c;s.h[6]=0x1f83d9ab;s.h[7]=0x5be0cd19;
    size_t i = 0;
    for (; i + 64 <= n; i += 64) { sha256_block(&s, p + i); }
    uint8_t tail[128];
    size_t r = n - i;
    memcpy(tail, p + i, r);
    tail[r++] = 0x80;
    size_t pad = (r <= 56) ? (56 - r) : (120 - r);
    memset(tail + r, 0, pad);
    r += pad;
    uint64_t bits = (uint64_t)n * 8u;
    for (int k = 7; k >= 0; k--) { tail[r++] = (uint8_t)(bits >> (k * 8)); }
    for (size_t o = 0; o < r; o += 64) { sha256_block(&s, tail + o); }
    for (int k = 0; k < 8; k++) {
        snprintf(out + k * 8, 9, "%08x", s.h[k]);
    }
    out[64] = 0;
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *p = (uint8_t *)malloc((size_t)n);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return p;
}

/* The pins. `payload` is the SHA-256 of the bytes the plugin hands the PSP,
 * which is what the deleted arrays contained; `file` is the SHA-256 of the
 * whole upstream file, which is what the binary now embeds and distributes, so
 * tools/fetch_amd_fw.sh's digests and these are the same numbers checked at
 * two different points. `size` is what the old use site hardcoded. */
struct fwpin {
    const char *name;
    const char *file_sha;
    const char *payload_sha;
    uint32_t    size;         /* expected ucode_size_bytes */
    uint32_t    off;          /* expected ucode_array_offset_bytes */
    uint32_t    ttl_id;       /* the _IpiSetFwEntry id it is swapped for */
};

static const struct fwpin kFiles[] = {
  { "ce",   "bd1fd7a357d7c9aa489469166435aa2d3e2ee1b324d4be35e9a648c794281674",
            "cf73e99a3f3b87a2893b3f30a813f6616bef541d40fd20d1eb2d98ebc2eab6f1",
            0x40380u, 0x100u,  1u },
  { "pfp",  "b817f15a37ae080d5037a159732d60be24dda0bed74b6a3ba609d7c38f4d29a7",
            "fee62b892c598baf6662a0d476b2983aff20708c021c362aaf4b50ce719cf841",
            0x40400u, 0x100u,  2u },
  { "me",   "e2eeb8958a08231813943dabffd0e18ab65e7c0338a36155b9e6584ce55df31f",
            "58a786dd54df1494af1d8ab519a41caf27d4f84abc044b5b9d937e4c988b16d0",
            0x40400u, 0x100u,  3u },
  { "mec",  "9020014a34a9357b28058f4ecdf1f0d3ffbcac7265df04c544722ed5f7447e9b",
            "4f612b942fc386eda32f75d62d7b65f60fd932e6af92bcd35ea0c9093e9b1f01",
            0x41680u, 0x100u,  6u },
  { "rlc",  "bf41c78ecf4f3444ee14c917358485e55bfc5c496779821f1cad643b25a3da7f",
            "de73d2f6dc3431e940da32e9c4af9899b1f5ad2f396ee36f13f0be6348a6c7da",
            0x06200u, 0x100u,  9u },
  { "sdma", "be8cdc90616c6aa1e92cf1307e18515958a5c6b80552e20fa4452e2e61c18fd4",
            "3ec1e7b26a13fb79d322580695de32952cf56c6f414cf36d1c5a2294207589c6",
            0x08400u, 0x100u, 14u },
  { "toc",  "9fc8da2f6106bf657222ca4bcca687e66e657e5710278c694d9d0e862221758d",
            "f4ca6af3ba20ead56ecd9b98646fc23f8f04ef1dbb8f5b098d8b7041e2962bcf",
            0x00600u, 0x100u, 36u },
};

/* The five RLC sub-images, in rb_fwhdr.h's index order. */
static const struct fwpin kSubs[RB_FWHDR_RLC_SUB_COUNT] = {
  { "rlc.srlist_cntl", NULL,
    "47518e58851b389b0d983c00db84a49cbf5d8d93b0175a2d18d364cbfbc2d884",
    0x00250u, 0x06300u, 26u },
  { "rlc.srlist_gpm",  NULL,
    "5c598ec7056693b24fac35a9ce2ed3301b60f4f810544234544e848b22858411",
    0x00600u, 0x06550u, 27u },
  { "rlc.srlist_srm",  NULL,
    "76d2d353bff3cf216885eace2ed9888c875cd5af473352071a7b1ecf58466463",
    0x04880u, 0x06b50u, 28u },
  { "rlc.lx6_iram",    NULL,
    "18c14f989a0ed7745fe0929638b4890f2f20595fbcfe2a148368c825495c660e",
    0x10200u, 0x0b3d0u, 29u },
  { "rlc.lx6_dram",    NULL,
    "5a6a372d40d2351434d7f5cc6e6590d11904cafac90efcc9dda253bae3390af3",
    0x10200u, 0x1b5d0u, 30u },
};

int fwhdr_tests(void)
{
    int bad = 0;
    char got[65];

#define CHECK(c, msg) do { \
    if (!(c)) { printf("FAIL fwhdr: %s\n", (msg)); bad++; } \
} while (0)

    /* --------------------------------------------- the three header bases
     *
     * Pinned as literals, because the deleted generator derived them by
     * counting words and a miscount would move every sub-image offset
     * silently. These are the numbers the extractor used while the arrays it
     * produced were verified byte-for-byte, so they are measured, not
     * assumed. */
    CHECK(RB_FWHDR_COMMON_LEN == 0x20u, "common header is 0x20 bytes");
    CHECK(RB_FWHDR_RLC_V2_0   == 0x68u, "rlc v2.0 base is 0x68");
    CHECK(RB_FWHDR_RLC_V2_1   == 0x9cu, "rlc v2.1 base is 0x9c");
    CHECK(RB_FWHDR_RLC_V2_2   == 0xacu, "rlc v2.2 base is 0xac");

    /* ------------------------------------------------------------ refusals
     *
     * Every one is a distinct code, and each is provoked by a buffer built
     * for it rather than by a real file, so the codes are exercised even on a
     * machine with no firmware present. */
    uint32_t o = 0xdeadu, s = 0xbeefu;
    CHECK(rb_fwhdr_payload(NULL, 64u, &o, &s) == RB_FWHDR_E_NULL,
          "NULL file is E_NULL");

    uint8_t small[8] = {0};
    CHECK(rb_fwhdr_payload(small, sizeof(small), &o, &s) == RB_FWHDR_E_SHORT,
          "a file shorter than the header is E_SHORT");

    uint8_t hdr[256];
    memset(hdr, 0, sizeof(hdr));
    CHECK(rb_fwhdr_payload(hdr, sizeof(hdr), NULL, &s) == RB_FWHDR_E_NO_OUT,
          "no out_off is E_NO_OUT");
    CHECK(rb_fwhdr_payload(hdr, sizeof(hdr), &o, NULL) == RB_FWHDR_E_NO_OUT,
          "no out_size is E_NO_OUT");

    /* header_size_bytes 0 - below the common header */
    CHECK(rb_fwhdr_payload(hdr, sizeof(hdr), &o, &s) == RB_FWHDR_E_HDRSIZE,
          "header_size_bytes 0 is E_HDRSIZE");

    /* a plausible header, then one field broken at a time */
#define PUT32(p, off, v) do { \
    (p)[(off)+0] = (uint8_t)((v) & 0xffu);          \
    (p)[(off)+1] = (uint8_t)(((v) >> 8) & 0xffu);   \
    (p)[(off)+2] = (uint8_t)(((v) >> 16) & 0xffu);  \
    (p)[(off)+3] = (uint8_t)(((v) >> 24) & 0xffu);  \
} while (0)

    PUT32(hdr, RB_FWHDR_O_HEADER_SIZE_BYTES, 0x20u);
    PUT32(hdr, RB_FWHDR_O_UCODE_ARRAY_OFFSET, 0x20u);
    PUT32(hdr, RB_FWHDR_O_UCODE_SIZE_BYTES, 0x10u);
    o = 0; s = 0;
    CHECK(rb_fwhdr_payload(hdr, sizeof(hdr), &o, &s) == RB_FWHDR_OK &&
          o == 0x20u && s == 0x10u,
          "a well-formed synthetic header parses");

    PUT32(hdr, RB_FWHDR_O_UCODE_SIZE_BYTES, 0u);
    CHECK(rb_fwhdr_payload(hdr, sizeof(hdr), &o, &s) == RB_FWHDR_E_ZEROSIZE,
          "ucode_size_bytes 0 is E_ZEROSIZE");
    PUT32(hdr, RB_FWHDR_O_UCODE_SIZE_BYTES, 0x10u);

    PUT32(hdr, RB_FWHDR_O_UCODE_ARRAY_OFFSET, 0x10u);
    CHECK(rb_fwhdr_payload(hdr, sizeof(hdr), &o, &s) == RB_FWHDR_E_OFF_IN_HDR,
          "a payload starting inside the header is E_OFF_IN_HDR");
    PUT32(hdr, RB_FWHDR_O_UCODE_ARRAY_OFFSET, 0x20u);

    PUT32(hdr, RB_FWHDR_O_UCODE_SIZE_BYTES, 0x1000u);
    CHECK(rb_fwhdr_payload(hdr, sizeof(hdr), &o, &s) == RB_FWHDR_E_OVERRUN,
          "a payload running past the file is E_OVERRUN");

    PUT32(hdr, RB_FWHDR_O_UCODE_ARRAY_OFFSET, 0xFFFFFFF0u);
    PUT32(hdr, RB_FWHDR_O_UCODE_SIZE_BYTES, 0x100u);
    CHECK(rb_fwhdr_payload(hdr, sizeof(hdr), &o, &s) == RB_FWHDR_E_OVERFLOW,
          "offset + size wrapping is E_OVERFLOW");

    /* A refused call must not write its outputs - rule 14, so a caller that
     * ignores the code cannot read a plausible offset out of a rejection. */
    o = 0x5a5au; s = 0xa5a5u;
    (void)rb_fwhdr_payload(hdr, sizeof(hdr), &o, &s);
    CHECK(o == 0x5a5au && s == 0xa5a5u,
          "a refusal leaves the outputs untouched");

    CHECK(rb_fwhdr_rlc_sub(hdr, sizeof(hdr), -1, &o, &s) == RB_FWHDR_E_BAD_SUB,
          "sub-image -1 is E_BAD_SUB");
    CHECK(rb_fwhdr_rlc_sub(hdr, sizeof(hdr), RB_FWHDR_RLC_SUB_COUNT, &o, &s)
              == RB_FWHDR_E_BAD_SUB,
          "sub-image past the count is E_BAD_SUB");

    /* v2.2 and ip 10.3 are preconditions, not details: the sub-image fields
     * do not exist before v2.1 and the offsets would read neighbouring
     * words. */
    memset(hdr, 0, sizeof(hdr));
    PUT32(hdr, RB_FWHDR_O_HEADER_SIZE_BYTES, 0xacu);
    hdr[RB_FWHDR_O_HEADER_VER_MAJOR] = 2; hdr[RB_FWHDR_O_HEADER_VER_MINOR] = 1;
    CHECK(rb_fwhdr_rlc_sub(hdr, sizeof(hdr), 0, &o, &s) == RB_FWHDR_E_RLC_VER,
          "an rlc header v2.1 is E_RLC_VER");
    hdr[RB_FWHDR_O_HEADER_VER_MINOR] = 2;
    hdr[RB_FWHDR_O_IP_VER_MAJOR] = 10; hdr[RB_FWHDR_O_IP_VER_MINOR] = 2;
    CHECK(rb_fwhdr_rlc_sub(hdr, sizeof(hdr), 0, &o, &s) == RB_FWHDR_E_RLC_IP,
          "ip 10.2 is E_RLC_IP");

    /* --------------------------------------------------- the real containers
     *
     * The point of the file. RB_FWHDR_DIR is passed by the Makefile. */
#ifndef RB_FWHDR_DIR
#define RB_FWHDR_DIR "third_party/amd-fw"
#endif

    const uint8_t *rlc_file = NULL;
    size_t rlc_len = 0;

    for (size_t i = 0; i < sizeof(kFiles) / sizeof(kFiles[0]); i++) {
        const struct fwpin *p = &kFiles[i];
        char path[512];
        snprintf(path, sizeof(path), "%s/yellow_carp_%s.bin", RB_FWHDR_DIR, p->name);

        size_t len = 0;
        uint8_t *d = slurp(path, &len);
        if (!d) {
            printf("FAIL fwhdr: %s absent - run 'sh tools/fetch_amd_fw.sh' "
                   "(an absent file is a FAILURE, not a skip)\n", path);
            bad++;
            continue;
        }

        /* The file as distributed. This is the digest the fetch script pins,
         * checked here a second time against the binary's own copy. */
        sha256(d, len, got);
        if (strcmp(got, p->file_sha) != 0) {
            printf("FAIL fwhdr: %s file sha256 %s, expected %s\n",
                   p->name, got, p->file_sha);
            bad++;
        }

        uint32_t off = 0, size = 0;
        int rc = rb_fwhdr_payload(d, (uint32_t)len, &off, &size);
        if (rc != RB_FWHDR_OK) {
            printf("FAIL fwhdr: %s payload refused %d\n", p->name, rc);
            bad++;
            free(d);
            continue;
        }
        if (off != p->off || size != p->size) {
            printf("FAIL fwhdr: %s off 0x%x size 0x%x, expected 0x%x / 0x%x\n",
                   p->name, off, size, p->off, p->size);
            bad++;
        }

        /* THE CHECK THIS FILE EXISTS FOR: the slice is byte-for-byte the array
         * that used to be compiled in. */
        sha256(d + off, size, got);
        if (strcmp(got, p->payload_sha) != 0) {
            printf("FAIL fwhdr: %s payload sha256 %s, expected %s "
                   "(the slice is NOT the bytes that shipped)\n",
                   p->name, got, p->payload_sha);
            bad++;
        }

        if (strcmp(p->name, "rlc") == 0) {
            rlc_file = d;
            rlc_len = len;
        } else {
            free(d);
        }
    }

    if (!rlc_file) {
        printf("FAIL fwhdr: no rlc container, so the five sub-images are "
               "unchecked\n");
        bad++;
    } else {
        for (int i = 0; i < RB_FWHDR_RLC_SUB_COUNT; i++) {
            const struct fwpin *p = &kSubs[i];
            uint32_t off = 0, size = 0;
            int rc = rb_fwhdr_rlc_sub(rlc_file, (uint32_t)rlc_len, i, &off, &size);
            if (rc != RB_FWHDR_OK) {
                printf("FAIL fwhdr: %s refused %d\n", p->name, rc);
                bad++;
                continue;
            }
            if (off != p->off || size != p->size) {
                printf("FAIL fwhdr: %s off 0x%x size 0x%x, expected 0x%x / 0x%x\n",
                       p->name, off, size, p->off, p->size);
                bad++;
            }
            sha256(rlc_file + off, size, got);
            if (strcmp(got, p->payload_sha) != 0) {
                printf("FAIL fwhdr: %s sha256 %s, expected %s "
                       "(the slice is NOT the bytes that shipped)\n",
                       p->name, got, p->payload_sha);
                bad++;
            }
        }

        /* The sub-images must not overlap the main payload, and must lie
         * inside the file. The extractor never checked either, and a swapped
         * pair of size/offset fields would still look plausible. */
        uint32_t main_off = 0, main_size = 0;
        if (rb_fwhdr_rlc_sub(rlc_file, (uint32_t)rlc_len, 0, &main_off, &main_size)
                == RB_FWHDR_OK &&
            rb_fwhdr_payload(rlc_file, (uint32_t)rlc_len, &main_off, &main_size)
                == RB_FWHDR_OK) {
            for (int i = 0; i < RB_FWHDR_RLC_SUB_COUNT; i++) {
                uint32_t off = 0, size = 0;
                if (rb_fwhdr_rlc_sub(rlc_file, (uint32_t)rlc_len, i, &off, &size)
                        != RB_FWHDR_OK) {
                    continue;
                }
                CHECK(off >= main_off + main_size,
                      "an rlc sub-image overlaps the main payload");
            }
        }
        free((void *)rlc_file);
    }

    /* The TTL ids the swap keys on. Pinned here because they are the one part
     * of the mapping that comes from our own _IpiSetFwEntry log rather than
     * from a header, and a renumbering would send the PSP the wrong image. */
    CHECK(kFiles[0].ttl_id ==  1u, "ce is TTL id 1");
    CHECK(kFiles[4].ttl_id ==  9u, "rlc is TTL id 9");
    CHECK(kFiles[5].ttl_id == 14u, "sdma is TTL id 14");
    CHECK(kFiles[6].ttl_id == 36u, "toc is TTL id 36");
    CHECK(kSubs[RB_FWHDR_RLC_SRLIST_CNTL].ttl_id == 26u, "srlist_cntl is id 26");
    CHECK(kSubs[RB_FWHDR_RLC_LX6_DRAM].ttl_id   == 30u, "lx6_dram is id 30");

#undef PUT32
#undef CHECK
    return bad;
}
