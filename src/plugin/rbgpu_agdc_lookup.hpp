#ifndef RBGPU_AGDC_LOOKUP_HPP
#define RBGPU_AGDC_LOOKUP_HPP
#include <stddef.h>
#include <stdint.h>

// KernelPatcher's FOUR-argument overload validates [base, base+size).
// Passing its default kernel lookup arguments (0,0) rejects every symbol.
// Resolve with the two-argument API, preserve the raw result for diagnostics,
// and apply the kext-only bounds explicitly without overflowing base+size.
template <class Patcher>
static inline auto rb_agdc_lookup(Patcher &p,size_t id,const char *name,
                                  uintptr_t base,size_t size,uintptr_t *raw)
    -> decltype(p.solveSymbol(id,name)) {
    auto address=p.solveSymbol(id,name);
    if (raw) *raw=static_cast<uintptr_t>(address);
    if (id!=Patcher::KernelID &&
        (address<base || !size || address-base>=size))
        return 0;
    return address;
}
#endif
