# AMDGFX10PM4Engine::powerUp / doStart — verified control flow

Read from the installed macOS 15.8.1 AMDRadeonX6000 (7.0.25) and
IOAcceleratorFamily2 (485.2) with tools/re, 2026-10-12. Addresses are file
addresses of the extracted kexts in `build/re`; resolve by name before use.

This spec exists to correct the reading behind milestones 2ch-2cl. Two things
in that reading were wrong, and one of them (2cl) is a regression that has to
be reverted before the next boot means anything.

## 1. powerUp is doStart(false)

```
__ZN32AMDRadeonX6000_AMDGFX10PM4Engine7powerUpEv        0xbea1182  (14 bytes)
    movq (%rdi), %rax
    jmpq *0x148(%rax)                 ; tail call, this' own vtable
```

PM4Engine vtable (`__ZTV32AMDRadeonX6000_AMDGFX10PM4Engine` + 16) slot 0x148
is a thunk at 0xbea3b60:

```
    xorl %esi, %esi
    jmp  doStart                      ; doStart(this, false)
```

So:

- `powerUp()` **is** `doStart(false)`, reached by a tail jump.
- There is **no recovery path in powerUp**. It has no body: doStart's return
  value is powerUp's return value, verbatim, and powerUp cannot inspect or
  retry it. Anything resembling recovery has to live in doStart itself or in
  the caller (`AMDPM4HWEngine`/`IAMDHWInterface::powerUpHWEngines`).
- Note vtable slot 0x138 is `powerUp` itself, and slot 0x150 is a thunk that
  calls `doStop(false)` and returns true. Do not confuse the two: inside
  doStart, `vtable[0x138]` and `vtable[0x150]` calls are made on *other*
  objects (engine+0x70 and this), not on this vtable's layout.

## 2. doStart DOES have a return-true path, and startKIQ returns 0 for success

The claim "doStart always returns 0, there is no return-true path" is wrong.
It reads 0xbea1280 as part of the failure path, but 0xbea1280 is the *common*
epilogue: it returns `r14 & 1`, and two different predecessors reach it —
0xbea127d sets `r14 = 0` first, while 0xbea150e jumps there with `r14 = 1`.

```
 bea126d:  testl %eax, %eax                 ; eax = startKIQ(...)
 bea126f:  je    0xbea1296                  ; eax == 0  -> CONTINUE (long path)
 bea1271:  callq *0x138(%r14)               ; eax != 0  -> abort helper
 bea127d:  xorl  %r14d, %r14d               ; r14 = 0
 bea1280:  andb  $0x1, %r14b                ; <-- shared epilogue
 bea1284:  movl  %r14d, %eax
 ...
 bea1295:  retq
```

`je` is taken when `testl` sets ZF, i.e. when **startKIQ returned 0**. So:

> **startKIQ returns 0 on success.** It is a status code (its signature returns
> a status and takes a `uint *` out-parameter), not a bool.

The 2ci comment in kern_start.cpp already derived this correctly — "2ch showed
it returns 0, meaning vtable[0x120] returned 0 AND spec matched" — and then
2cl forced the return to 1 anyway. `kiqres=0` in the 2ck/2cl panic lines was
**success being reported**, and 2cl converts it into the one branch that is
guaranteed to fail: it calls `engine+0x70 -> vtable[0x138]()` and returns
false. 2cl must be removed; it cannot ever produce a passing boot.

### The return-true path

With the arg false (the powerUp path), doStart runs:

