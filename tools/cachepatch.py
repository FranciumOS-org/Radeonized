#!/usr/bin/env python3
"""Patch amdMtl_HWL_DetermineSwizzleMode inside the dyld shared cache (2nw).

Why this exists. The swizzle policy that fixes app text (2nr) has to run
inside every process, and seven routes to a universal fix were measured
closed: the kernel's own chooser (2ln), AddrLib's gbAddrConfig (2nt),
AMD_DeviceSettings, the kernel/userspace mode tables, pipeBankXor, an
LSEnvironment in the app bundle (2ns) and a dyld root at the driver's install
path (2nv, dlopen of an extracted copy exits 139). What remains is that the
cache is a FILE, and this edits it.

Two facts make it tractable rather than reckless:

  * the cache's signature is **ad-hoc** (flags 0x2) with an EMPTY
    CMS/BlobWrapper, so nothing is cryptographically pinned - the SHA-256
    page hashes can be recomputed and the signature stays self-consistent;
  * Authenticated Root is disabled and /System/Volumes/Preboot is mounted
    read-write, so no remount of / and no new snapshot are needed.

What it installs. A 5-byte detour at the function entry into a code cave in
the padding that follows the driver's own __TEXT, holding a wrapper that
calls the original through a trampoline and then applies RBGPU_SWIZZLE=fix:
LINEAR for non-render-targets, and render targets remapped off the `_X`
family. `usage` is read as the OR of descriptor +0x68 (as requested) and
+0xb0 (as resolved) - JBR leaves usage unset, so +0x68 alone calls a render
target "not a render target" (2nr).

  python3 tools/cachepatch.py scan
  python3 tools/cachepatch.py build  <out.03>     # patch a COPY, verify it
  python3 tools/cachepatch.py verify <file.03>    # full re-hash of every page
  sudo tools/cachepatch.sh install                # backup + install

Nothing here writes to the live cache; `install` is a separate root-only
script so that the thing which modifies a system volume is the thing the user
runs deliberately.
"""
import hashlib
import os
import shutil
import struct
import sys

CACHE_DIR = "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld"
SUB = os.path.join(CACHE_DIR, "dyld_shared_cache_x86_64h.03")

# Measured, and each re-checked by scan() against the file before anything is
# written. A constant that is not verified at run time is a guess (rule 17).
FN_VM     = 0x7FFB08D35A11          # amdMtl_HWL_DetermineSwizzleMode, unslid
MAP_VM    = 0x7FFB00000000          # .03's executable mapping, file offset 0
DRV_TEXT_END_VM = 0x7FFB08BF3000 + 0x5029AE
CAVE_VM   = 0x7FFB090F5A00          # inside the padding after that, 16-aligned
PROLOGUE  = bytes([
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC,
    0x88, 0x00, 0x00, 0x00, 0x45, 0x89, 0xCD, 0x44,
    0x89, 0xC3, 0x48, 0x89, 0x4D, 0xD0])          # exactly 30 bytes = whole insns
NOX = {21: 5, 22: 6, 23: 6, 25: 9, 26: 10, 27: 10}   # Z modes 20/24 untouched


def off(vm):
    return vm - MAP_VM


