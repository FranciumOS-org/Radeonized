/* SPDX-License-Identifier: MIT */
#ifndef RB_AGDC_H
#define RB_AGDC_H
#include <stddef.h>
#include <stdint.h>

/* 2ra: reply layouts measured in Apple's AMD framebuffer, not inferred from
 * the consumer. getVendorInfo 0xbffb59a; getGpuCapability 0xbffb712.
 * Capability +0x34/+0x3c are KERNEL OBJECT POINTERS. Apple's filtered API
 * converts them to registry IDs before returning to userspace (0x13dfd167).
 * Never send this kernel form directly to a user client. */
#define RB_AGDC_VENDOR_SIZE 0x2cu
#define RB_AGDC_CAP_SIZE    0xdcu
static inline void rb_agdc_put32(uint8_t *p, uint32_t v) {
    for (unsigned i=0; i<4; i++) p[i]=(uint8_t)(v >> (8*i));
}
static inline void rb_agdc_put64(uint8_t *p, uint64_t v) {
    for (unsigned i=0; i<8; i++) p[i]=(uint8_t)(v >> (8*i));
}
static inline int rb_agdc_vendor(void *out, size_t size) {
    if (!out || size != RB_AGDC_VENDOR_SIZE) return -1;
    uint8_t *p=(uint8_t *)out;
    for (size_t i=0; i<size; i++) p[i]=0;
    rb_agdc_put32(p, 0x30000);
    p[4]='A'; p[5]='M'; p[6]='D';
    rb_agdc_put32(p+0x24, 0x1002);
    rb_agdc_put32(p+0x28, 2); /* per-GPU service; AMD wrangler uses 7 */
    return 0;
}
static inline int rb_agdc_capability(void *out, size_t size,
                                     uint64_t pci, uint64_t framebuffer) {
    if (!out || size != RB_AGDC_CAP_SIZE || !pci || !framebuffer) return -1;
    uint8_t *p=(uint8_t *)out;
    for (size_t i=0; i<size; i++) p[i]=0;
    /* One existing framebuffer. All optional feature masks remain zero;
     * this bridge does not implement links, DSC, VRR, or display modes. */
    rb_agdc_put32(p+0x20, 1);
    rb_agdc_put32(p+0x30, 1);
    rb_agdc_put64(p+0x34, pci);
    rb_agdc_put64(p+0x3c, framebuffer);
    return 0;
}

/* Validate only the operations we implement. Later AGDC operations legitimately
 * have input data (e.g. 0x921 uses a 0xb0-byte input/output structure).
 * Classifying those as malformed hides the missing operation. */
#define RB_AGDC_BAD_ARGUMENT (-1)
#define RB_AGDC_UNSUPPORTED  (-2)
static inline int rb_agdc_reply(unsigned attr, size_t in_size, void *out,
                                size_t *out_size, uint64_t pci, uint64_t fb) {
    if (attr != 1 && attr != 0x980) return RB_AGDC_UNSUPPORTED;
    if (in_size || !out_size) return RB_AGDC_BAD_ARGUMENT;
    return attr == 1 ? rb_agdc_vendor(out,*out_size)
                     : rb_agdc_capability(out,*out_size,pci,fb);
}
#endif