| Step | Address | Must hold |
|---|---|---|
| `initComputeMQD(4)` | 0xbea11ad | true, else return false immediately |
| *(arg==true only: CP_SOFT_RESET_CNTL write + 200 ms sleep)* | 0xbea11bf | skipped for powerUp |
| `(engine+0x70)->vtable[0x130]()` | 0xbea123c | return ignored |
| `KIQHWChannel::startKIQ(engine+0xe0, …)` | 0xbea1268 | **== 0** |
| `this+0x10 = 1`; `submitSetResourcesPacket(engine+0x2b8)` | 0xbea12a4 | true, else exit via vtable[0x150] with r14=0 |
| ring loop `r13 = 0 .. this+0x348` (0x348 = 5, set in `init`) | 0xbea139f | |
| &nbsp;&nbsp;`r13 == 0`: `initGraphicsMQD()` | 0xbea13ad | true, else r14=0 and exit |
| &nbsp;&nbsp;`r13 == 5`: `initComputeMQD(5)` | 0xbea13bc | true, else r14=0 and exit |
| &nbsp;&nbsp;`r13 == 1..4`: skipped | 0xbea13a8 | |
| &nbsp;&nbsp;per active ring: `channel->vtable[0x130]()`, `submitMapQueuesPacket(...)` | 0xbea1402 | ANDed into r14 |
| ring-state check (see §3) | 0xbea1420 | all four values **zero** |
| `waitForActiveValue(0, false, 1, 0)` | 0xbea14df | true |
| `(engine+0xc0)->vtable[0x328]()` | 0xbea14f7 | true |
| `jne 0xbea1280` with r14 = 1 | 0xbea150e | **returns true** |

Any failure after startKIQ funnels to 0xbea1519, which calls `this->vtable[0x150]()`
— the `doStop(false)` thunk — and then returns false. That is the "recovery"
the earlier reading was looking for, and it lives in doStart, not powerUp.

## 3. The GC registers doStart touches

The encoded ids are `(dword_offset << 32) | hw_id`, with hw_id 11 = `GC_HWID`
(docs/linux-ref/soc15_hw_ip.h). The access object is `engine+0x30`:
`vtable[0x148]` resolves an encoded id to a register handle, `vtable[0x118]`
reads 32 bits, `vtable[0x120]`/`[0x128]`/`[0x130]` write. Names from
docs/linux-ref/gc_10_3_0_offset.h (all have `_BASE_IDX` 0):

| Encoded id | Offset | Register | Use |
|---|---|---|---|
| 0x1f590000000b | 0x1f59 | `CP_SOFT_RESET_CNTL` | read, `\|= 4`, write — **arg==true only** |
| 0x1de10000000b | 0x1de1 | `CP_RB0_CNTL` | write 0x80000000, later masked write 0x7fffffff |
| 0x1de20000000b | 0x1de2 | `CP_RB_RPTR_WR` | write 0 |
| 0x1df40000000b | 0x1df4 | `CP_RB0_WPTR` | write 0, later read |
| 0x1df50000000b | 0x1df5 | `CP_RB0_WPTR_HI` | write 0, later read |
| 0x0f600000000b | 0x0f60 | `CP_RB0_RPTR` | read |
| 0x0dc20000000b | 0x0dc2 | `GRBM_GFX_CNTL` | write (queue/pipe/me select), restored to 0 |
| 0x1e800000000b | 0x1e80 | `CP_GFX_HQD_ACTIVE` | polled, graphics ring |
| 0x1fab0000000b | 0x1fab | `CP_HQD_ACTIVE` | polled, compute rings |

The ring-state check at 0xbea1420-0xbea14c9 requires
`CP_RB0_RPTR == 0`, `CP_RB0_WPTR == 0`, `CP_RB0_WPTR_HI == 0` and
`(channel+0x50)+0x54 == 0`; any nonzero value clears r14 and the function
returns false. In other words: after mapping the queues, the GFX ring must
read back empty and aligned.

## 4. HAZARD: waitForActiveValue can hang the machine with no timeout

`waitForActiveValue(_eAMD_HW_RING_TYPE ring, bool a, bool b)` is at 0xbea152a.
doStart calls it as `(ring=0, a=false, b=1, 4th=0)` — `esi=0, edx=1, ecx=0`.

```
 bea15e6:  cmpl $-1, %eax        ; read == 0xffffffff -> return false
 bea15ee:  xorb %r12b, %r14b     ; r14 = read ^ expected(1)
 bea15f4:  andb -0x34(%rbp), %al ; & 4th arg  == 0
 bea15f7:  cmpb $0x1, %al
 bea15f9:  jne  0xbea1611        ; ALWAYS taken when 4th arg == 0
 bea15fb:  incl %r15d            ; <- counter: never reached
 bea15fe:  cmpl $0x3e7, %r15d    ; <- 1000-iteration timeout: never reached
 bea1607:  delay 0x64            ; <- 100 us delay: never reached
 bea1611:  testb $0x1, %r14b
 bea1615:  jne  0xbea15c5        ; re-read immediately
```