def build_cave():
    """Emit the wrapper, the mode table and the trampoline. Returns bytes.

    Stack discipline, checked by hand and stated so it can be checked again:
    at entry rsp = 8 (mod 16); push rbp -> 0; push rbx -> 8; sub 0x18 -> 0,
    which is what the ABI wants at the call. The three stack arguments are
    copied so the original prologue finds them at 0x10/0x18/0x20(%rbp).
    """
    w = bytearray()
    w += bytes([0x55])                               # push rbp
    w += bytes([0x48, 0x89, 0xE5])                   # mov rbp, rsp
    w += bytes([0x53])                               # push rbx
    w += bytes([0x48, 0x83, 0xEC, 0x18])             # sub rsp, 0x18
    w += bytes([0x48, 0x89, 0xD3])                   # mov rbx, rdx   (desc)
    w += bytes([0x48, 0x8B, 0x45, 0x10])             # mov rax,[rbp+0x10]
    w += bytes([0x48, 0x89, 0x04, 0x24])             # mov [rsp],rax
    w += bytes([0x48, 0x8B, 0x45, 0x18])             # mov rax,[rbp+0x18]
    w += bytes([0x48, 0x89, 0x44, 0x24, 0x08])       # mov [rsp+8],rax
    w += bytes([0x48, 0x8B, 0x45, 0x20])             # mov rax,[rbp+0x20]
    w += bytes([0x48, 0x89, 0x44, 0x24, 0x10])       # mov [rsp+0x10],rax
    call_at = len(w)
    w += bytes([0xE8, 0, 0, 0, 0])                   # call TRAMP
    w += bytes([0x48, 0x83, 0xC4, 0x18])             # add rsp, 0x18
    w += bytes([0x48, 0x8B, 0x8B, 0x68, 0, 0, 0])    # mov rcx,[rbx+0x68]
    w += bytes([0x48, 0x0B, 0x8B, 0xB0, 0, 0, 0])    # or  rcx,[rbx+0xb0]
    w += bytes([0xF6, 0xC1, 0x04])                   # test cl, 4   (RenderTarget)
    jnz_at = len(w)
    w += bytes([0x75, 0])                            # jnz RT
    w += bytes([0x31, 0xC0])                         # xor eax,eax  -> LINEAR
    jmp_at = len(w)
    w += bytes([0xEB, 0])                            # jmp OUT
    rt = len(w)
    # 312: a MULTISAMPLED render target keeps Apple's own mode. The remap
    # below turns 27 (64KB_R_X) into 10 (64KB_D) and AddrLib refuses 10 for
    # numFrags > 1 with ADDR_INVALIDPARAMS, so the sizing call never wrote its
    # outputs, newTextureWithDescriptor returned nil, and every WebGL page
    # (antialias:true) came up white. MTLTextureDescriptorPrivate +0x30 is the
    # sample count (the sizing function tests it `>= 2` at 0x...243b3).
    w += bytes([0x48, 0x83, 0x7B, 0x30, 0x02])       # cmp qword [rbx+0x30], 2
    jae_at = len(w)
    w += bytes([0x73, 0])                            # jae OUT  (MSAA: keep)
    w += bytes([0x83, 0xF8, 0x1B])                   # cmp eax, 27
    ja_at = len(w)
    w += bytes([0x77, 0])                            # ja OUT
    lea_at = len(w)
    w += bytes([0x48, 0x8D, 0x0D, 0, 0, 0, 0])       # lea rcx,[rip+TABLE]
    w += bytes([0x0F, 0xB6, 0x04, 0x01])             # movzx eax, byte [rcx+rax]
    out = len(w)
    w += bytes([0x5B, 0x5D, 0xC3])                   # pop rbx; pop rbp; ret

    while len(w) % 16:                               # pad to 16 for the table
        w += b"\x90"
    table_at = len(w)
    w += bytes([NOX.get(i, i) for i in range(28)])
    while len(w) % 16:
        w += b"\x90"
    tramp_at = len(w)
    w += PROLOGUE
    w += bytes([0xE9, 0, 0, 0, 0])                   # jmp FN+30

    # ---- fix every displacement now that the layout is known ----
    w[jnz_at + 1] = rt - (jnz_at + 2)
    w[jmp_at + 1] = out - (jmp_at + 2)
    w[ja_at + 1] = out - (ja_at + 2)
    w[jae_at + 1] = out - (jae_at + 2)
    struct.pack_into("<i", w, lea_at + 3, table_at - (lea_at + 7))
    struct.pack_into("<i", w, call_at + 1, tramp_at - (call_at + 5))
    tramp_end = tramp_at + len(PROLOGUE) + 5
    struct.pack_into("<i", w, tramp_at + len(PROLOGUE) + 1,
                     (FN_VM + len(PROLOGUE)) - (CAVE_VM + tramp_end))
    for v in (w[jnz_at + 1], w[jmp_at + 1], w[ja_at + 1], w[jae_at + 1]):
        assert 0 <= v < 128, "a short jump went out of range"
    return bytes(w)


def detour():
    d = struct.pack("<i", CAVE_VM - (FN_VM + 5))
    return bytes([0xE9]) + d


def cd_info(fh):
    fh.seek(0)
    hdr = fh.read(0x200)
    cso, css = struct.unpack_from("<QQ", hdr, 0x28)
    fh.seek(cso)
    sb = fh.read(0x100)
    magic, length, count = struct.unpack_from(">III", sb, 0)
    assert magic == 0xFADE0CC0, f"not a superblob: {magic:#x}"
    for i in range(count):
        t, o = struct.unpack_from(">II", sb, 12 + 8 * i)
        fh.seek(cso + o)
        bm, bl = struct.unpack_from(">II", fh.read(8), 0)
        if bm == 0xFADE0C02:
            fh.seek(cso + o)
            cd = fh.read(48)
            (ver, flags, hashOff, identOff, nSpecial, nCode, codeLimit) = \
                struct.unpack_from(">IIIIIII", cd, 8)
            hashSize, hashType, _plat, pageLog = struct.unpack_from(">BBBB", cd, 36)
            return dict(cd_off=cso + o, hashOff=hashOff, nSpecial=nSpecial,
                        nCode=nCode, codeLimit=codeLimit, hashSize=hashSize,
                        hashType=hashType, page=1 << pageLog, flags=flags,
                        sig_off=cso, sig_size=css)
    raise SystemExit("no CodeDirectory in the cache signature")


