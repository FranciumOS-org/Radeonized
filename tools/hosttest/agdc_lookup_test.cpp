#include "rbgpu_agdc_lookup.hpp"
#include <stdio.h>

struct Patcher {
    static constexpr size_t KernelID=0;
    uintptr_t address=0xffffff8000a49940ULL;
    unsigned plain=0,ranged=0;
    uintptr_t solveSymbol(size_t,const char *) { ++plain; return address; }
    uintptr_t solveSymbol(size_t id,const char *name,uintptr_t base,size_t size) {
        ++ranged;
        auto a=solveSymbol(id,name);
        return a>=base && a-base<size ? a : 0;
    }
};

int main() {
    unsigned failed=0;
#define A(x) do { if (!(x)) { printf("FAIL lookup:%d: %s\n",__LINE__,#x); ++failed; } } while (0)
    Patcher p;
    uintptr_t raw=0;
    // The exact boot-155 regression: a valid kernel VA, no supplied range.
    A(rb_agdc_lookup(p,Patcher::KernelID,"ctor",0,0,&raw)==p.address);
    A(raw==p.address && p.plain==1 && p.ranged==0);
    const uintptr_t base=p.address-0x40;
    A(rb_agdc_lookup(p,7,"kext",base,0x80,&raw)==p.address);
    A(rb_agdc_lookup(p,7,"below",p.address+1,0x80,&raw)==0);
    A(raw==p.address); // a refusal must retain the address it rejected
    A(rb_agdc_lookup(p,7,"end",base,0x40,&raw)==0);
    A(rb_agdc_lookup(p,7,"empty",base,0,&raw)==0);
    A(rb_agdc_lookup(p,7,"overflow-safe",base,(size_t)-1,&raw)==p.address);
    p.address=0;
    A(rb_agdc_lookup(p,Patcher::KernelID,"missing",0,0,&raw)==0 && raw==0);
    A(p.ranged==0);
    printf("agdc lookup: kernel symbols, kext bounds, rejected addresses: %s\n",failed?"FAILED":"passed");
    return failed ? 1 : 0;
}