With the 4th argument 0, the timeout counter and the 100 us delay are both
dead code. The loop re-reads `CP_GFX_HQD_ACTIVE` as fast as the register
window allows, and only two things end it: the register reading 1 (success)
or the register reading 0xffffffff (clean false).

**A GFX ring that stays inactive while the register is readable as 0 is an
unbounded spin inside doStart** — no panic, no log, just a wedged CPU and
eventually a watchdog. This path has not been reached yet because 2cl diverts
doStart before it; removing 2cl exposes it on the very next boot.

Mitigation, before any boot that removes 2cl: hook `waitForActiveValue` and
force its 4th argument to 1. That is not a bypass — it restores Apple's own
1000 x 100 us bound (100 ms), turning a hang into a logged false. Record the
observed register value either way; it is the first direct evidence of whether
the CP has a live graphics queue.

## 5. The display panic has a supported escape hatch

`Kernel-2026-10-11-215906.panic` comes from IOAcceleratorFamily2:

```
__ZN21IOAccelDisplayMachine23display_mode_did_changeEj   0x14e2148e
 14e21507:  callq *0x870(%rax)         ; AMDAccelDisplayMachine::displayModeDidChange
 14e21514:  testb %al, %al
 14e21516:  je    0x14e215c0
 ...
 14e215c0:  testb $0x20, 0xc91(%r15)   ; r15 = displayMachine+0x128 = IOGraphicsAccelerator2
 14e215c8:  je    0x14e21669            ; bit clear -> .cold.1 -> panic
 14e215ce:  xorl  %r12d, %r12d          ; bit set   -> just return false
```

So the bit at accelerator `+0xc91` mask `0x20` turns that panic into a plain
false return. `+0xc88..+0xca0` is a config structure zeroed by
`IOGraphicsAccelerator2::initializeConfigStructure` (0x14e4d78a), and exactly
one site in the whole kext sets this bit — inside
`IOGraphicsAccelerator2::start` (0x14e4e165):

```
 14e4e15a:  callq *0x2d0(%rax)         ; getProperty("IOPCITunnelled", gIOServicePlane,
                                       ;   kIORegistryIterateRecursively|kIORegistryIterateParents)
 14e4e160:  testq %rax, %rax
 14e4e163:  je    0x14e4e192
 14e4e165:  orb   $0x20, 0xc91(%r14)   ; property present -> set the bit
```

The bit is Apple's **eGPU / Thunderbolt-tunnelled** flag: a hot-pluggable GPU
must not panic the kernel when a display mode change fails, a built-in one
does. Because the lookup walks the registry parents, publishing
`IOPCITunnelled` on the 680M's `IOPCIDevice` — the same place the peer
already sets `RBGPULoad`, `LoadHWServices`, `LoadAccelerator` and
`@0,name` — is enough, with no code patching at all.

Be honest about the side effects before using it: `IOPCITunnelled` also makes
`IOGraphicsAccelerator2::start` read `Removable` right after, and other code
(IOPCIFamily, AMDSupport, WindowServer) treats a tunnelled GPU differently —
power management, surface eviction, and whether it can drive the built-in
panel. It is the right switch for *surviving* the bring-up, and a wrong
permanent answer. It belongs behind its own boot-arg.

## 6. What to instrument next (log-only, one boot)

Revert 2cl first; it cannot pass. Then, in one boot:

1. Force `waitForActiveValue`'s 4th argument to 1 (§4) and log `(ring, read
   value, return)`.
2. Log `submitSetResourcesPacket`'s return.
3. Log `initGraphicsMQD`'s return (it is a different function from
   `initComputeMQD`, and the late path needs it; nothing has exercised it yet).
4. Log each `submitMapQueuesPacket(ring, …)` return.
5. Log the four ring-state values of §3 before the check.
6. Log `(engine+0xc0)->vtable[0x328]()`'s return.

That sequence names the exact exit without adding a single new hardware write
beyond the ones Apple's own code already makes on this path.