def rehash_pages(path, pages, info):
    with open(path, "r+b") as fh:
        for pg in sorted(pages):
            fh.seek(pg * info["page"])
            data = fh.read(min(info["page"], info["codeLimit"] - pg * info["page"]))
            h = hashlib.sha256(data).digest()
            fh.seek(info["cd_off"] + info["hashOff"] + pg * info["hashSize"])
            fh.write(h)


def cmd_scan():
    with open(SUB, "rb") as fh:
        info = cd_info(fh)
        fh.seek(off(FN_VM))
        got = fh.read(len(PROLOGUE))
        fh.seek(off(CAVE_VM))
        cave_now = fh.read(512)
    cave = build_cave()
    print(f"subcache          {SUB}")
    print(f"function          vm {FN_VM:#x}  file {off(FN_VM):#x}")
    print(f"  prologue match  {got == PROLOGUE}")
    print(f"cave              vm {CAVE_VM:#x}  file {off(CAVE_VM):#x}")
    print(f"  driver __TEXT ends at vm {DRV_TEXT_END_VM:#x} (cave is after it: "
          f"{CAVE_VM >= DRV_TEXT_END_VM})")
    print(f"  all zero now    {set(cave_now[:len(cave) + 32]) == {0}}")
    print(f"  need {len(cave)} bytes, {len(cave_now)}+ available")
    print(f"signature         adhoc={bool(info['flags'] & 2)} "
          f"hashType={info['hashType']} page={info['page']} "
          f"slots={info['nCode']} codeLimit={info['codeLimit']:#x}")
    pages = sorted({off(FN_VM) // info["page"]} |
                   {(off(CAVE_VM) + i) // info["page"] for i in (0, len(cave) - 1)})
    print(f"pages to re-hash  {pages}")
    ok = got == PROLOGUE and set(cave_now[:len(cave) + 32]) == {0} \
        and bool(info["flags"] & 2)
    print(f"\nSAFE TO BUILD: {ok}")
    return 0 if ok else 1


def cmd_build(out):
    cave = build_cave()
    print(f"copying {SUB}\n     -> {out}  ({os.path.getsize(SUB)/2**20:.1f} MB)")
    shutil.copyfile(SUB, out)
    with open(out, "rb") as fh:
        info = cd_info(fh)
        fh.seek(off(FN_VM))
        assert fh.read(len(PROLOGUE)) == PROLOGUE, "prologue moved - refusing"
        fh.seek(off(CAVE_VM))
        assert set(fh.read(len(cave))) == {0}, "cave is not empty - refusing"
    with open(out, "r+b") as fh:
        fh.seek(off(CAVE_VM)); fh.write(cave)
        fh.seek(off(FN_VM));   fh.write(detour())
    pages = {off(FN_VM) // info["page"]}
    pages |= {(off(CAVE_VM) + i) // info["page"] for i in range(0, len(cave) + 1)}
    rehash_pages(out, pages, info)
    print(f"patched: {len(cave)}-byte cave at {off(CAVE_VM):#x}, "
          f"5-byte detour at {off(FN_VM):#x}, re-hashed pages {sorted(pages)}")
    return cmd_verify(out, full=True)


def cmd_verify(path, full=False):
    cave = build_cave()
    bad = 0
    with open(path, "rb") as fh:
        info = cd_info(fh)
        fh.seek(off(FN_VM))
        d = fh.read(5)
        print(f"detour bytes      {d.hex()}  expected {detour().hex()}  "
              f"{'OK' if d == detour() else 'MISMATCH'}")
        bad += d != detour()
        fh.seek(off(CAVE_VM))
        c = fh.read(len(cave))
        print(f"cave bytes        {'OK' if c == cave else 'MISMATCH'} ({len(cave)} bytes)")
        bad += c != cave
        if full:
            fh.seek(info["cd_off"] + info["hashOff"])
            slots = fh.read(info["nCode"] * info["hashSize"])
            mism = []
            fh.seek(0)
            for pg in range(info["nCode"]):
                data = fh.read(min(info["page"],
                                   info["codeLimit"] - pg * info["page"]))
                if hashlib.sha256(data).digest() != \
                        slots[pg * 32:(pg + 1) * 32]:
                    mism.append(pg)
                    if len(mism) > 8:
                        break
            print(f"page hashes       {info['nCode']} slots checked, "
                  f"{len(mism)} mismatched {mism[:8]}")
            bad += len(mism)
    print(f"\nVERIFY: {'PASS' if not bad else 'FAIL'}")
    return 0 if not bad else 1


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a or a[0] == "scan":
        sys.exit(cmd_scan())
    if a[0] == "build":
        sys.exit(cmd_build(a[1]))
    if a[0] == "verify":
        sys.exit(cmd_verify(a[1], full=True))
    sys.exit(__doc__)
