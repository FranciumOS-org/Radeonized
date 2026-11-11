/* 3fxe: the AMD firmware containers, embedded WHOLE and byte-exact.
 *
 * Each yellow_carp_*.bin from linux-firmware is included verbatim with
 * .incbin, so every byte this binary carries is a copy of an upstream file -
 * not an excerpt, not a transcription. src/common/rb_fwhdr.h slices the
 * payload at runtime from the container's own header, exactly as Linux does.
 *
 * These are AMD microcode under AMD's own licence; see
 * third_party/LICENSE.amdgpu, reproduced verbatim as that licence requires,
 * and third_party/README.md for the notices. The files are fetched by
 * tools/fetch_amd_fw.sh and are not in this repository.
 *
 * Why assembly rather than C arrays: the previous form was 8 MB of hex text
 * in src/plugin/fw_yellow_carp*.cpp, committed to the repository. .incbin puts
 * the raw file in the binary and keeps it out of the tree.
 *
 * The Makefile passes the firmware directory as an assembler include path
 * (-I), which is where .incbin looks; it does not concatenate string
 * literals, so the path cannot be pasted together here. Each blob is
 * 4096-aligned because the PSP is handed the pointer directly and 2aj's
 * arrays were aligned too.
 */
#if RB_FW_YC

#define RB_BLOB(sym, file)                     \
    .const;                                    \
    .p2align 12;                               \
    .globl _ ## sym ## _file;                  \
_ ## sym ## _file:                             \
    .incbin file;                              \
_ ## sym ## _file_end:;                        \
    .p2align 3;                                \
    .globl _ ## sym ## _file_len;              \
_ ## sym ## _file_len:                         \
    .long _ ## sym ## _file_end - _ ## sym ## _file

RB_BLOB(yc_ce,   "yellow_carp_ce.bin")
RB_BLOB(yc_pfp,  "yellow_carp_pfp.bin")
RB_BLOB(yc_me,   "yellow_carp_me.bin")
RB_BLOB(yc_mec,  "yellow_carp_mec.bin")
RB_BLOB(yc_rlc,  "yellow_carp_rlc.bin")
RB_BLOB(yc_sdma, "yellow_carp_sdma.bin")
RB_BLOB(yc_toc,  "yellow_carp_toc.bin")

#endif /* RB_FW_YC */
