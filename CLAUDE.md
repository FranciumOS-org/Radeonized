# RembrandtGPU — notes for Claude

> **New session: read `docs/HANDOFF.md` FIRST.** It is the current *state* —
> what works, what is open, the frontier, the verified facts and the
> procedure. `docs/JOURNAL.md` is the chronological evidence journal
> (~17 000 lines): authoritative for what was measured, in order, including
> every withdrawn reading, and **not to be trimmed**. Grep it by milestone tag
> (`2ks`, `2lg`) for the history of one step; never read it whole.
> `docs/HANDOFF-FULL.md` is a stale dump from 2026-10-05 — ignore it.

Goal: graphics acceleration for the Radeon 680M (Rembrandt, `1002:1681` rev
0a, GC 10.3.3 / DCN 3.1.2 / PSP+SMU 13.0.x / SDMA 5.2.3) on macOS. Plan:
`docs/PLAN.md`. Machine: ASUS TUF A15 FA507NU (Ryzen 7 7735HS), macOS 15.8.1
(24H32), OpenCore. Boot-args include `-wegnoegpu`; csr-active-config **0xA43** (not
0xA03 as this line said until 2026-10-22; measured from NVRAM. SIP reports
Filesystem Protections *disabled*, yet the dyld cryptex path still refuses
writes with EROFS because its files carry the `restricted` flag, which the
sealed-OS policy enforces independently of the CSR bits).

## Status in three lines

**BOOT 156 / 2rc: AGDC REGISTERED AND STARTED; PANEL FROZE.** Loaded
667372BC matches, nonce 156, solved 0x3ff, started1, iop_verify1. Actual
WindowServer vendor/capability queries4/1, all one-shots consumed, SSH and
registry live. Next request **0x921 = getLinkConfig (0xb0-byte structure)**
is unimplemented. Our blanket input-length guard hid its failure; 2rc fixes
that and retains filter return codes. plugin_check now captures display logs
and a root WindowServer sample. GPU progress errors are also present, not
yet explained. **Next is boot-link timing/state and plane support; no repeat
boot needed just for this diagnostic build.** JOURNAL **2rc**.

**2rd/2re: 0x921 IS NOW IMPLEMENTED FROM REAL TIMING, AND IT NEEDED NO DCN
CODE.** `AGDCLinkConfig_t` is not an AMD structure: it is 0xb0 bytes and
embeds the **public** `IODetailedTimingInformationV2` at **+0x04**, with the
three `__reservedA` words the header calls "Init to 0" reused by AGDC as the
link id (+0x04) and flags (+0x08). Measured in `getLinkConfig` (`0xc009184`),
whose block copy is shifted by exactly `-0xc` = `sizeof(__reservedA[3])`, and
verified by **`offsetof`** - `sizeof(V2)` is 0xa0, so `0x04 + 0xa0 = 0xa4` is
exactly the next written field and the whole thing sums to the 0xb0 Apple's
length check demands. So the honest source for every timing field is
`IONDRVFramebuffer::getCurrentDisplayMode` + `getTimingInfoForDisplayMode`,
both **defined** in IONDRVSupport and **neither routed** - resolved and
**called**, so rule 3 is untouched, nothing is patched and no register is
read or written. **AND IOPRESENTMENT VALIDATES NO TIMING FIELD**:
`__GatherAGDCLogicalDeviceCapabilities` (`0x7ff812e9781a`) fails only on a
non-zero IOReturn or a zero `outLen`, copies `reply+0x10..0x9b` into its own
cache at `this->0x19b0c` and returns success either way - so a zero-filled
reply would **pass** and silently poison that cache, which is why a missing or
invalid timing is a **refusal** here, never a zero fill. The ONE field it acts
on is **flags bit 0**, which makes it issue selector **0x925**
(`getPortStatus`) and then a 0x24-byte follow-up, so **bit 0 stays clear** and
bit 3 (which Apple sets unconditionally) stays set. The bridge reports one
framebuffer with **no link and no stream** - link id `0xFFFFFFFF`, stream
index 0, state `0x80000000`, Apple's own "none"/"unknown" - which is what it
actually knows; whether anything past this stage needs a real link is **NOT
established**, and both are single named constants in `rb_agdc.h`.
Reachability is traced, not inferred: 0x921 runs
`0x13dfc32b → 0x13dfc5b8 → 0x13dfd1f1 → 0x13dfd204 → 0x13dfcf0d`, which does
`callq *0x858(%rax)` - the vendor slot the bridge overrides. Corrected my own
first cut: **Apple never checks `inLen`**, only the output length, and input
and output are the same userspace structure. **NOT VERIFIED ON HARDWARE.**
Read `agdc_tim_src 3`, `agdc_tim_err 0`, `agdc_link_result 0` first, and
`agdc_tim_h`/`agdc_tim_v` must be the panel's own numbers or nothing
downstream means anything. Still absent: **0x711 /
`getFbExtendedCapability` (0x196c bytes)**, truthful plane capabilities, the
bind/transaction/scanout backend, and any explanation for the frozen panel or
the `checkGPUProgress` errors. UUID **B8E38412**; imports 362/362 unchanged.
Detail: JOURNAL **2rd**, **2re**; `docs/SPEC-AGDC-BRIDGE.md`.

**2rf: 0x711 IS ALSO IMPLEMENTED, AND THE WHOLE STAGE NEEDED ONE
CAPABILITY.** `AGDCFBGetExtendedCapability_t` is a **query**: `u8` index +0x00,
`u32` type +0x04, `u32` count +0x08 (**in** capacity, **out** filled), then
**8 entries of 0x32c** from +0x0c - closed three ways
(`getFbCapabilityForScaler` bzeroes `entry+0x08` for 0x324; `8*0x32c+0xc` is
exactly 0x196c; 8 is the capacity IOPresentment pre-loads).
`__GatherAGDCDisplayPipelineCapabilities` (`0x7ff812e979dc`) issues **sixteen**
0x711 queries, one per type bit, and **eleven are for types Apple itself does
not implement** - so failure is tolerated by design. **`0x2006` comes from
exactly two sites, both belonging to the FIRST query, type 0x10 (the
scaler)**: a non-zero IOReturn and a returned **count of zero**. Later
failures fall into the next query and the last is preceded by
`xorl %r14d,%r14d`, so even its failure returns success; a count >= 9 is
clamped to 8. **So the stage reduces to: answer type 0x10 with count >= 1.**
The bridge reproduces Apple's own **no-scaling** entry (`0xbfed05a`) with the
extents from the same real timing 0x921 uses; every other type returns
Unsupported **without writing a byte**, and entries 1..7 stay zero.
**NOT established, not guessed:** which slot of each pair is width vs height -
`updateStreamComposition` (`0xbfe9270`) fixes the pairing but not the axis
names, and it cannot change the meaning, because source and destination
extents are equal so transposing both still says 1:1. Two guards passed for
the wrong reason and were fixed: the type check failed the **build**
(`-Werror=unused-but-set-variable`) rather than a test, and the zero fill was
invisible against an already-zeroed buffer. **ONE BOOT NOW COVERS BOTH
STAGES.** Read in order: `agdc_tim_src 3`, `agdc_tim_err 0`,
`agdc_tim_h`/`agdc_tim_v` matching the panel, `agdc_link_result 0`,
`agdc_extcap_type 0x10`, `agdc_extcap_result 0`. UUID **5719404C**; imports
362/362 unchanged; Info.plist graphics-dependency count still 0.
**NOT VERIFIED ON HARDWARE**, and a published capability is not a picture: the
bind/transaction/scanout backend and all plane state are still absent, as is
any explanation for boot 156's frozen panel or the `checkGPUProgress` channel
errors. Detail: JOURNAL **2rf**.

**2rg CORRECTS 2rd ON THE SENTENCE THAT MATTERED, AND IT COSTS ONE NEW
REFUSAL.** 2rd said the 0x921 reply goes into "a cache it never reads in this
image." **It reads it twenty-four times, and that cache IS the display's
advertised capability.** `__gatherCapabilities` (`0x7ff812e95370`, 0x104 bytes,
read whole) is the function above both gathers: it aborts only on
`__GatherAGDCLogicalDeviceCapabilities` returning non-zero, then **computes**
`refresh = pixelClock / ((hActive+hBlanking) * (vActive+vBlanking))` after
storing **60.0f** (`0x42700000`) as the default - the `ucomiss` constant at
`0x7ff812ea3468` reads **`0.0f`** (measured; its neighbour is 160.0f), so that
branch is only a divide-by-zero guard, not a plausibility test.
**And the field names are APPLE'S OWN**:
`IOPresentmentCreateEndPointDictionary` (`0x7ff812e9550c`) `leaq`s fourteen
cached fields in turn into CFNumbers keyed `PixelClock`, `MinPixelClock`,
`MaxPixelClock`, `HorizontalActive`, `HorizontalBlanking`,
`HorizontalSyncOffset`, `HorizontalSyncPulseWidth`, `VerticalActive`,
`VerticalBlanking`, `VerticalSyncOffset`, `VerticalSyncPulseWidth`,
`HorizontalSyncConfig`, `VerticalSyncConfig`, `VerticalBlankingExtension` -
**field for field, in order, matching the public header**, which is a stronger
confirmation of the `reply+0x04` embedding than 2rd's shifted block copy or the
`offsetof` sum, because it is Apple naming the fields instead of us matching
offsets. The derived values publish as **`LooksLikeWidth`** / **`LooksLikeHeight`**
(`0x19c58`/`0x19c60`) and the computed refresh under **`RefreshTime`**,
**`MinRefreshTime`** and **`MaxRefreshTime`** - three keys, one address. So
2rd stands exactly as measured: IOPresentment **validates** no timing field, and
**consumes** nearly all of them.
**THE NEW REFUSAL: a zero `pixelClock`.** It **passes** the `> 0.0f` guard
(which tests the *totals*, not the clock) and publishes **0.0 Hz**, where a
zero total would at least leave Apple's stated 60.0f - so it is the one
poisoning left after 2rd, and `rb_agdc_link_config` refuses it before touching
the output. The zero-active refusal is now independently justified too:
`IOPresentmentGetCurrentEndPointDimensions` (`0x7ff812e96ed6`) writes **neither**
output when `hActive` or `vActive` is 0, leaving the caller's values untouched;
it also shows `horizontalScaled`/`verticalScaled` (V2+0x18/+0x1c) **override**
the actives when set, which the bridge inherits from the NDRV by copying the V2
body wholesale.
**AND flags bit 0 IS SAFE - 2rd overstated it.** Bit 0 calls
`__GatherAGDCLinkCapabilities(reply+0xa4)` (`0x7ff812e98af6`), which issues
0x925 then 0x988 and tolerates every failure, returns **void** with `r14`
(the success code, zeroed at `0x7ff812e979b4`) callee-saved across it, and
whose only effect is `0x19c78` - **four writers, ZERO readers in the whole
image**. Bit 0 stays clear because the bridge knows of no link to describe, a
reason of honesty and not of hazard; recorded because a wrong reason for a
right decision breaks the next time it is revisited. Also: `reply+0xa4`/`+0xa8`
are **one** field - a single `movq` passed as `AGDCStreamAddress_t` - so the
zero at `+0xa8` is that address's high half, not padding.
New keys `agdc_tim_hblank`/`_vblank`/`_clk` and **`agdc_tim_mhz`**, the refresh
*Apple's own formula* will produce from this boot's timing, published even when
the reply is refused; `agdc_tim_err` gains **5 = zero pixelClock**. Three
deliberate breaks trip 4 / 2 / 6 assertions. UUID **E56F3227**; imports 362/362
unchanged; Info.plist graphics-dependency count still 0. **NOT VERIFIED ON
HARDWARE.** Reading order gains one line: after `agdc_tim_h`/`agdc_tim_v`, read
**`agdc_tim_mhz`** against the panel's 143 833 - 0 or 60 000 means the NDRV's
detailed timing is incomplete and that, not the bridge, is next. Detail:
JOURNAL **2rg**.

**BOOT 160 / 2rn: OTG0 WORKED, THE IOP PATCH LANDED, AND THE BRIDGE IS SIMPLY
TOO LATE.** No black panel, desktop live, `plugin_check.sh` run from inside the
boot. Rule 8/15 clear: `rbgpu-boot` **160**, UUID **9028175A** = `build/out`,
`diag_used` **564** of **768**, both one-shots cleared.
**THE OTG0 SOURCE WORKS AND THE SEGMENT CONSTANT WAS RIGHT:** `otg_err 0`, and
2oq's four-register signature held live - `otg_ctl0 0x11201` with
`OTG_MASTER_EN`, `otg_ctl1/2/3` all `0x300`, `otg_off_ok 3` - so DMU segment 2
= `0x34c0` is confirmed on hardware. Every register matched 2oq
(`htot 2106`, `vtot 1129`, `hbse 0x7f07ff`, `hsa 0x3c0000`, `vbse 0x14044c`,
`vsa 0x50000`), the converter accepted them (`agdc_otg_ret 0`,
`agdc_otg_src 2`) and produced **`agdc_tim_h 1920`**, **`agdc_tim_v 1080`**,
`agdc_tim_err 0`, **`agdc_tim_cached 2`**. Boot 159's 0x921 refusal is **gone**.
**BUT THE PIXEL CLOCK CAME OUT 3.3 % HIGH AND THE METHOD WAS WRONG, NOT THE
WINDOW:** `otg_frames 15` in `otg_ns 100 977 001` = **148.549 Hz** where the
panel scans at 143.833 - it really delivered **14.52** frames, and an integer
counter over a FIXED window can only resolve +/- one frame, so
`agdc_tim_clk` read 353 198 717 against a true 341 987 984. Lengthening the
window only shrinks that linearly and costs delay on a path that is already too
late. **FIXED by latching both endpoints on a counter EDGE**: the frame count
is then exact by construction and the residual is the difference between two
100 us polls, ~**0.1 %** over 16 frames (~111 ms) - ~30x better for the same
elapsed time. `otg_frames`/`otg_ns`/`otg_polls` are published so the arithmetic
is auditable from any capture.
**THE IOP PATCH LANDED:** `iop_verify 1`, `iop_patched 1`, `iop_match 1`,
`iop_saw_ws 1` - 2qo's five bytes went into WindowServer's own memory and the
vendor gate opened, as in boot 150.
**THE REAL FINDING IS AN ORDERING RACE**, and only the timestamped log shows
it - same process, same thread: at **33.920** the wrangler's own `debug-gpus`
reads **`agdc 0x000000000`**; **21 ms later** IOPresentment logs
*"Unable to get AGDC information. Return code: 0x10000003"*, then *"System set
to use IOP but no display pipes are present"*; and at **34 seconds** the same
dump reads **`agdc 0x100000525 /RembrandtGPUAGDC`**. **The bridge is published,
paired and correct - it just arrives after the only question that mattered was
already answered.** `agdc_user_vendor`/`agdc_user_cap` are **absent** where 159
had 4 and 1, so WindowServer never queried us, and 0x711 was **never reached**
(not refused). **`0x10000003` is the same constant as boot 150 with a different
cause** - in 150 the association map was empty because no per-GPU AGDC existed,
in 160 because none existed *yet*; the error string alone could never have
separated those, the timestamps did.
**SUSPECT, NOT ESTABLISHED, AND IT IS MINE:** 2rm put a **100 ms `IOSleep`** in
`sampleTiming()`, which runs immediately before `service->start()` - delay added
to the publish path in the same change that made the publish matter. 159
published early enough to be queried; 160 did not. But 160 also differs by
`rbgpu-iop` being armed, which changes when WindowServer runs, so blaming the
sleep would repeat 2qm's error. Both are live suspects, and boot 161 will say
which: new keys **`agdc_t_presample_ms`**, **`agdc_t_sampled_ms`**,
**`agdc_t_started_ms`** (uptime ms from `mach_absolute_time`, directly
comparable with the log's timestamps) turn "too late" into "late by N, of which
the sample cost M".
Re-checked: `fb0:0` reads `/IONDRVFramebuffer` where 159 read `/.Display_boot`
(instance name, not a different driver, nothing downstream reads it); `gpuc`
still **0** as 2r1 predicted; `agdc_failed_attr 3` is attribute 3 from a new
caller in Apple's own `attr <= 0x7f` group, refused and harmless. UUID
**A83C0170**; imports **362 of 362**; graphics deps **0**. Evidence at
`logs/boot160/`. **The display pipe is still not created, and the reason is now
a SCHEDULE rather than a capability.** Detail: JOURNAL **2rn**.

**2rm: OTG0's LIVE TIMING IS NOW THE 0x921 SOURCE, AND EVERY ACCESS IS A
READ.** 2rl left 0x921 refusing because the NDRV answers
`getTimingInfoForDisplayMode` with `flags = 0`. The panel's real timing was
already on disk from 2oq/2os: `H_TOTAL 0x83a`, `H_BLANK_START_END 0x007f07ff`,
`H_SYNC_A 0x003c0000`, `V_TOTAL 0x469`, `V_BLANK_START_END 0x0014044c`,
`V_SYNC_A 0x00050000`. **The field order is established by arithmetic, not by
a header this tree does not have** (`dcn_3_1_2_offset.h` is here, no
`sh_mask`): h active 2047-127 = **1920**, front 59 + sync 60 + back 67 =
**186** = 2106-1920; v active 1100-20 = **1080**, front 29 + sync 5 + back 15 =
**49** = 1129-1080. Three independently derived porches summing to the
blanking the totals imply, **on both axes**.
**IT CORRECTS 2os:** the registers hold the TRUE totals **2106 / 1129**; 2os
used 2107 x 1130, one too many per axis, with which the porches do not sum. So
the pixel clock is **341 987 984 Hz (341.988 MHz)**, not 342 453 428 - 0.14 %
out, harmless so far, but the journal's number was wrong.
**`src/common/rb_otg.h`** converts raw registers -> a 0xa0-byte
`IODetailedTimingInformationV2`, freestanding and host-tested. V2 offsets come
from walking the **public** Kernel.framework header's member types (UInt64 is
8-aligned, so `pixelClock` = 0x28 and the struct closes at 0xa0), **not** from
counting header lines (2q4's lesson); all four offsets `rb_agdc.h` pinned from
the consumer side agree and the size equals `RB_AGDC_TIMING_SIZE`, both
asserted. `kIOSyncPositivePolarity` is a local constant quoting
`IOGraphicsTypes.h:1002`, never an `#include` - Info.plist graphics deps stay
**0**. Field pairs are read **order-agnostically** (span + lower edge), so the
hardware's start/end naming cannot matter, and every refusal names itself
(`_HACTIVE`, `_HTOTAL`, `_HSYNC`, `_REFRESH`, ...). An unprogrammed OTG reads
zeros and so **refuses**, never publishing a zero timing into Apple's
capability cache. **A guard that could not fail was removed before shipping:**
`front + sync + back == blanking` is an identity once active is
`hb_hi - hb_lo`, so it could never trip.
**`rbgpuOtgSample()` reads; nothing writes a DCN register** - that is what
keeps it out of the user-decision category, since a wrong read reports a wrong
number and cannot blank the panel. The plugin's `g_disc` is a two-IP stub with
no DMU entry, so **DMU segment 2 = `0x34c0`**, parsed by this project's own
`rb_disc_parse` from this machine's real table (DMU **3.1.3**, nbase 5,
`base[2] = 0x34c0`; `OTG0_CONTROL` at dword `0x5001`). **A constant is an
assumption, so it is checked** against 2oq's four-register signature - OTG0
with `OTG_MASTER_EN` and OTG1-3 at exactly `0x300` - with all four values
published either way; failing it refuses instead of converting garbage. The
refresh is **measured** (`OTG0_FRAME_COUNT` across a 100 ms `IOSleep` on the
bridge's own `thread_call`, `absolutetime_to_nanoseconds` for the elapsed,
clamped 20-400 Hz), the method `RembrandtProbe.cpp` already uses. Rule 3
untouched - **nothing routed**; rule 17 is why the clock is
`hTotal * vTotal * measured refresh` and not CVT/GTF.
Host test runs the converter on this machine's own values, asserts
1920/1080/186/49/59/60/29/5, the clock as arithmetic, both polarity mappings,
that every untouched byte is zero, that each refusal leaves the output alone,
and that the guards **compose** with `rb_agdc_link_config`. Three deliberate
breaks trip **21 / 27 / 1** assertions.
**Reading order after `agdc_tim_err 3`:** `agdc_otg_bound 1`, `agdc_otg_src`
(0 none / 1 refused / 2 sampled), `otg_err` **0**, `otg_ctl0` + `otg_off_ok 3`,
the six raw registers `agdc_otg_htot/_hbse/_hsa/_vtot/_vbse/_vsa`,
`agdc_otg_mhz`, `agdc_otg_ret` **0**, then **`agdc_tim_cached 2`** (2 = OTG0,
1 = NDRV, so the source is never ambiguous). `agdc_tim_err` is set to 3 before
the fallback and cleared only on success, so a boot where it does nothing reads
exactly as 159 did. UUID **9028175A**; imports **362 of 362**; graphics deps
**0**. **NOT VERIFIED ON HARDWARE**, and what one boot can show is bounded:
0x921/0x711 answering is not a display pipe, because `iop_oneshot 2` means
IOPresentment never ran in 159 - arm **`rbgpu-iop=1` alongside
`rbgpu-agdc=1`** to reach that layer. Detail: JOURNAL **2rm**.

**BOOT 159 / 2rl: THE PANEL LIVED, AND THE WRANGLER PAIRED OUR BRIDGE.** The
user ran `plugin_check.sh` from inside the armed boot, on a live desktop - the
first armed boot since 156 where publishing the AGDC did not freeze the screen.
Rule 8/15 clear: `rbgpu-boot` **159** in capture and live, uptime 4 min,
`kextstat` UUID **8CB914E5** byte-equal to `build/out`, `diag_used` **542** of
**768**, and **`agdc_oneshot 0`** - the one-shot cleared, so 2rh's failure did
not recur.
**THE HEADLINE IS APPLE'S OWN `debug-gpus` DUMP:
`gpu 0xfbc7 agdc 0x100000536 /RembrandtGPUAGDC`.** Every boot before this read
**0**, and 2qq/2qr built a chain on that zero. **2r0's mechanism is confirmed
live** - publish an IOService that `safeMetaCast`s to
`AppleGraphicsDeviceControl` and the wrangler's own matching notification pairs
it: no `IOFramebuffer` subclass, no vtable, no personality.
`AppleGraphicsDeviceControl` went **1 -> 2**, `AppleGraphicsDeviceControlClient`
is live, and `agdc_user_vendor 4` / `agdc_user_cap 1` - WindowServer queried
through it. **`gpuc` is still 0**, exactly as 2r1 predicted (no writer in the
wrangler).
**THE OPT-OUT WORKED BUT IS NOT ESTABLISHED AS THE FIX:**
`agdc_policy_optout 1`, `AppleGraphicsDevicePolicy` **0** instances - AGDP did
not attach - **but boot 159 differs from 156 by SIX milestones** (2rd-2rh,
2rk), and 2rh alone moved the timing read off WindowServer's thread. Calling
the living panel `NeedsPolicyDriver`'s doing would repeat 2qm's error. **The
clean A/B is one boot: `-rbgpuagdcpolicy`**, which leaves the property absent
with everything else byte-identical.
**0x921 REFUSED, AND THE REASON IS THE NDRV, NOT THE BRIDGE.**
`agdc_tim_src 3`, `agdc_tim_mode 100`, `getTimingInfoForDisplayMode`
**succeeded**, then **`agdc_tim_flags 0`** - `kIODetailedTimingValid` clear - so
`agdc_tim_err 3`, `agdc_link_result -2`, `agdc_logical_ret 0xE00002C7`, and
**every `agdc_extcap_*` key ABSENT** (0x711 unreached: IOPresentment aborts the
pipeline gather after the logical-device gather fails). **Exactly the condition
2rg's reading order pre-stated, and the refusal is CORRECT** - a zero fill would
have passed Apple's checks and poisoned its capability cache, and
`agdc_tim_flags 0` publishes what was refused (rule 19). Also:
`agdc_last_attr 0x82` returned **0**; `agdc_vendor_unsupported 0x2001`.
**AND THE NDRV HAS NO DETAILED TIMING TO GIVE - MEASURED.** `IOFBConfig`'s one
mode `ID=100` decodes to width **1920**, height **1080**, refreshRate
**0x8fd53f/65536 = 143.83 Hz**, flags **0x40000b** (containing 2q7's
`kDisplayModeAcceleratorBackedFlag` 0x400000, an independent re-confirmation) -
and **every remaining word is zero**: no blanking, no sync offsets, no pixel
clock. **`IODisplayEDID` is absent from the entire registry** (controls in the
same dump: `IOFBMemorySize`, `IOFBConfig`, `IOFBProbeOptions`, `AppleDisplay`
x2, `IODisplayConnect` x2), as are `IOFBDetailedTimings`,
`IOFBCurrentPixelClock`, `IOFBTimingRange`. Those two reads come from a
**later UNARMED boot** (`logs/boot159/ioreg-UNARMED-later-boot.txt`) but are
arm-independent framebuffer properties - stated because that dump was briefly
mis-filed as 159's.
**IOPRESENTMENT NEVER RAN, SO NOTHING HERE TESTS THE DISPLAY PIPE:**
`iop_oneshot 2` = **not armed**, so 2qo's patch was never placed and
`UseIOPresentment` returned 0 at the class-name gate.
`"Unable to get AGDC information"` appears **0** times and **that is NOT
evidence the 2qq/2qr blocker is gone** (rule 14). The control is broken two
ways: `plugin_check.sh`'s predicate does match
`senderImagePath CONTAINS "IOPresentment"`, but **`--style compact` never
prints the sender path**, so grepping the capture for that word returns 0
regardless. An unfiltered query returned 33 AGDC lines (the `debug-gpus` dump
among them) - that is the control. **To test the pipe, arm `rbgpu-iop=1`
alongside `rbgpu-agdc=1`.**
Re-checked, still dead: **`com.nullmoth.NVRMAGDC`** logged four
kernelmanagerd lines and `error: tried to insert an invalid codeful kernel
extension`, and `kextstat` (control **186**) shows it **NOT loaded**.
Compositor unchanged: `"Metal compositor activated."` twice, neither other
line - same as 149.
**NEXT IS A READ, NOT A GUESS: OTG0's live timing registers**, which the
firmware already programmed (`OTG0_CONTROL = 0x00011201`); read-only, named in
`docs/HANDOFF.md` §4, and permitted by `rb_probe.c`'s rules at probe level 2.
Synthesising blanking from CVT/GTF would be rule 17 verbatim. Detail: JOURNAL
**2rl**.

**2rk: `NeedsPolicyDriver` IS AN OPT-OUT, AND ITS ABSENCE RECRUITS
`AppleGraphicsDevicePolicy` ONTO OUR BRIDGE - 2rj HAD THE SENSE BACKWARDS.**
Instrument first (rule 26, again): **`strings -a` SILENTLY UNDER-REPORTS** on
the 373 MB `SystemKernelExtensions.kc` - 11 hits for `AppleGPUWrangler` where
a byte search finds **32**, and **0** for `NeedsPolicyDriver` where it finds
**4**; `grep -rl --binary-files=binary` found **neither**, control included.
Use `mmap` + `bytes.find` on a KC, never `strings`. The four sites:
AGDC `0x13dfdf9f` (the string, LEA'd in `start`), **AGDP `0x13e0f0c3` - read at
`0x13e098ce`, written at `0x13e0e800`** - `AppleMuxControl` `0x13e392b6` and
`AppleMuxControl2` `0x13e50a72`, both **writes**.
**The one reader is `AppleGraphicsDevicePolicy::probe` (`0x13e0989a`)**, and it
reads: `*score=0; r=IOService::probe(...)` (a **non-virtual super call through
`__ZTV9IOService`**, so rule 7's dump-offset form - `*0x5c8` is address-point
`0x5b8`); `*score=1000`; then
`OSDynamicCast(OSBoolean, provider->getProperty("NeedsPolicyDriver"))` and
**`if (!b) return r; if (b->getValue()) return r; *score=0; return NULL;`**
(slots named from the boot KC: `0x2b8` getProperty, `0x278`
**`setProperty(const char *, bool)`**, `0x670` attach, `0x5c0` start, OSBoolean
`0x128` getValue). So an **ABSENT property is CONSENT**, and `vt[0x278]` with
`edx=0` means `AGDC::start` **CLEARS** the flag for `+0x28` in {7,8} - Apple's
own AMD value **opts OUT** of the policy driver, and our `2` opts **in**.
**AND AGDP MATCHES EVERY AGDC:** its personality is
`IOProviderClass = AppleGraphicsDeviceControl`, `IOResourceMatch = IOKit`, no
PCI or board-id gate at match time, and provider matching is a `metaCast` so a
subclass qualifies (2r0). The two `AppleMuxControl` personalities are **not**
ours - `IOACPIPlatformDevice` + `IONameMatch` `GMUX`/`GPUC`, a gmux this
laptop lacks; live class list has `AppleMuxControl` **absent** and
`AppleGraphicsDevicePolicy` present at **0** instances, so AGDP's kext is
loaded and merely has nothing to attach to.
**SO THERE IS A NEW FREEZE CANDIDATE: boot 156 published an AGDC with the
property absent, making AGDP eligible to probe and start on it** - a
mux/dual-GPU policy driver on a single-GPU APU. **NOT ESTABLISHED as the
cause** (never observed attaching; boot 156's capture never reached disk; the
baseline shows AGDP at 0 against the wrangler, which is itself an AGDC).
**THE FIX IS ONE PROPERTY AND IT IS APPLE'S OWN:** the bridge publishes
**`NeedsPolicyDriver = false`** between `init()` and `attach()`, exactly where
AGDP's own nested AGDC does it. **`-rbgpuagdcpolicy`** leaves it absent for an
A/B test; **`agdc_policy_optout`** says which way it went (1 = opted out);
verdict **16** = the property could not be published, which **refuses the
whole bridge** rather than publishing a service that invites a driver we did
not mean to invite.
**AND 2rj's `-0x4(%r15)` IS PINNED: it is `vendorInfo+0x28`, the SAME SLOT.**
Buffer 1 is `rbp-0x138`, so `0x28(%r15)` is `rbp-0x110`; `%r15` is then
**reassigned** to `rbp-0x10c` for the `0x980` query, so `-0x4(%r15)` is
`rbp-0x110` again - register reuse, no second field, and buffer 2 spans
upward from `rbp-0x10c` so it **cannot clobber it**. `start` therefore tests
one value twice, and **never reads the `0x980` reply at all** - it is fetched
and dropped. UUID **8CB914E5**; plugin imports **362 of 362**, 0 unresolved;
Info.plist graphics deps **0**. **NOT VERIFIED ON HARDWARE.** Read
**`agdc_policy_optout 1`** before the timing block. Detail: JOURNAL **2rk**.

**2rj: `AGDC::start` QUERIES OUR VENDOR TWICE AND BRANCHES ON THE ANSWERS, AND
IT HOLDS THE KEXT'S ONLY INFINITE WAIT.** `0x13dfd37c` is
**`setDeviceAttribute`**, a 36-byte thunk to
`filtered_doDeviceAttribute(api=1 SET)` - **2r1 confirmed from AGDC's own
side**. Swept whole-kext: **one** `waitForMatchingService`, one `IOSleep`, 12/14
recursive lock/unlock. The wait is in `start` and it is
`waitForMatchingService(serviceMatching("AppleGPUWrangler"), **$-1 = UINT64_MAX**)`
- **infinite**, bracketed by `mach_absolute_time` so Apple measures the delay.
Benign here (2r5: the wrangler is present), but this bridge calls
`service->start(pci)` from its own `thread_call`, so were it ever absent that
thread blocks **forever** with no timeout and no diagnostic of ours;
`agdc_stage` would sit at 60.
**`start` RE-ENTERS OUR CODE THREE TIMES** - `*0x858` at `0x13dfbb60` and
`0x13dfbba1`, plus `filtered_doDeviceAttribute` at `0x13dfbef6` - and the first
two decode fully: query attr **1**, then `if ((vendorInfo->0x28 - 1) > 1u)`
skip, else query **0x980**; then `if ((eax - 7) > 1u)` skip, else
`setProperty("NeedsPolicyDriver")`. So **`vendorInfo+0x28` must be 1 or 2** -
which **2r5 derived independently from IOPresentment's os_log**, two files
agreeing - and the bridge writes **2**, taking the query-capability branch,
correctly. **New decode:** `+0x28 == 7` (what Apple's AMD path reports)
**skips 0x980 entirely** and sets **`NeedsPolicyDriver`**, so `+0x28` is not a
version - it selects *"policy-driver GPU, do not ask me"* vs *"ask me and
decide from the reply"*.
**A TIDY LOCK STORY WAS DROPPED BEFORE IT BECAME A CLAIM:** three `*0x858`
sites sit inside `filtered_doDeviceAttribute` and `0x13dfc6ff`'s lock is not
unlocked until `0x13dfd0f3`, which looked like "our callback runs under AGDC's
lock" and would have explained a convoy. But 2rd's traced 0x921 path
(`0x13dfc32b → 0x13dfc5b8 → 0x13dfd1f1 → 0x13dfd204 → 0x13dfcf0d`) **skips that
whole region**, so **no lock is held at our callback**. Recorded as a
non-finding, because this was the second tidy lock story this week and the
first (2rh) reached a commit before being measured false.
**THE FREEZE IS STILL UNEXPLAINED** - boot 156 froze with `agdc_started=1`
before any timing code existed, and 2rh/2ri/2rj have not touched it. What is
new: `start` **branches on our replies**, so the bridge's answers configure
Apple's object rather than merely being reported. Live lead: find
**`NeedsPolicyDriver`**'s readers, and pin down which field the `TAIL`
comparison reads after the 0x980 call - it is `-0x4(%r15)`, four bytes *below*
the capability buffer, so it is **not** obviously a field of our reply and must
be established before anything is concluded from it. No code changed.
Detail: JOURNAL **2rj**.

**2ri RETRACTS 2rh's HYPOTHESIS: NEITHER TIMING GETTER TAKES A LOCK, AND THE
FROZEN PANEL PREDATES ALL OF 2rd-2rg.** `getCurrentDisplayMode` is **34 bytes**
- two loads from `this->0x1e8`/`0x1ec`, two stores, `return 0`, **no call at
all**. `getTimingInfoForDisplayMode` goes through `_doStatus` (`0x15001610`),
which takes **no lock and never blocks**: `_ml_at_interrupt_context` (named via
`kc_stubs.py`, 1678/1678), then an `inGate` predicate, and on failure **only**
`OSReportWithBacktrace` with **Apple's own string
`"%s::_doStatus(%d) not gated\n"`** - then **every path falls through to the
call anyway**. So the inversion 2rh proposed cannot happen, and the cheap static
read that proves it was available the whole time (the rules' own closing
variant: *read the evidence already in hand*).
**AND THE FREEZE WAS NEVER 2re's:** CLAUDE.md's own line above says
*"BOOT 156 / 2rc: AGDC REGISTERED AND STARTED; PANEL FROZE"* - boot 156 froze
with `agdc_started=1` **before 2rd/2re/2rf/2rg existed**. Publishing the bridge
freezes the panel and always has. **Itemised, the bad night was: (1) the freeze,
known since 156, cause STILL UNKNOWN and untouched; (2) `plugin_check.sh`'s
unguarded `sample`, mine, fixed; (3) the one-shot not clearing, known from 2qy,
mitigated; (4) the capture never reaching disk, fixed.** Two fixed, one
mitigated, **and the thing that freezes the screen is none of them.**
**2rh's fix still stands, for a measured reason instead of a guessed one:** the
vendor callback runs on WindowServer's thread, which is **not** the IOGraphics
gate, so calling from there earned **three** `OSReportWithBacktrace`es per
0x921/0x711 query; once on our own `thread_call` costs one set.
**Also fixed, a real rule-19 defect found while reading:** `vendor()`'s
`agdc_extcap_type` probe read `out+0x04` behind **`if (out)` alone** - a NULL
check, not a bounds check - an out-of-bounds kernel read for any caller passing
fewer than 8 bytes. Now `out && outSize && *outSize >= 8`, with
`agdc_extcap_type_short` publishing the refused length.
**THE OPEN QUESTION IS: why does publishing the bridge freeze the panel?** Not
the timing path. Next read: `AppleGraphicsDeviceControl::start` and the
wrangler's `0x13dfd37c` call on our service. UUID **61BE982B**; imports
362/362. Detail: JOURNAL **2ri**.

**2rh: BOOT 157 HUNG THREE TIMES. THE CALLBACK WAS CALLING INTO
IOFRAMEBUFFER, AND `plugin_check.sh` WAS FINISHING THE JOB.** Not panics -
all three resets recorded **`previous shutdown cause 5`**, not `-128`, and
**no `.panic` file was written**: frozen panel, live SSH, power-cycled. **It
repeated because the one-shot did not clear** - `rbgpu-agdc` survived the boot
that consumed it, so the next two re-armed and re-hung. 2qy had already
measured that failure (`mcr_oneshot 4`) and the activation commands rested on
it anyway. **So between experiments take `-rbgpuagdc` OUT of boot-args**: the
bridge needs the arg *and* the key, so without the arg a stuck one-shot is
inert. That is the rescue that holds, not the one-shot.
**THE DEFECT IS 2re's.** `currentTiming()` called
`IONDRVFramebuffer::getCurrentDisplayMode`/`getTimingInfoForDisplayMode`
**synchronously inside `filtered_doDeviceAttribute`** - WindowServer's thread,
inside a user-client external method - while both expect the IOGraphics
workloop. That inversion's symptom is exactly a wedged desktop with a live
kernel. CLAUDE.md already carried this warning for `-rbgpuvblpump`; 2re asked
the *routing* question (rule 3) and never the *locking* one. **NOT ESTABLISHED
that the lock is the cause** - the capture died with the boot - but the
structure was wrong regardless. **FIXED:** `sampleTiming()` runs on the
bridge's own `thread_call`, **once**, immediately before `service->start()`,
and caches the 0xa0-byte V2 body; `vendor()` serves the cache and **calls
nothing** (verified mechanically). A failed sample still **refuses** 0x921 and
0x711.
**EVIDENCE LOST, AND ONE INFERENCE RETRACTED.** No `logs/<ts>-plugin` from
2026-10-26 exists at all - the directory never reached the disk before the
reset - so the breadcrumbs are now **`sync`ed immediately** after being
written. And `rbgpu-diag-prev` is **NOT boot 157**: rule 8, it carries no
nonce, its numbers are identical to boot 156's and **every 2re/2rf/2rg key is
absent from it** at `diag_used` 548 of 768. **The diag notes are HEX** -
`diag_max=300` IS 768 - so the "check diag_max reads 768" instruction given
with the activation commands was wrong and would have made a good install look
broken.
**RULE 16, TWICE IN ONE NIGHT: `plugin_check.sh:83`'s unguarded
`/usr/bin/sample WindowServer`** (added by me in 2rc) suspends the target and
walks its stacks, and a WindowServer wedged in a kernel call **cannot be
suspended** - so it blocked in the kernel and took the machine down on two of
three attempts, destroying the capture each time. A timeout does **not** fix
it (killing the wrapper cannot unblock a `task_suspend` already in the kernel,
as the script's own `rb_ioreg` comment says of `ioreg`), so the sample is now
**OFF by default**, opt-in with **`RBGPU_WS_SAMPLE=1`**, bounded by the
`rb_ioreg` idiom as a second line only. `compositor_check.sh`'s is bounded too
but stays on, since that script only makes sense on a responsive desktop.
**RULE 14 ON MY OWN REFUSAL PATH:** the old first guard returned NULL with **no
key**, making "unresolved", "no framebuffer" and "never called" identical. Now
**6** = getters unresolved, **7** = no framebuffer, and `agdc_tim_err` is
**seeded to 9** at load so an *absent* key is impossible; **9 means
`sampleTiming()` was never reached**. `agdc_tim_cached 1` confirms the positive
case. UUID **B5419C66**; imports 362/362; Info.plist graphics deps 0.
**`src/common` did not change, so NO host-test case was added** - the fix is in
the plugin's threading, which the host harness cannot reach; that is a coverage
gap, not a justification. **NOT VERIFIED ON HARDWARE.** Detail: JOURNAL **2rh**.

**FRAMEBUFFER WORK IS THE USER'S ACTIVE REQUEST (2ra).** The missing per-GPU
AGDC bridge is now implemented in `src/plugin/rbgpu_agdc.cpp`, built and
host-tested, **published and queried in boot 156**. It clones the inherited AGDC tables,
supplies attributes 1/0x980 for our actual PCI/boot framebuffer, and retains
Apple's entitlement and pointer-sanitization paths. **-rbgpuagdc + one-shot
rbgpu-agdc**, off by default. Next: boot-link and plane protocol/backend
implementation; activation commands and measured verdicts in
`docs/SPEC-AGDC-BRIDGE.md`. No EFI/NVRAM was changed while implementing it.
Do not divert this request into rendering loose threads again.

**2r9 closes the tiled-render-target loose thread.** With the existing
kernel linear force on, plain Metal render targets have GPU descriptor
**SW_MODE 6/10**, but reach the kernel as **type-0x40 raw allocations,
format 0**. `AMDAccelResourceAddr2::initialize` skips its swizzle lookup on
format 0, so its `m0` histogram does not say the client's texture is linear.
Three sizes, each traced and untraced: coordinate-coded rendering and full
readback passed **2,195,456 pixels, zero mismatches**. IOSurface controls use
type 0x82 and mode 0. `make texturepath resourcewatch`; JOURNAL **2r9**.
Both fixes remain necessary; why `_X` is wrong and the exact AppKit backing
store lifecycle remain unexplained. No kext behavior or boot config changed.

**An armed boot is a usable, accelerated Mac** — the CP halt is fixed
(`-rbgpuvramadj`, 2ks/2ku/2kw), zero VM faults, Metal compute bit-exact.

**THE UNIVERSAL FIX IS DONE (2on, boot 130).** `-rbgpumtlkfix` with
`rbgpumtlkscope=*` fixes **every process on the system**, Safari included,
with no wrapper, no injected dylib and no change to any app bundle - so it
survives app updates. 2045 of ~2049 execs patched. The kext hooks
`_vm_shared_region_slide` for the cache slide and
`_task_set_main_thread_qos` for a per-process point at exec, then places a
224-byte cave in `__TEXT,__const` and a 5-byte detour over
`amdMtl_HWL_DetermineSwizzleMode` with `_vm_map_write_user`, verifying both
from the target's own memory. **Lilu's user patcher is irrelevant here - it
is disabled on Big Sur and newer and cannot be re-enabled (rule 22).**

**The defect was never only about text.** It is the swizzle mode the
*userspace* Metal driver picks for client textures; images were affected too
(2on). Everything from 2lg to 2ol called it "the text defect" because text is
where it was noticed. Policy `fix`: LINEAR for non-render-targets, render
targets mapped off the `_X` family (21->5, 22->6, 23->6, 25->9, 26->10,
27->10; Z modes 20/24 untouched), keyed on `usage` OR'd from descriptor
**+0x68 and +0xb0** (JBR leaves +0x68 unset on a render target).
**WHY the `_X` family is wrong is still NOT established** - 2nr blamed Navi
21's pipe count and 2ns measured that false: AddrLib's whole configuration is
`AMDCommonHwInfoRec+0xa4` = gbAddrConfig, which reads 0x242, agreeing with
the chip. 2na's XOR-linear bit matrix still points at the `_X` XOR term, just
not at the pipe count.

**`tools/rbgpu-app.sh`, `tools/mtllinear-app.sh` and the `(GPU)` wrappers are
GONE (2op)** - superseded by the kext path, which reaches what they never
could: platform binaries and sandboxed App Store apps. All 11 wrappers were
removed in 2on and both scripts are deleted. A leftover wrapper elsewhere is a
safe no-op (2om). 2ns's "there is NO global fix" was right about the place and
wrong about the reach, exactly as 2lg was.

**BOTH HALVES ARE NOW DEFAULT-ON AND BOTH ARE NEEDED (2oo).** The kernel-side
linear force is on unless `-rbgpunolinear`; the userspace patcher is on unless
`-rbgpumtlkoff`. Boot 131 removed the force and the desktop regressed - apps
correct at launch, wrong again after idle or a tab switch, Terminal broken
regardless - because native AppKit apps never reach the userspace chooser
(2oj). **No boot-arg or manual setup is required for a working desktop.**

**BOOT 132 CONFIRMED IT, FROM THE DEFAULTS (2op).** The user: *"everything is
flawless, not a single artifact or even an off pixel"*. Both halves ran from
their defaults, checked rather than assumed: `-rbgpumtlkfix` and
`-rbgpulinear` survive only inside log-message text and are **not read**
anywhere, so the args still sitting in boot-args enabled nothing. Only the
scope string came from an arg (`rbgpumtlkscope=*`, same value as the default).

Use `tools/mtlswizzle.c`, a **trampoline** hook, not `tools/mtllinear.c`'s
constant stub: the stub can only be blanket, and blanket LINEAR **crashes**
JetBrains, because JBR renders every Swing surface into a plain MTLTexture and
a linear surface is not a legal render target (Electron survives it only
because Skia's render targets are IOSurface-backed and never reach this
chooser). Policy `RBGPU_SWIZZLE=fix` is the one to use: LINEAR for
non-render-targets, and render targets mapped **off the `_X` family**
(22->6, 26->10, 27->10; Z modes untouched). `fix` keeps the surfaces **tiled**,
so it is a better answer than 2np's LINEAR, not merely a wider one.
**WHY it works is NOT established** - 2nr said the `_X` pipe-bank XOR is built
from Navi 21's pipe count, and 2ns measured that false: AddrLib's whole
configuration is `AMDCommonHwInfoRec+0xa4` = gbAddrConfig, and it reads 0x242,
agreeing with the chip exactly. 2na's exact XOR-linear bit matrix still points
at the `_X` XOR term, just not at the pipe count.

`usage` is at descriptor **+0x68 as requested and +0xb0 as resolved** - always
OR them. JBR leaves usage unset, so +0x68 reads 0 on a render target.

For a one-off test in a single process you can still inject
`tools/mtlswizzle.c` with `RBGPU_SWIZZLE=fix`; never a launchd environment
(rule 21). **Apple platform binaries strip `DYLD_INSERT_LIBRARIES` and cannot
be reached by injection at all** - which is why the kext path matters, and
2on proved Safari is both affected and fixed by it.

**Nor is TextEdit, or any native AppKit app** - 2oj/2ok: this defect is the
mode chosen for client `MTLTexture`s, which Electron and JBR hit by rendering
text into plain textures, while AppKit text goes through CALayer backing
stores that do not reach this chooser (2nj, Defect B). TextEdit renders
correctly at baseline, so it can only ever show "did not crash".
**2nj's second half - that they do not reach the kernel align manager
either - is now in doubt**: boot 131 broke Terminal with the force removed
and boot 132 fixed it with the force defaulted on, so something on that path
*is* reachable kernel-side. Unresolved, and recorded as the loose thread in
2op rather than smoothed over.

**texprobe is NOT an oracle for this defect** - it is clean at baseline (0/12,
every footprint 1.00x) because its round trip is self-consistent. Only a real
application tells you anything.

**Defect B IS CLOSED (2op)** - the CoreAnimation layer backing store that came
out rearranged in about half of allocations no longer reproduces.
`tools/screenprobe.sh` on boot 132: **32 of 32 captures CLEAN**, row and
column jumps 0, against **20/32 and 18/32 scrambled** on the identical
`refresh` x4 instrument before the fix - and every one of the 28 earlier runs
had scrambled captures, the three just before the fix being 100 % scrambled.
2on was right to warn against assuming the universal fix reached it; the
measurement answered the other way. The "wrong after idle or a tab switch"
symptom of boot 131 was this, so **no third defect need be hypothesised**.
Boot 132 also proved the detour a third independent way: screenprobe's own
in-process search finds the 32-byte prologue of
`amdMtl_HWL_DetermineSwizzleMode` **0x** in its own `__TEXT`, where 14
launches before the fix all read 1x. That verdict used to print
`INCONCLUSIVE` - it had only two branches and the kext's 5-byte detour leaves
*neither* pattern findable, so the success case read like a failure; fixed in
`tools/screenprobe.swift`.

**Note for any session run inside the Claude app:** it is itself launched with
`DYLD_INSERT_LIBRARIES` set, so every shell inherits it and `open -a` passes
it on. Launch anything with `env -u DYLD_INSERT_LIBRARIES` or it starts under
whatever patch this Claude is running - which once looked exactly like a
regression (2ns).

**ROOT CAUSE, FINAL, AND BOTH EXITS ARE CLOSED BY APPLE (2qk + 2ql).**
`CoreDisplay::Display::GetNextSurface` (unslid `0x7ff80408bf86`) is the one
function that produces the desktop's surface, and it has exactly **two** paths,
picked by two gates in its first twenty instructions:
`callq UseIOPresentment(); testb %al,%al; je LEGACY` then
`cmpb $0x1, [0x7ff843bac773]; jne LEGACY`, else fall through to
`DisplaySurfacePool`. **Both paths are dead here, for two unrelated Apple
decisions.**

**Path 1, legacy (`CGXAcquireDisplayDeviceSurface`): an id check no caller can
satisfy.** Boot 149's kernel log, found by grepping for the method's own os_log
function name: `IOAccelSurface::set_id_mode: New id (0x00000001) less than 256,
Assembly surfaces for display updates not supported` (plus `Surface mode
contains bad bits` x4 and `mode 0x4 must has kIOAccelSurfaceModeWindowedBit
bit`). The base method (`IOAcceleratorFamily2 +0x14e48cac`) returns
`kIOReturnBadArgument` when `mode & 0xFF8073C0`, when **`id <= 0xff`**
(`cmpl $0xff, %r14d` at `0x14e48e85`), or when the windowed bit is missing - and
**`MPAcquireSurfaceID()` can only ever return 0..255** (a 256-bit bitmap;
it returned **1**). 2ql closes *why the base implementation runs*: there are
three `set_id_mode`s and **neither override calls the base**, so the object's
class simply does not override it.
`IOGraphicsAccelerator2::newUserClient` switches on the `IOServiceOpen` type
(jump table at `0x14e51ab8`, decoded): **type 0** -> `this->vt[0xa30]()` +
`IOAccelSurface::init`, **type 0x20** -> `IOAccelSurfaceMTL`. There is no
`IOGraphicsAccelerator2::newSurface`, so slot `0xa30` is the vendor's, and
Apple's is `AMDRadeonX6000_AMDGraphicsAccelerator::newSurface()` (`0xbe3ed98`)
-> `new AMDRadeonX6000_AMDAccelSurface()`, which **does not override
`set_id_mode`**. So legacy -> AMD surface -> inherited base -> id >= 256
required -> unsatisfiable; Metal -> `IOAccelSurfaceMTL`, whose `set_id_mode`
has **no id check at all**. (It also proves the surface was created:
`newSurface` returning NULL gives `kIOReturnNoResources`, and we measured
`BadArgument`.)

**Path 2, IOPresentment (`DisplaySurfacePool` -> `CD_IOSurfaceCreate`, a real
IOSurface): closed by the framebuffer's CLASS NAME.**
`UseIOPresentment()` (`0x7ff804057b40`) is
`if (gUseIOP != 1) return 0; if (present == 0 || (supported & present) != present)
{ gUseIOP = 0; return 0; } return 1`, where `supported` is OR'd from five
`com.apple.CoreDisplay` CFPreferences booleans - `useIOP`, `useIOP_onIG` (0x02),
`useIOP_onAMD` (0x04), `useIOP_onNVIDIA` (0x08), `useIOP_onAPV` (0x10) - and
`present` comes from `__GetSystemGPUVendors_block_invoke`, which iterates
`IOServiceMatching("IOFramebuffer")` and classifies each service by
**`IOObjectCopyClass`** + four `CFStringFind`s: `Intel` 0x02, `AMD` 0x04,
`NVDA` 0x08, `AppleParavirt` 0x10, **no match 0x01**. **`supported` is built
only from bits 0x02/0x04/0x08/0x10, so bit 0x01 can NEVER be in it** - one
unclassifiable framebuffer fails the test for every value of every preference.
`make useiop` measures it non-root in a second: our only framebuffer is
**`IONDRVFramebuffer` / `.Display_boot` -> bit 0x01**, `present = 0x01`,
`supported = 0x1e`. All five prefs **default to 1** (the shared default byte
`0x7ff843bad260` comes from `0x7ff843bad738`, whose only writer, the block in
`CoreDisplay::VorpalBlade()` at `0x7ff8040598b7`, stores `$1`
**unconditionally**), so no `defaults write` is missing - the class name is.
Gate 2, `displaySurfaceUsesIOSurface`, is already 1 (it is one of the same
preference keys, and boot 149's os_state dump printed it).

**Do NOT force `UseIOPresentment` true.** It has **72 direct call sites**
(`MPRebuildDevice` x5, `setup_hardware`, `displayConnectChange` x3,
`displayDidWake` x2, `displayAliasActivate` x4, `DisplayPipe::Commit` x2,
`CGXMappedDisplayStart` x4, gamma, mirroring, shutdown...). It does not pick a
surface allocator, it selects the whole **IOPresentment** world - the
`IOPresentmentBindIOAccelSurfaceToResource` / `IOPresentmentSetPlaneState`
kernel API, which `IONDRVFramebuffer` does not implement. The
`-rbgpumtlkfix` machinery *could* patch it (its first instruction is a 6-byte
`mov` with no branch inside, so it is easier than the Metal detour) and it
would switch 72 call sites onto a stack that is not there.

**So the display half now rests on ONE decision, not another read: an
`IOFramebuffer` whose class name contains "AMD".** That is exactly the
subclass 2oz built and compiled, then ruled out on **kernel-collection
placement** (IOGraphicsFamily is in SystemKernelExtensions.kc, OpenCore
injects us into BootKernelExtensions.kc) - not on capability. **2oz's
"own the methods instead" is NOT equivalent after all:** routing
`IONDRVFramebuffer`'s methods changes what it reports, but nothing routed on an
instance changes `IOObjectCopyClass`, which is
`getMetaClass()->getClassName()` on the shared OSMetaClass. Either find a load
path that may depend on IOGraphicsFamily, or decide the display stays software.
**AND THE LEVER IS NOT WORTHLESS (2qm) - IOPresentment NEEDS NOTHING WE LACK
IN THE KERNEL.** Before accepting 2ql's "decide whether the display stays
software", the obvious check: would the IOPresentment path work if the gate
opened? `IOPresentment.framework` **is on this machine**, dlopens, and its own
strings name `IOFramebuffer`, `IOAccelIndex`, `IOAccelDisplayPipeTriage`,
`IOAccelDisplayPipeTransactionWait`, `DisplayPipeSupported` and
`ioPresentmentFindAGDCMatch` - **no IOMobileFramebuffer, nothing AMD-specific
in the kernel** - and `make ioptest` measured every one of them live on this
boot: `IOAccelIndex 0`, `IOAccelRevision 2`,
`IOAccelDisplayPipeCapabilities = {DisplayPipeSupported=1,
TransactionsSupported=1}` (on the **accelerator**, not the framebuffer), two
live `IOAccelDisplayPipeUserClient2`s and AGDC present. So what the class-name
test refuses appears to be already wired up. **The client test is
INCONCLUSIVE and the framework said why**: unprivileged, the call ABORTS after
logging `ioPresentmentFindAGDCMatch() failed - retCode: 0xe00002e2` and
`GPUWranglerOpenWithTimeout: failed to connect; status=0xe00002e2`, and
`0xe00002e2` is **`kIOReturnNotPermitted`** - an entitlement, not an answer
about the pipe. **Next step, one command, the user's:
`sudo ./build/out/ioptest`.** Pipe created -> intervention A below is a real
candidate fix. `Unable to create IOAccelDisplayPipe` -> the capability is
missing and 2ql's verdict stands. NotPermitted again -> still inconclusive and
only WindowServer itself can answer.

**INTERVENTION A IS BUILT, AND 2qo CORRECTED IT FIRST: the classifier has TWO
callers, so patch the CALL SITE, not the function.** `dsc.py callxref` finds
`0x7ff80412cc38` called from `__GetSystemGPUVendors_block_invoke` **and** from
`CoreDisplay::Framebuffer::Framebuffer` +0x8ec, which stores the result at
`fb+0x1f0` = `MPPriv+0x180` = Apple's own **`gpuVendor`** key (offset measured:
`InitializeMPPrivAccelerator` puts the embedded MPPriv at fb+0x70, and
`CreateMPPrivDict` pairs +0x180 with that CFString; the state dump prints
`gpuVendor = 1`). `MPGetGPUVendor` is **MP ops slot 0x200** and four sites read
it, so the function-level patch 2qm proposed would ALSO have flipped: the
pipe-info allocator (`access_display_pipe_info` does `testb $0x1c,%al; je
<allocate>` - vendor 4 makes it return **NULL** where it returns a real
0x3018-byte structure today), `UseIOPGamma`, and `__WSMachineIsAMD` -> HDR
support + new-style mirroring. **2qm's "the entire effect is
`gSystemGPUVendors = 0x4`" was an inference from an unchecked caller count. A
blast radius is a measurement, not a description.**
The narrow lever costs the same five bytes: the call at **`0x7ff80412ce97`** is
`e8 9c fd ff ff` and `movl $0x4, %eax` is `b8 04 00 00 00`, so overwriting it
leaves `orl %eax, gSystemGPUVendors` reading 4 and changes nothing else -
`gpuVendor` stays 1. One whole instruction, no branch targets a byte inside it,
nothing extra leaks, and the 27-byte anchored window occurs **once in the
main cache file's 0x25198000-byte executable mapping** (**corrected by 2r2**:
that is ONE of the cache's NINE files, not the whole cache - the claim used to
read "the cache's entire". The patch is not in doubt, because what it needed
was a unique match in the text mapped into WindowServer, `rbgpu_mtlk_place`
refuses on anything but exactly one match, and boot 150 verified the write
from that process's own memory). **`-rbgpuiop` + the
one-shot `rbgpu-iop`** (off by default, WindowServer by exact name, placed via
the verified `rbgpu_mtlk_place` at `_task_set_main_thread_qos` before the
process runs any instruction, because `GetSystemGPUVendors` is a
`dispatch_once`). The one-shot is the point: 2qn's rescue needed a boot-arg
edit from another OS, and **a one-shot means a black screen is recovered by a
plain power cycle.** NVRAM-hot keys, in reading order: `iop_oneshot` (0 armed,
1 /options unreachable, 2 not armed, 3 literal "0", **4 armed but not cleared -
the next boot is armed too**), `iop_saw_ws` (absent = WindowServer never execed,
nothing proven), `iop_match` (1 call, 2 already patched, 0 moved), and
**`iop_verify 1`, the only line that proves the write landed.** Writes no DCN
register. What it cannot pre-settle: whether IOPresentment fails *past* the
gate - `IONDRVFramebuffer` implements none of
`IOPresentmentBindIOAccelSurfaceToResource`/`SetPlaneState` - which 2qn proved
no client probe can answer. **It is still the user's call: a mistake is a black
screen at login.** 2ql's "do NOT force `UseIOPresentment`" stands untouched -
this patches none of its 72 call sites, only one input it reads, to a value
true of the hardware. Detail: JOURNAL **2qo**.
**BOOT 150: THE GATE OPENED, AND THE BLOCKER MOVED DOWN ONE LAYER (2qp).**
`iop_verify 1` (bytes read back from WindowServer's own memory), no black
screen, and WindowServer logs Apple's own **"System set to use IOP but no
display pipes are present"** - LEA'd immediately before a `movb $0x0,
gUseIOP`, so reaching it proves the supported-mask test passed. **2ql's
class-name test is beaten.** It then fails at
`Framebuffer::Framebuffer+3601`, emplacing into
`unordered_map<unsigned long long, CoreDisplay::DisplayPipe>`
(backtrace: `CGXMappedDisplayStart` -> `GPUWranglerRegisterEventBlock` ->
`GPUController::GPUController` block -> `GPU::GPU`), so zero pipes exist and
IOP is switched back off. Screen unchanged - legacy path, `CompositorSW` -
the expected partial result. **Keep `-rbgpuiop`**: without it this layer is
unreachable. Measured and worth carrying: **`gUseIOP` is a LATCH** (read
first, any failing path writes 0, plus three more direct zero-writes in
`CGXMappedDisplayStart`), so re-opening the vendor gate can never suffice
alone; `UseIOPresentment` triggers `GetSystemGPUVendors`' `dispatch_once`
itself, so ordering was never the risk; **no `com.apple.CoreDisplay`
preference domain exists**, so nothing was set to 0; and **`FB_Triage` is a
RED HERRING** - only `DumpIORegistryFrameBufferInfo`/`AcceleratorInfo` read
it, diagnostic dumps that run *because* the pipe failed, so publishing it
would change a debug dump and nothing else (the ioreg grep that "confirmed"
its absence had a control that also read 0 - rule 26). **FRONTIER, static:**
`CoreDisplay::FindDisplayPipeForService` (`0x7ff804056d04`) gates on byte
`0x7ff843bad2d8` then popcounts a table at `0x7ff843bad2b8`;
`DisplayPipeForService` (`0x7ff804056c52`) logs *"Unable to find display
pipe"*. Read **who populates that map**. Detail: JOURNAL **2qp**.
**2qq/2qr CLOSED THE WHOLE CHAIN, AND IT ENDS AT THE MISSING AMD
FRAMEBUFFER.** The pipe is `IOPresentmentCreateForRegistryID(0, registryID)`,
which returned NULL, and Apple named why in boot 150's log - found only after
dropping a `subsystem == "com.apple.CoreDisplay"` filter, because
IOPresentment logs under its own name: `(IOPresentment) Error: **Unable to get
AGDC information.  Return code: 0x10000003**`, 3 ms before the DisplayPipe
backtrace, same process and thread. It wants **`AGDCVendorInfo`** and
**`AGDCGPUCapability`**; live `debug-gpus` has **`gpuc` = 0** and **`agdp` =
0** with our fb and accelerator correctly paired - which **corrects 2qn** (the
association exists, its contents do not; a field being present is not a field
being populated). Those are filled by the **vendor AGDC plugin**, i.e.
`AMDSupport`/`AMDFramebuffer`, which never instantiates here (2oq). **So this
route terminates at the same root cause and 2ql's structural verdict is
confirmed twice from two directions - the next thing to supply is the AMD
framebuffer stack, not another gate.**
**2qz CORRECTS THAT LAST SENTENCE, AND IT IS THE BIGGEST OPENING IN A WHILE.**
`AppleGPUWrangler`'s `__text` is only **`0x58aa` (6376 instructions)**, so it
was read whole, statically, no boot (`kc_stubs.py`: 1678 of 1678 stubs
resolved). `gpuc` is `gpuStruct+0x10`; `agdp` is the registry ID of an
`IORegistryEntry *` at `gpuStruct+0x68` - 2qr's decode confirmed. **`+0x68`
has EXACTLY ONE writer in the whole kext**, at `0x13df4d34` inside the
function starting `0x13df4c96`: it `safeMetaCast`s an incoming object, looks
up that object's per-GPU struct, tests its class against **one of two
globals**, and only then stores + retains it, guarded by `cmpq $0x0` so it is
**set once**. The wrangler imports exactly **three** `gMetaClass` symbols -
`AppleGraphicsDeviceControl`, `AppleGraphicsDeviceControlClient`, `OSObject` -
so that test can only be for AGDC. **And the function has ZERO direct callers
inside the wrangler**, so it is an entry point: **AGDC registers ITSELF with
the wrangler, per GPU.** Nothing on that path touches `IOFramebuffer`, a
vtable or a subclass. **Measured live, "the vendor AGDC plugin never
instantiates here" is too strong**: `com.apple.AppleGraphicsDeviceControl`
(8.1.10) is **LOADED with 4 dependents**, `AGDCPluginDisplayMetrics` is loaded,
and `AppleGraphicsDeviceControlPlugin` is **registered, matched and active** -
but it attaches to `AppleGPUWrangler` on `IOResources`, i.e. **machine-level,
not per-GPU**, and its class is neither name the writer tests. What is missing
is a **per-GPU `AppleGraphicsDeviceControl` registered against our GPU**.
**NOT established:** which of the two globals is which, and whether
`0x18(%r15)` is a metaclass pointer - the compare is a plain `cmpq`, so if it
is, it is an **exact-class** test, which would matter. Noted, unchased: this
machine presents as **`MacBookPro16,2`** and AGDP/AGDC is often board-id
gated. Detail: JOURNAL **2qz**.
  **2r0 SETTLES THE QUESTION 2qz FLAGGED, AND THE ANSWER IS THE GOOD ONE.**
  Nothing calls `0x13df4c96` because the pairing is a **matching
  notification**: the wrangler installs **two**, both on
  `serviceMatching("IOService")` - the widest possible match - with the **same
  handler `0x13df3194`** and the discriminating **metaclass carried as the
  refCon** (`*[0x13df60a8]`, `*[0x13df60b8]`). The handler admits a service via
  **five `safeMetaCast`/`metaCast` tests** (four metaclass globals plus a
  by-name `metaCast("IOThunderboltSwitch")`), and **`safeMetaCast` walks the
  superclass chain - so A SUBCLASS QUALIFIES.** 2qz's fear of an exact-class
  `cmpq` was misplaced: that compare is in the *store* path and only says which
  of the two notifications fired. The wrangler's whole metaclass import list is
  `AppleGraphicsDeviceControl`, `AppleGraphicsDeviceControlClient`, `OSObject`,
  and it imports **both AGDC constructors**, so it defines its own AGDC
  subclasses - the live `AppleGraphicsDeviceControlPlugin` is one. **There is
  NO `IOFramebuffer` or `IOAccelerator` metaclass import at all**, so `agdp` is
  unrelated to an IOFramebuffer subclass. AGDC itself is **12 KB**
  (`__text 0x13dfac54 + 0x30cb`) and makes **zero calls into the wrangler** -
  the coupling is one-way, AGDC publishes and the wrangler pairs; but right
  after the store the wrangler calls **into** AGDC at `0x13dfd37c` with our
  object. **So the route to `agdp` is: publish an IOService that
  `safeMetaCast`s to `AppleGraphicsDeviceControl`, and the wrangler pairs it
  itself** - and 2qy already measured that a class can be registered at
  runtime from the boot KC with a system-KC superclass. **Still unestablished,
  do not gloss:** which of the four globals is which class (only three
  `gMetaClass` symbols for four slots); **`gpuc` (+0x10) has NO identified
  writer** - 14 stores to `+0x10` and none adjacent to any of the six call
  sites of the struct finder `0x13df2e74`, while IOPresentment wants **both**
  `AGDCVendorInfo` and `AGDCGPUCapability`; and a runtime AGDC subclass would
  have **its methods called by Apple's code**, so it must behave like an AGDC,
  which is the real unmeasured cost. Detail: JOURNAL **2r0**.
  **2r1 CORRECTS THE PREMISE UNDER ALL OF THIS, INCLUDING 2qr.** `gpuc`
  (`gpuStruct+0x10`) has **NO writer in AppleGPUWrangler** - four candidate
  classes eliminated: all 14 plain `0x10(%rX)` stores (the two plausible ones
  are provably other structs - `0x13df450e` builds an event-record node whose
  `+0x10` is `mach_absolute_time()`, `0x13df581e` is a trivial
  `OSObject::init` wrapper); **zero** wide/SSE stores exist in the kext
  (control: 47 `movzwl`); and **all eight** `filtered_doDeviceAttribute` calls
  write **stack** buffers or a caller's buffer, never the struct. The `0x301`
  call beside the `agdp` store, which looked like the answer, is a **SET, not a
  get** - `movzwl (%r10),%eax ; movw %ax,0xb0(%rdi)`, i.e. AGDC reads 2 bytes
  *from* `&gpuStruct[0xc]` (which is `id`) into **its own** object, so the
  wrangler is pushing the GPU id INTO AGDC. **And the bigger finding: `gpuc`
  and `agdp` are the wrangler's DEBUG-DICTIONARY keys, not what IOPresentment
  asks for.** `AGDCVendorInfo` / `AGDCGPUCapability` / `AGDCPortCapability`
  appear **0 times in both AppleGPUWrangler and AppleGraphicsDeviceControl**
  (controls: `agdp` 1x in the wrangler, `AppleGraphicsDeviceControl` 7x in
  AGDC); 2qm read those names from **IOPresentment.framework's** string table.
  2qr joined "IOPresentment wants AGDCVendorInfo/AGDCGPUCapability" to
  "`debug-gpus` has `gpuc=0, agdp=0`" **on the resemblance of the names**, and
  that join was never measured. **2r0's mechanism still stands** (the two
  notifications, the `safeMetaCast` admission, no `IOFramebuffer` import) -
  **its motivation does not.** Do NOT build an AGDC subclass on the belief that
  setting `agdp` unblocks IOPresentment. **Next read, and it is a different
  file: IOPresentment's own path to `0x10000003`** (the measured log line from
  boot 150), via `tools/re/dsc.py` / `dscinv.sh`, anchoring on that constant.
  Detail: JOURNAL **2r1**.
  **2r5 CLOSES THE DISPLAY CHAIN AND TURNS IT INTO A DECISION.** The function
  at 2r4's addresses is **`ioPresentmentCreateAssociationMap`**
  (`0x7ff812e8a730`); `ioPresentmentFindAGDCMatch` survives only as the
  **os_log function string** (`0x7ff812ea14f7`) of a helper inlined into it.
  `%r13d` is `IOIteratorNext` over
  `IOServiceGetMatchingServices(IOServiceMatching("AppleGraphicsDeviceControl"))`
  **minus the entry named `AppleGPUWrangler`** - an exact 17-byte compare done
  as three xors against `"AppleGPU"`, `"Wrangler"` and the NUL at `[16]`
  (`0x7ff812e8ad6c`-`0x7ff812e8ada1`). **`make agdcinfo` measures it live,
  non-root, in a second: exactly ONE service matches and it IS the wrangler, so
  zero candidates survive and the association map is EMPTY.** So
  `AppleGPUWrangler` **is** an `AppleGraphicsDeviceControl` subclass and
  `AppleGraphicsDeviceControlPlugin` is **not** (live, and it does not match).
  Instrument control, rule 26: `ioreg -c IOUserClient -r -d1` matches **431**
  services so `-c` does follow the superclass chain, while `-d1` *without*
  `-r` prints only the root and proves nothing.
  **`0x10000003` is `MACH_SEND_INVALID_DEST`** (SDK `mach/message.h:807`), not
  a user-client status as 2r4 supposed: the second inlined query,
  `ioPresentmentPopulateIOFramebufferEntries`, looks our framebuffer's GPU up
  in that map (stride 0x30), finds nothing, keeps service id **0**
  (`0x7ff812e8b3be`) and opens *that* - and `agdcinfo`'s control measures
  `IOServiceOpen(MACH_PORT_NULL, ..., 2, ...)` returning exactly `0x10000003`.
  **The selectors name themselves**, in os_log formats three bytes from the
  failing branch: **selector 1 = `kAGDCVendorInfo`** (`0x7ff812ea1512`; 0x2c
  bytes, `+0x28` must be 1 or 2) and **selector 0x980 =
  `kAGDCGPUCapability`** (`0x7ff812ea1625`; 0xdc bytes, non-zero count at
  `+0x30`). 2qr's motivation is restored; 2r1's measurement (0 hits in either
  kext) stands - these are IOPresentment's labels, not the kexts'.
  **KERNEL SIDE: there is NO type-2 user client to implement.**
  `AppleGraphicsDeviceControl::newUserClient` (`0x13dfd45e`) has **no switch on
  the type at all**, unlike `IOGraphicsAccelerator2`'s (2ql): it is IOService's
  canonical body - `allocClassWithName(this's `IOUserClientClass` property,
  measured as `AppleGPUWranglerClient` on the wrangler), cast to
  `AppleGraphicsDeviceControlClient`, then `initWithTask` / `authorizeTask` /
  `attach` / `start` - with the type passed only to the first two.
  `authorizeTask`'s failure is `0xE00002E2`, exactly what `agdcinfo` measures
  unprivileged. `AppleGraphicsDeviceControlClient::externalMethod`
  (`0x13dfae20`) forwards **every** selector to `UserKernelTransfer` ->
  `filtered_doDeviceAttribute(api=3, attribute=selector, ...)`, so **the
  selectors are AGDC device attributes** (`agdc_filtered_api_t`: 0 get, 1 set,
  2 do, 3 UserKernelTransfer). `0x980` is handled explicitly (require
  `*outLen == 0xdc`, call **virtual slot `0x858`**); **attribute 1 is NOT** -
  the `attr<=0x7f` group handles only 3, 0x10 and 0x20, so it falls to the
  plugin path: a 16-slot table at `0x13e00940` indexed `(attr>>2)&0x3c0`, the
  slot's `+0x18` >= attr, `agdc->0xa0` **and** `->0xa8` non-NULL, then
  `AGDCPlugin::doPluginAttribute`. Side confirmation: attribute **0x301** is
  `movzwl (%r10),%eax; movw %ax,0xb0(%rdi)`, i.e. **2r1's reading of the
  wrangler's `0x301` call as a SET, confirmed from AGDC's own side**.
  **THE COST, stated before anyone builds:**
  `__ZTV26AppleGraphicsDeviceControl` spans `0x13dff478`..`0x13dffd30` =
  **0x8b8 = 279 pointers**, so ~277 instance methods **plus** the metaclass
  vtable 2qy showed is also needed (the base's `alloc()` is pure virtual), in a
  class whose superclass lives in another kernel collection - i.e. through
  2qy's runtime-metaclass route - with Apple's code calling our methods and
  **no fail-safe**; and the post-gate question is still unanswered behind it,
  because `IONDRVFramebuffer` implements neither
  `IOPresentmentBindIOAccelSurfaceToResource` nor `SetPlaneState`, which 2qn
  proved no client probe can decide. **So the display half is now a DECISION,
  not another read.** The one cheap thing left is the user's to run:
  **`sudo ./build/out/agdcinfo`**, which says whether an AGDC user client here
  answers attribute 1 at all (unprivileged it returns
  `kIOReturnNotPermitted`, which the tool pre-states as INCONCLUSIVE).
  Detail: JOURNAL **2r5**. Killed suspects: `FB_Triage`
(diagnostic dumps only) and the third-party NV kexts in `/Library/Extensions`
(`kextstat`, control 67 - **none loaded**). The `IOPresentment` boot-arg token
is logging-only and real but **no longer worth a boot**. Keep `-rbgpuiop`:
correct, verified, inert without its one-shot. Detail: JOURNAL **2qq**,
**2qr**.
**B, kernel, global** - rename `IONDRVFramebuffer`'s class by
writing an `OSSymbol *` to `gMetaClass + 0x18` (offset measured from
`is_io_object_get_class`, which does `metaclass->0x18` then `vt[0x148]`
`getCStringNoCopy`, and from `OSMetaClass`'s ctor, which has exactly one store
there). **B is strictly worse than A for the same effect** - it changes every
`IOObjectCopyClass`, ioreg and panic-log consumer system-wide to fix one string
comparison in one userspace function. Either way it is a kext change on the
process that owns the display, so it is the user's decision and a mistake is a
black screen; rescue `-rbgpuoff`. Writes no DCN register.

Also: `isAcceleratorBacked` (`-rbgpufbaccel`) does have consumers - the
mirror-loop walk at `GetNextSurface+0x6e` reads bit 22 of `dev->0x64` to pick
the pool's buffer count (2 vs 3), and
`all_devices_in_hw_mirror_loop_are_accelerator_backed` uses it to pick mode
flag `0x8800` over `0x8400` - but every one of those is downstream of a gate
that fails, so **keep the arg OUT**. 2ql also retires the 2qk-correction's open
question about `DisplaySurfacePool::GetNextSurface`'s reuse path: the pool is
never reached, so it was never the question.

**(2qh, superseded in part by 2qk) the display's
`CoreDisplay::DisplaySurface` has NO IOSurface, and SkyLight swaps the
compositor because of it.** `WS::Displays::Update::Update` correctly sets
`Update->0x1c0 = display->compositor()` - a **CompositorMetal** - and then
`CDDisplay::begin_update` (`0x7ff807d6c488`) creates a `CDSurface`, calls
`CDSurface::prepare_for_update`, and that does
`io = CoreDisplay_DisplaySurface_GetIOSurface(s); if (io == NULL)
cdsurf->0x40 = 1` (`0x7ff807d6f42d`). `begin_update` then tests
`cdsurf->0x40 == 1` and **overwrites `Update->0x1c0` with the software
compositor** read straight from `*[0x7ff843c9eca8]` (`0x7ff807d6c5c8`).
`UpdateDisplays` loads that from `Update+0x1c0` (= `-0x350(%rbp)`) and hands it
to `render_update`. **So there is no flag that chooses software** - SkyLight
chooses Metal and is then overridden, which is why every accelerator-side
reading (mode 0, capability flag 1, `priv.accelerator = 0x14b23`, working
`new_compositor`) was healthy and irrelevant. The getter is
`if (s==NULL) return NULL; return *(s)`, and `prepare_for_update` tests
`s == NULL` on a *separate* branch, so **a DisplaySurface exists with a NULL
IOSurface**; `CD_IOSurfaceCreate` logs
`'IOSurfaceCreate failed for dictionary'` and that appears **0 times** this
boot (control: 113 CoreDisplay lines), so creation was never attempted.
**This is 2pd, confirmed at the decision point, and it joins 2q9**:
`numDisplayBuffers = 0` is the popcount of `IOAccelSurfaceMTL->0x13c`'s low
req-bits, 0 because no drawable is attached - a display with no attached buffer
has no IOSurface to hand out. Same fact, two layers; and it is why 2pr's bigger
`getVRAMRange` changed nothing (room was never the problem, a **surface** was).
Next static read: `CoreDisplay::Display::GetNextSurface` reaches
`DisplaySurfacePool::GetNextSurface` only at +1270 of 0x65c bytes, so find the
earlier return.

**THE DISPLAY HALF IS HALF-FIXED, AND THE REAL DEFECT IS NAMED (2pd/2pf,
boot 138).** The pacing is **fixed**: CoreVideo nominal went **30.000 ->
129.267 Hz** and the measured callback rate 29.99 -> **134.10 Hz**, a 4.3x
improvement the user felt before reading a number. The string is **fixed**:
System Profiler reads `AMD Radeon 680M` / `VRAM (Total): 4 GB` - *published by
us, not detected*. **But the stalls are NOT fixed and the cause is now
measured**: `sample WindowServer` shows `CompositorSW::CompositeLayersToDestination`
at **48 % of the main thread**, rasterising the whole layer tree on the CPU via
`CA::OGL::SW::` in **RGBA half-float**. Chain: no DCN 3.1.2 -> the display is
`IONDRVFramebuffer`, whose destination is a raw CPU-mapped range, **not an
IOSurface and not a Metal texture** -> SkyLight's only two destination
constructors take exactly those, so `CompositorMetal` (which IS activated, per
the log) has nothing to render the display into -> software compositing ->
~9 fps, multi-second blurs, 30 s wallpaper changes.
**PAGE FLIPPING IS NOT THE FIX, and this was MEASURED, not argued (2pf):**
with `-rbgpuflipprobe` armed and routed, `AMDRadeonX6000_AMDHWDisplay::getDisplayPipeTransactionFlip`
was **never called once** in a whole boot. The flip is downstream of a decision
already lost; a `HUBP0_PRIMARY_SURFACE_ADDRESS` write would have fixed nothing
and risked a black panel. **Do not propose it again without new evidence.** The
real lever is making the display's destination a GPU surface, i.e. an
accelerated framebuffer owning the display - which needs the DCN 3.1.2 code
macOS lacks, written from Linux's `dcn31` reference. Name it as a large body of
work, not one more hook. Still cheap and unspent: phase-lock the vblank by
polling `OTG0_FRAME_COUNT` (read-only; the timer's 7.736 ms vs the panel's
6.952 ms is ~0.78 ms of workloop overhead, which is why nominal reads 129 and
not 143.833).

**The display half has never started**, and 2oq surveyed why, statically.
Apple's controller personality matches Navi 21 PCI IDs and ours is
`0x16811002`; spoofing `device-id` kills the accelerator. But the personality
route is **pointless**: **macOS contains no DCN 3.1.2 code at all** - Apple's
DAL compiles `dcn10/20/21/30/302`, and `dcn31`/`dcn312`/`vba_31`/`yellow_carp`
are 0 hits in every AMD kext against a control of 58 for `dcn30`. This chip is
DCN 3.1.2 / DMU 3.1.3.

**A SUBCLASSED `IOFramebuffer` IS NOT POSSIBLE FROM THIS KEXT (2oz).** It was
built and it compiled: `IOFramebuffer.h` ships in Kernel.framework (no Xcode,
no KDK), only four pure virtuals, and a subclass resolved **356 of 356**
imports through `com.apple.iokit.IOGraphicsFamily` - Apple does the same in
`AMDFramebuffer`. Displacing `.Display_boot` would also have worked: same
provider (IOPCIDevice), same category (`IOFramebuffer`), its post-probe score
is **20000**, and xnu's `probeCandidates` was *read* - `probe()` returning NULL
does `continue`, and the start loop is `// start the best (until success) of
each category` - so **both** failure modes fall back. What kills it is the
**kernel collection**: IOGraphicsFamily is in SystemKernelExtensions.kc,
IOPCIFamily in BootKernelExtensions.kc, and OpenCore injects us into the
**boot** KC. `kextstat` proves it on this machine - **WhateverGreen depends on
Lilu and IOPCIFamily, NOT on IOGraphicsFamily.** Shipping that dependency
would have failed the plugin's load and taken the accelerator with it.
`/Library/Extensions` is no escape (Auxiliary KC, loads too late, and
forbidden). **Own the methods instead**: every framebuffer method is defined in
IONDRVSupport and therefore routable - mode list, timings, aperture, cursor,
gamma, attributes, the interrupt trio - which is the same technique that fixed
the accelerator, and functionally the same thing.

**The desktop runs on `IONDRVFramebuffer` with `IOFB0Hz = Yes` and one fixed
mode.** No vblank is why animations are timer-paced; one mode is why nothing
switches; the 7 MB is the same fact. **But the firmware already brought DCN
3.1.2 up** - `OTG0_CONTROL = 0x00011201` (master enable), OTG1-3 off, DMCUB
firmware 4.0.15.47 running - so the route is to **adopt the running pipe**,
not to bring DCN up: read OTG0's timing, publish our own `IOFramebuffer`
(IOGraphicsFamily is a public KPI), then take OTG0's vblank and flip
`HUBP0_PRIMARY_SURFACE_ADDRESS`. The last two **write DCN registers, which is
the user's decision** - the probe allows no writes but `MM_INDEX`, and a wrong
OTG/HUBP write blanks the panel (rule 12). Only the vblank step fixes
animations. `docs/HANDOFF.md` §4 and §10 have the detail.

Details, evidence and next steps: `docs/HANDOFF.md`.

## Layout

- `src/common/` — freestanding C shared by the kext and host tools: the IP
  discovery parser (`rb_discovery.c`), the read-only probe (`rb_probe.c`), the
  register list (`rb_regs.h`, every offset cites its Linux header), the SMU
  queries (`rb_smu.c`, `rb_smu13.c`). Message groups (`RB_SMU13_ALLOW_*`)
  start at queries only; allowing any other group on hardware is the user's
  decision. Tested in `tools/hosttest/smu13_test.c` against a simulated SMU
  whose tables are Linux's own structs.
- `src/kext/` — `RembrandtProbe`, an IOService in its own IOMatchCategory
  beside the boot framebuffer driver. Runs the probe once in `start()`,
  publishes `RBProbe` properties, unmaps everything.
- `src/plugin/` — the Lilu plugin (`kern_start.cpp` holds the hooks and
  probes, `RembrandtGPUPeer.cpp` the IOService peer). Boots from the EFI.
- `tools/hosttest/` — the probe against a simulated chip + parser fuzzing,
  ASan/UBSan. `make kext` runs it first; never load a kext whose hosttest
  fails. Add a case whenever `src/common` changes, and check a new test fails
  when the code it guards is broken.
- `docs/linux-ref/` — unmodified Linux files the code follows
  (`UPSTREAM.md` records each commit; fetch from torvalds/linux).
- `tools/re/` — static analysis of Apple's AMD kexts: `kc_extract.py` (kext
  out of a kernel collection as a standalone Mach-O), `machoread.py` (chained
  pointers, strings, symbols), `dumptable.py`, `kc_stubs.py` (kernel calls
  through branch stubs → names; `annotate` for objdump output), `vtslots.py`
  (IOService-family vtable offsets → methods), `strxref.py` (functions using a
  C string), `switches.py` (jump tables: case → handler), `ptrsym.py` (names
  every `RBGPULog` pointer inside a loaded AMD kext; run by
  `plugin_check.sh`). Output to `build/re/`. Findings:
  `docs/APPLE-STACK.md`.

## Workflow

- Build: `make kext tools` (CommandLineTools only, no Xcode); `make plugin`
  for the Lilu plugin — **`make kext tools` does NOT rebuild it**. The kext
  step resolves every import with `kmutil libraries`.
- **Always check the UUID moved** after a build
  (`dwarfdump --uuid build/out/...`). Two boots were lost to a stale build.
- The probe's hardware rules: no writes except `MM_INDEX`/`MM_INDEX_HI`
  (enforced in `rb_probe.c`, asserted by the hosttest), never enable PCI
  decoding or bus mastering, never map BAR0's first 16 MB (the boot
  framebuffer's), read only inside the carve-out. Levels 2 (DCN) and 3 (+ GC)
  are opt-in: `RBProbeLevel` in `src/kext/Info.plist`, or `rbprobe_level=N`.
- The SMU queries' hardware rules (`rb_smu.c`; opt-in `make kext SMU=1`,
  plist `RBSMUQuery`, boot-arg `rbprobe_smu=1`): writes only to MP1
  C2PMSG_66/82/90 of an MP1 13.x, only allowlisted query messages
  (GetSmuVersion, GetDriverIfVersion, GetEnabledSmuFeatures), nothing sent
  unless the response register reads 1, stop at the first non-OK answer.
  Linux's protocol (`smu_msg_v1_send_msg`).
- Register gates follow Linux's **version grouping**, not the exact version:
  this chip reports MMHUB 2.4.0 (Linux: `mmhub_v2_3` offsets) and DMU 3.1.3
  (Linux: `DCN_VERSION_3_1`, `dcn_3_1_2` offsets). Check the grouping before
  adding a gate.
- Results land in `logs/<timestamp>/`.
  `tests/data/rembrandt-680m-tufa15.bin` is this machine's real IP discovery
  table (2026-10-03); the hosttest parses it and runs the full probe on it.

## Hard rules

- **Always ask the user before loading or unloading a kext.** A bad load can
  hang or panic the machine and ends the session.
- The user runs `sudo tools/load.sh` (2oq: takes an optional probe level,
  `sudo tools/load.sh 2` for the display engine - applied to the staged copy
  only, so the committed default stays 1), `sudo tools/unload.sh`,
  `sudo tools/efi.sh …` and `tools/plugin_check.sh`.
  **Claude never runs these and never asks for the password.**
- **Never install into the EFI or `/Library/Extensions`.**
- Before any load, **commit** so a hang loses nothing. The user allows Claude
  to commit at any time without asking (2026-10-04); **pushing still needs
  asking**.
- **Deliberate diagnostic panics are off the table** (user's decision,
  2026-10-12).
- **Adding an SMU message group is the user's decision** — say what it changes
  on the chip.
- **NootedRed and NootRX are under the ChefKiss Open Knowledge License v1,
  whose §2 and §3 forbid AI reading, summarizing, analyzing or deriving output
  from the work. Do not read or analyze them.** Findings from those projects
  must come from the user in their own words.
- `efi.sh` saves `config.plist` to `logs/efi-backups/` first; it edits via
  `tools/efi_config.py` (tested by `tools/efi_config_test.py`).
  `efi.sh sysreport-on` + reboot + `efi.sh vbios` pulls the 680M VBIOS out of
  the ACPI VFCT table (`tools/vfct.py`) into `firmware/` (gitignored).

## Rules that cost boots to learn

Each was paid for on hardware. `docs/JOURNAL.md` has the evidence under the
tag in brackets; `docs/HANDOFF.md` §7 states them with their reasoning.

**Routing Apple's code**

1. Never route a name unless `nm -m build/re/<kext>` shows it **defined**
   there. `solveSymbol` does not skip imports — it returns the bare slide and
   `routeMultiple` writes a jump there. [2c]
2. Disassemble a symbol's first 16 bytes and **refuse if a rel8/rel32 branch
   begins within the first 5**. Lilu copies displaced instructions
   byte-for-byte into a trampoline, so a short branch keeps its original
   displacement and lands in Lilu's own buffer. [2gb]
3. **Grep the mangled symbol across the whole plugin before adding a route.**
   A second `RouteRequest` for an already-routed function **panics the machine
   outright**. A new transform goes **inside the existing hook**. [2kn]
4. **`RouteRequest::from` is NOT written back** by Lilu 1.7.3, although
   `kern_patcher.hpp` documents it as such. Call `solveSymbol` yourself. A doc
   comment is not a measurement. [2kp]
5. **Cross-check every hand-written mangled name against `nm`.** Three length
   prefixes were wrong in one change; each would have made `solveSymbol`
   return 0 and silently unscope the fix. [2kr]
6. **When a fix moves upstream, REMOVE the downstream patch it replaces, in
   the same change.** "Redundant" and "inert" are different things — 2ku and
   2ki composed and the halt came back. [2kv]
7. vtable dumps: **call offset = dump offset − 0x10**. A *non-virtual* call
   through a vtable **symbol** uses dump offsets directly. [2gp]

**Reading a boot**

8. **Read `rbgpu-boot` FIRST.** If it has not increased since the last
   capture, the boot was never armed and *nothing* in NVRAM is this boot's.
   `rbgpu-diag` carries no nonce, so "armed=1 reads live" certifies a **stale**
   capture. [2iz]
9. **`rbgpu-arm` is one-shot** — the peer consumes and deletes it. Every armed
   boot needs it set again; an install that only replaces the kext and edits
   boot-args does **not** arm. [2iz]
10. **Run `sudo tools/plugin_check.sh` in the armed boot, over SSH, before
    rebooting.** It saves the capture to `logs/<timestamp>-plugin/` on disk,
    which survives a power cycle; live NVRAM afterwards does not. Three
    experiments were spent blaming the code for this. **Look in
    `logs/<timestamp>-plugin/` before reading live NVRAM.** [2jg, 2gq, 2gw]
11. **A note is evidence only when its `b=` nonce is new.** [2gd]
12. **A stale note is no evidence about where a boot got to** — not weak
    evidence of an early stop. A user rebooting a black screen looks exactly
    like a hang. [2gw]
13. **A return code is not a result until the code that sets it has been
    read.** A named constant tells you what the NAME means, not what the
    function meant by it. Request 0x1B's *success* path returns
    `kIOReturnUnsupported`. [2ha]
14. **Absence is not a result.** A silent `return` inside an instrument, a
    full diag table and a missing NVRAM write all look identical to "the code
    never got there". Every path out of a probe must say something.
    **And check `diag_used` against `diag_max` BEFORE concluding anything from
    an absent key (2qw)** - equal means the table filled and absence carries no
    information at all. That has now happened three times (2db at 96, 2ds at
    192, 2qw at 448, which dropped `mcp_verdict` and `mcp_say` - the keys
    holding a probe's whole conclusion - while every component survived).
    Raising the cap failed twice, so the table now seeds `diag_max` and
    `diag_used` into slots 0 and 1 before any real key: the two numbers that
    say whether the data is trustworthy are the only two that cannot be
    evicted by it filling up. The `rbgpu_log` warning was not enough, because
    it lands only in `plugin_check.sh`'s capture and that boot had none - **an
    instrument's own capacity is part of the instrument.**
    [2eh, 2ek, 2fb, 2db, 2ds, 2qw]
15. Before interpreting anything, check the loaded UUID against `build/out`,
    whether `rbgpu-arm` was left set, and which boot-args are actually in
    `config.plist`. [2cu, 2cv]

**Instrumenting**

16. **The instrument is a suspect.** It has cost a wedge (2fo), a crawl
    mistaken for a hang (2ez) and two panics (2fz/2ga, 2gg). When a
    measurement and a suspect are both new, **remove the suspect** rather than
    reasoning about it.
17. **Never substitute a value and then narrate it as a hardware result.**
    [2cy, which cost fifteen milestones]
18. **Do not build a table from n=1 rows.** [2ev, 2fs]
19. **A NULL check is not a pointer check.** Validate derived pointers before
    dereferencing — but **with the right yardstick, and `0xffffff8000000000`
    is only sometimes it (2qu).** That constant is the **kernel's** base and is
    correct for a **kalloc'd kernel object** (the `fb`, `info` and `res` guards
    in 2pa/2ou/2ll, working for months). It is **wrong for a symbol in a
    kext**: `kextstat` here lists 180 kext addresses, **50 below** the kernel
    base and 130 above — IOGraphicsFamily is at `0xffffff7f95c94000` in the
    **system** KC, our plugin at `0xffffff8004043000` in the **boot** KC.
    Boot 151 was spent on a read-only probe that solved both of its symbols
    and then threw them away as "not a kernel VA". For a kext symbol the floor
    is **`0xffffff7f80000000`** (below all 180, measured). The second lesson
    is the one that generalises: **a refusal path must publish the value it
    refused.** This one `continue`d and reported the address as 0, so the
    capture could not say what had been rejected — rule 14 applies to an
    instrument's failure paths, not only its returns. **2qu also claimed the
    kext's own `[address, address+size)` would reject valid symbols, and boot
    152 measured that false**: it used `kextstat`'s size column (`0x2efea`,
    `__TEXT` only) in place of the `size` Lilu actually passes, which is
    `0x42000` and spans `__TEXT + __DATA + __DATA_CONST`, so the range check
    would have worked. Both guards are fine; the floor is just simpler. The
    reusable form of *that* mistake: **one number was read and another
    asserted — publish the parameter instead of assuming it.** [2gh, 2qu, 2qv]
20. **A registration that reports success is not evidence that a patch
    applied.** A Lilu `BinaryModPatch` with `section == 0`
    (`SectionDisabled`) is skipped and its file never read, while
    `onProcLoad` still returns `NoError` - so the hook logged `ARMED` and
    patched nothing. Log the enabling field itself and refuse when it is
    disabled. [2lq]
21. **Never put `DYLD_INSERT_LIBRARIES` in a launchd environment, and test a
    recovery procedure in the situation it exists for.** `launchctl setenv` is
    system-wide, not per-app, and dyld **kills** any process that cannot load
    an inserted dylib - including every sandboxed app, which cannot read one
    from a home directory. Every newly launched app died; the machine needed a
    reboot. The documented undo, `launchctl unsetenv` over SSH, addresses a
    different domain than the Aqua session and did nothing - the working form
    is `sudo launchctl asuser $(id -u) launchctl unsetenv DYLD_INSERT_LIBRARIES`.
    Inject per process, where the blast radius is one process started on
    purpose. [2mq]
22. **Lilu's user patcher DOES NOT RUN on this OS at all.** Lilu's
    `Configuration::getBootArguments` sets
    `isUserDisabled = ... || getKernelVersion() >= KernelVersion::BigSur`,
    and that term is unconditionally true on Sequoia (24 >= 20). It is a
    `||`, so **no boot-arg re-enables it**. `UserPatcher::init` then returns
    `true` without registering its kauth listener and `registerPatches`
    returns `true` without reading a file, parsing the map or hooking
    anything - every layer reports success and nothing exists. That, and
    nothing else, is why 2lm, 2lp, 2nb, 2nm, 2oa and 2od were all inert
    while `onProcLoad` returned `NoError`. **Do not write another Lilu
    `BinaryModInfo` for this machine.** All of that code was DELETED in 2op
    (~590 lines), so there is no longer a working example in the tree to copy
    by accident. This rule has now been wrong twice
    and under-specified once: the earlier readings (a mod can only patch an
    existing file; addresses come from the `.map` but bytes from a file) are
    accurate descriptions of code that never executes here. The reusable
    lesson: **check that a subsystem is ENABLED on this OS before reading
    how it behaves** - one `grep isUserDisabled` would have saved three
    milestones and a panicked boot. What survives: **verify a patch from the
    target process's own memory** (`tools/cavecheck`), and note that a
    `MatchAny "/"` proc callback firing ZERO times is the cheapest proof the
    listener does not exist. The *mechanism* Lilu would have used is still
    open to us directly - see 2of. [2nn, 2nz, 2oc, 2of]

23. **`pgrep` cannot see the process a script is running inside.** BSD
    `pgrep` never matches the invoking process or **its ancestors**, so a
    script run from Claude Code cannot see the Claude app: `pgrep -x Claude`
    returns nothing while `ps` lists it. This is what made 2no's first launch
    of Claude "exit cleanly with no crash report" - the launcher's
    `running()` said no, the quit-and-wait was skipped, and the second copy
    forwarded into Electron's single-instance lock and died. The lock was the
    symptom; the blind check was the cause. Use
    `ps -axo pid=,command=` with a prefix match. [2nu]
24. **The dyld shared cache is inside a GRAFTED, Apple-signed cryptex.**
    `<Preboot>/<UUID>/cryptex1/current/os.dmg` is grafted into
    `Cryptexes/OS` at boot, which is why the tree reports Preboot's own
    `st_dev`, has no `mount` entry, is read-only to root (EROFS on `cp` AND
    `chflags`, which no CSR bit lifts) and is **absent when the volume is
    mounted raw in Recovery**. The image is raw UDRW, but the volume inside
    is sealed, its root hash is an IM4P and that hash is covered by an
    Apple-signed IM4M ticket - so it cannot be modified. Do not spend
    Recovery trips looking for the cache as a file. This rule is about the
    cache ON DISK and is unrelated to rule 22, which is about how Lilu finds
    patch bytes - editing the cryptex was never needed (2oc). [2ny]
25. **Read the machine, not the manual.** Four Recovery trips were each ended
    by something asserted from general macOS knowledge instead of measured
    here: `Cmd-R` (this is OpenCore, `PollAppleHotKeys` False - it is SPACE at
    the picker), the Data volume's name (`MACOS - Data`), Recovery booting
    from its own APFS container, and a booted-system path existing on a raw
    volume. The OpenCore config that answered the first was already in
    `logs/efi-backups/`. [2ny]
26. **A search is an instrument too.** A grep that returns nothing is not
    evidence of absence until it has been checked against a case known to
    be present. `objdump` separates mnemonic and operands with a **tab**, so
    a space-based pattern silently matched nothing and produced a confident
    wrong negative. [2lw]

27. **No vnode work at plugin init - the root filesystem is not mounted
    yet.** `RGPU::init()` runs at kext load during early kernel bootstrap.
    `FileIO::readFileToBuffer` goes through `vnode_open`, so a file probe
    there panics the boot **before the Apple progress bar appears**, with no
    `.panic` report written because the log machinery is not up either. Lilu
    makes the same call and deliberately defers it to `registerPatches`, at
    user-patcher init. Read a file from a callback that runs at exec time
    instead. The probe that did this existed only to report a precondition
    (rule 14) and so was rule 16 exactly: the instrument is a suspect. Note
    also that `-rbgpuoff` disables the plugin through
    `PluginConfiguration::bootargOff`, so `init()` never runs - a boot with
    it set is **not** a control for anything on the init path. [2oe]

28. **Attribute every log line to a boot by its timestamp AND its pid before
    using it.** `/usr/bin/log show --last boot` means "the boot I am in now",
    so it silently changes meaning the moment the machine reboots mid-session -
    rule 8 for the unified log, in a tool rule 8 does not mention. It made an
    `isAcc=NO` from an unaccelerated boot look like the answer, when the
    accelerated boot logs the same line twice and the second says **`isAcc=YES`**;
    and it made `GPU: FB: 0 of 1 opened` look like a failure in the boot under
    investigation when those lines were `WindowServer[161]` and
    `WindowServer[358]` from other boots. `--start/--end` is **not** a reliable
    filter either: a 22:30-22:50 window returned entries timestamped 21:28 and
    01:28. The per-line timestamp and pid are trustworthy; the range predicate
    is not. And **`log` is a zsh builtin** that shadows `/usr/bin/log` and
    answers `too many arguments`, which is indistinguishable from an empty log -
    always the absolute path, always with a known-present control. [2q0]

**The recurring error, stated once**

Thirty recorded instances of reading a value without first establishing
what makes it meaningful. The practice that keeps catching it: **say in the
code what would have to be true for the reading to mean anything, and log it
when it is not.** The 2kx/2la/2lc trio adds a variant worth naming — **read
the evidence already in hand before designing the next experiment**: all three
tested a global layout parameter that the user's photograph had already
excluded.

## Chip facts established on hardware

- **GC seg0 = 0x1260** (agreed four independent ways), seg1 = 0xa000. Apple's
  encoded register ids are `(dword offset << 32) | 11`, 11 = GC_HWID. Offsets
  from `docs/linux-ref/gc_10_3_0_offset.h`; byte address is
  `(seg + dword) * 4`.
- The register accessor's slots on `pm4+0x30` / `hw->0x368`: `[0x148]`
  resolves an id to a dword offset, `[0x118]` **reads** it, `[0x120]`
  **writes** it. [2fd, 2fg]
- Carve-out MC `0xF4_0000_0000`, system-physical `0x7_8000_0000`
  (`FB_OFFSET` 0x780), BAR0 `0x8_8000_0000`. Apple's GPUVM space is
  `0xFF_BF..`. **`mc2pa(mc) = mc - vram_start + vram_base_offset`** — the one
  missing term that was the whole CP halt.
- **One** SDMA instance; Apple's Navi 21 path builds four engines
  (`hw->0xc2 * 2 + 2`). [2gc, 2gi]
- The hw engine array is `hw + 0x3b0 + engine*8`: `[0]` PM4/GFX, `[1..4]` the
  four SDMA engines. The SDMA *walk* bases at `hw + 0x3b8`, so `engine[i]` is
  hw index `i + 1`. [2gu, 2gv]
- `GB_ADDR_CONFIG` is dword `0x13de`, BASE_IDX 0; this chip reads **0x242**
  (4 pipes, 4 packers, 1 SE). Fields per `gc_10_3_0_sh_mask.h`.
- `AddrSwizzleMode`: 0 = LINEAR, 9 = 64KB_S, 22 = 4KB_D_X, 25 = 64KB_S_X,
  27 = 64KB_R_X.
- **`getPDEValue` returns flags only** — the address is ORed in by its caller.
  Never hook it to correct an address. [2ko]

## Boot-args

The authoritative list, with the reason for each, is **`docs/HANDOFF.md` §9**.
The two that bite hardest:

- **Never arm `-rbgpuforceenable` without `wdt=-1`.** Without it the frozen
  boot panics at 120 s with no usable SSH window. [2hh]
- **Must stay OUT:** `-rbgpufbmatch` (spoofs `device-id`, **kills the
  accelerator** — this plugin works by making Apple *accept* `0x1681`),
  `-rbgpupdefix` (composes with the upstream fix, halt returns),
  `-rbgpunodisppanic`, `-rbgpusdmahook`, `-rbgpustampraw`, `-rbgpuregdump`,
  `debug=0x100`.
- **The linear force is ON BY DEFAULT (2oo) - `-rbgpulinear` is gone.** The
  arg is no longer read anywhere in the plugin; the behaviour it used to gate
  is now the default, and **`-rbgpunolinear`** turns it off. **You need it as
  well as the userspace fix**: boot 131 removed it and apps came up correct,
  then went wrong again after idle or a tab switch, with Terminal and other
  native apps broken regardless. The two cover different paths - native AppKit
  apps never reach the userspace chooser at all (2oj). This also retires 2mm's
  suspicion of the arg. **2r9 corrects the old explanation:** plain Metal
  render targets use raw type-0x40 backing allocations with format 0 in the
  kernel, and their own client GPU descriptors carry mode 6/10. The kernel
  force does not recompute those client texture layouts. Its exact connection
  to AppKit's backing-store lifecycle remains untraced.
- **`-rbgpumtllinear`, `-rbgpuhwblt`, `-rbgpumtlcave` and `-rbgpumtlfix` NO
  LONGER EXIST (2op).** All four were Lilu `BinaryModInfo` paths that could
  never apply (rule 22), and 2op deleted the code - ~590 lines - under rule 6:
  when a fix moves upstream, remove the downstream patch it replaces. None of
  the four strings is read anywhere in the plugin now, so leaving them in
  boot-args is harmless and does nothing. Take them out when convenient:
  `sudo tools/efi.sh bootarg remove -rbgpuhwblt` and the same for
  `-rbgpumtlcave`. Their history is in `docs/JOURNAL.md` under 2lm/2lp/2oa/2oc/2od.

- **`-rbgpumtlkpatch` / `-rbgpumtlkfix`** (2og/2oi) are the kernel-side
  patcher, which **WORKS**: it hooks `_vm_shared_region_slide` and
  `_task_set_main_thread_qos` and places bytes with `_vm_map_write_user`,
  because Lilu's user patcher cannot run here at all (rule 22). 2oi placed
  and verified the cave in a live process. **It is ON BY DEFAULT since 2oo**:
  stage B, scope `*`, no boot-arg needed. **`-rbgpumtlkoff`** disables it,
  `-rbgpumtlkpatch` selects the inert cave-only stage, and
  `rbgpumtlkscope=a,b,c` still overrides the scope. Defaulting it on is safe
  because it **fails safe**: every site is read back and compared before
  anything is written, so an OS update that moves the driver means nothing is
  patched rather than something wrong being patched. Boot 131 showed
  **WindowServer is safe to patch** - no exclusion is needed.
  `rbgpumtlkexclude=a,b` is checked FIRST and always wins: the escape hatch
  for an app that misbehaves, settable over SSH without a rebuild. Read
  `mtlk_patched` / `mtlk_failed`; detailed keys are throttled to the first 8
  patches because this runs on ~2000 execs per boot. **A leftover
  `(GPU)` wrapper is a safe no-op** - `mtlswizzle` needs exactly one match of
  a 32-byte pattern, the detour changes the first five bytes and the cave's
  trampoline holds only 30, so it finds 0 and refuses; do not shorten
  `kFind` below 31 bytes. Three things learned earlier:
  **`proc_selfname` is the parent at this hook point** - the exec'ing process
  is only reachable as `proc_name(proc_pid(get_bsdtask_info(task)))`;
  **plain `VM_PROT_READ|VM_PROT_WRITE` succeeds** on a cache page, so Lilu's
  `CS_ENFORCEMENT` surgery is never needed; and **the write is per-process
  copy-on-write**, so the cave must be verified in *this* process before the
  detour is written. The qos hook runs on **every** exec, so a fault in it
  makes the machine unbootable - rescue with `-rbgpuoff`. Read `mtlk_*` from
  the IORegistry; `mtlk_verify=1` is the only line that proves anything.
  **2r6 CLOSES ITS ONE SILENT FAILURE.** Failing safe meant an OS update that
  moves `amdMtl_HWL_DetermineSwizzleMode` left the desktop **unpatched** -
  correct, and **invisible**: the counters were published only after the
  stage-B detour, so in exactly that case `mtlk_patched`, `mtlk_failed` and
  `mtlk_cavehits` were **all absent**, indistinguishable from "the hook never
  ran" (rule 14, on this plugin's most-travelled path). Now `place()` records
  its verdict in `g_mtlkLastMatch` on **every** call, not only the first 8
  reported ones, so **`mtlk_moved`** counts the signature an OS update
  actually produces - the read succeeded and **neither** the original nor the
  replacement bytes are there, as distinct from a failed read (`0xFFFF`), a
  missing task map or an unsolved symbol - and `rbgpu_mtlk_counters()`
  publishes `mtlk_cavehits`/`mtlk_patched`/`mtlk_failed`/`mtlk_moved` on
  **every** path out, throttled to the first 64 scoped execs then every 256th
  so rule 16's warning about the diag lock still holds (~8 extra writes per
  boot). **The visible signal is `mtlk_moved_verdict 1`**, set once per boot
  when 8 scoped execs have all missed the pattern with nothing patched,
  alongside a log line carrying both unslid addresses and this boot's slide so
  the cave can be re-derived without spending another one. Read it with
  `ioreg` or any `plugin_check.sh` capture - that script dumps the whole
  `RBGPUDiag`, so the new keys need no script change. **NOT yet verified on
  hardware**: a normal boot must show `mtlk_moved` and `mtlk_moved_verdict`
  both ABSENT with `mtlk_patched` tracking `mtlk_cavehits`.

- **`-rbgpuvbl` / `-rbgpuvblpump`** (2ou/2ov) are the 30 Hz fix, both OFF by
  default. `tools/dispclk` measured the animation clock at **exactly 30.00 Hz**
  while the panel scans at 143.833 Hz - CoreVideo's fallback for a display with
  no refresh clock, which the boot framebuffer is. **`-rbgpuvbl`** fills
  `IODisplayModeInformation.refreshRate`, and boot 134 proved it takes
  (`CGDisplayModeGetRefreshRate` 0 -> 144.000) **but does not change the
  pacing** - nominal stayed 30, so that field is CoreGraphics' mode listing and
  not CoreVideo's clock. **`-rbgpuvblpump`** is the actual fix: CoreVideo paces
  off `StdFBShmem_t.vblDelta`, only `IOFramebuffer::handleVBL` writes it, and
  it only runs when a `'vbl '` `_VSLService` fires - which never exists here,
  because only an NDRV creates one. The pump calls `handleVBL(fb, nullptr)` on
  a timer, which `VSLDoInterruptService`'s own code shows is equivalent. The
  framebuffer pointer comes free from the `-rbgpuvbl` hook, so **both are
  needed**. `rbgpuvblhz=<millihertz>` sets the rate (refused outside
  20000-400000). **No DCN register is written** and it is not phase-locked, so
  it fixes pacing, not tearing. Read it with `tools/dispclk`: nominal 30.00
  means it did not take. The pump calls into IOFramebuffer 144x/s from a
  workloop - if that path needs a lock WindowServer holds the desktop can
  wedge, so rescue with `-rbgpuoff`.
- **`-rbgpufbvbl`** (2oz) is the vblank taken through the **documented API**
  and supersedes `-rbgpuvblpump` as the primary path; it implies the pump's
  timer, which is its delivery mechanism. `IOFramebuffer.h` says the class
  "will call its subclasses to set callbacks to be called on interrupt
  events", so IOFramebuffer **hands the driver the proc**; we route
  `IONDRVFramebuffer::registerForInterruptType`, keep the proc on `'vbl '`,
  return success and call it ourselves. Its disassembly closes the 30 Hz
  chain and confirms the `_VSLService` layout a third way (head `fb+0x268`,
  type `+0x08`, next `+0x28`): `"Ignore VBL"` is absent, so the path taken is
  the **empty list** - no NDRV ever called `VSLNewInterruptService` - and it
  tail-calls super, installing nothing. `unregisterInterrupt` and
  `setInterruptState` are routed too, so a sleep that disables the vblank is
  obeyed; handing back our own ref is safe by measurement - both only
  **compare** it against node addresses and never dereference it. Fixes
  **pacing**, not tearing: phase lock needs `OTG0_FRAME_COUNT` polling, which
  is read-only and is the next step. Writes **no DCN register**. Read it with
  `tools/dispclk`; rule 14 keys: `fb_vbl_calls` + `fb_vbl_t1..t8` (was the
  door knocked on, with what types), `fb_vbl_bound`/`fb_vbl_origret` (we took
  it), `fb_vbl_sent` (proc deliveries) vs `vblp_ticks` (fallback deliveries).
  **BOOT 138 RESULT: it works, but via the FALLBACK.** `fb_vbl_bound = 1` so
  the proc was captured, then macOS called `setInterruptState(ourRef, 0)` -
  `fb_vbl_state = 0` - and `fb_vbl_sent` is absent: **zero** deliveries through
  the proc. All 23553 frames came from `vblp_ticks`, 2ov's direct `handleVBL`
  call. So **2oy's ordering fix is what fixed the pacing** (`vblp_idle_nohandler
  = 682` measures the race it was built for), and keeping the fallback instead
  of deleting it is the only reason the boot delivered anything. Rule 6 does
  NOT apply yet - do not remove the pump. Also: `fb_vbl_origret` is
  **`0xE00002BE` = kIOReturnNoResources**, not the `0xE00002C7` 2oz predicted -
  that constant sits on the `"Ignore VBL"` path, which is not the path taken.
  And `registerForInterruptType` is called **54** times with types like
  `0xff0000e9` / `0x60000400` that are not fourccs, so `'vbl '` arrives after
  the 8-call `fb_vbl_t1..t8` window.
- **`-rbgpufbvram`** (2pa) publishes the label the "7 MB" is missing:
  `VRAM,totalMB` (default 4096, override `rbgpufbvrammb=<N>`, refused outside
  256-16384) and `VRAM,totalsize` on **both** the framebuffer and its provider,
  plus `model` = "AMD Radeon 680M" on the provider for the absent "Chipset
  Model" line. Routed on `IONDRVFramebuffer::enableController`, NOT done from
  `RembrandtGPUPeer::start()`, which runs before IONDRVSupport loads and is the
  boot-137 race (2oy). The cast to `IOService` is checked by asking the object
  `getMetaClass()->getClassName()` and refusing unless it is
  `IONDRVFramebuffer`. Allocates nothing - the 4096 MB is already there. Read
  it with `system_profiler SPDisplaysDataType`; "7 MB" means it did not take.
  Keys: `fbvram` (3 = published, 1 = bad pointer, 2 = wrong class, with
  `fbvram_class`), `fbvram_props`, `fbvram_routed`. **BOOT 138: CONFIRMED** -
  `fbvram 3`, `fbvram_props 5`, `fbvram_mb 4096`, and System Profiler reads
  `Chipset Model: AMD Radeon 680M` with `VRAM (Total): 4 GB`. Note for honesty
  when reporting it: the name is a **string literal we write**, and the 4096 MB
  is measured but also published by us - macOS detects neither.
- **`-rbgpuflipprobe`** (2pe) is observe-only and **has already returned its
  answer** (2pf): routed successfully, `getDisplayPipeTransactionFlip` never
  called once. Keep it only if re-testing that after a change; it writes
  nothing.
- **`-rbgpufbvramrange`** (2pq) is the flip window, **off by default**. 2pp
  measured that Apple's accelerator is **already bound to our
  IONDRVFramebuffer** (`IOAccelIndex`/`IOAccelRevision`/`IOAccelTypes` are
  published on `.Display_boot`), advertises `DisplayPipeSupported=Yes` and
  `TransactionsSupported=Yes`, and that **WindowServer holds an
  `IOAccelDisplayPipeUserClient2`** - because
  `IOAccelDisplayMachine::found_framebuffer` filters NOTHING. What it lacks is
  room: `IOGraphicsAccelerator2::find_vram_descriptor` takes its whole notion
  of scanout VRAM from slot 0x9b0 = `IOFramebuffer::getVRAMRange`, which
  returns **8294400 bytes - one buffer, room for zero others**, and a flip
  needs a second. This routes `IONDRVFramebuffer::getVRAMRange` and reports a
  bigger range **to one return address only** - the call inside
  `find_vram_descriptor` at `+0x4a`, verified by reading its six bytes
  (`ff 91 b0 09 00 00`) back from the running kernel first. All 14 other call
  sites keep the original, so **`IOFBMemorySize` staying 8294400 is the
  control**. The range is built at the original's own physical base and only
  longer, so offset 0 stays the live scanout and failure still shows a picture.
  `rbgpufbvramrangemb=<N>` clamped 8-256 (BAR0 is 256 MB, measured); default
  64. Writes **NO DCN register**: a flip touches only OTG (0/434 moved) and
  HUBP (0/80), both bit-identical dcn302 vs dcn312 - DCHUBBUB's 74/77 is a
  **modeset** requirement and the firmware already did it. **That is the "new
  evidence" the page-flipping prohibition above asks for.** Keys:
  `fbvr_routed`, `fbvr_verify`, `fbvr_orig_len` (**the gate - if it is not
  8294400 nothing else means anything**), `fbvr_hit`, `fbvr_big`, `fbvr_fail`,
  `fbvr_calls`, `fbvr_ret1..8`. **BOOT 144: the hook worked and the hypothesis
  is DEAD (2pr).** `fbvr_verify 1`, `fbvr_orig_len 8294400`, `fbvr_orig_base
  0x8_8000_0000` (**BAR0's base - the GOP framebuffer IS BAR0 offset 0**,
  measured for the first time), `fbvr_biglen 64 MB`, `fbvr_hit 2`,
  `fbvr_big 1`, and `fbvr_ret1 == fbvr_fvd + 0x50` so it fired at exactly the
  intended call site. The control held - `IOFBMemorySize` still 8294400, no
  regression, vblank 133.801/137.04 Hz. **And `bufferFlipCount` and
  `bufferSwapCount` are still 0 with `fliprq_calls` absent.** Room was never
  what was missing. Keep the arg OUT; it costs a 64 MB kernel mapping and buys
  nothing.
- **`dsc.py` NOW READS THE WHOLE CACHE - and it used to read one file of
  seven, so every negative before 2r3 was only that wide (2r2/2r3).** The
  cache is **seven** files (2r2 said nine; that was `ls | wc -l` counting
  `.atlas` and `.map`, which are not caches), each with its **own executable
  mapping at a distinct VA base**: main `0x7ff800000000`+`0x25198000`, `.01`
  `0x7ff900000000`, `.02` `0x7ffa00000000`, `.03` `0x7ffb00000000`, `.04`
  `0x7ffc00000000`, `.05` `0x7ffd00000000`, `.06` `0x7ffe00000000`. `Cache`
  now globs the siblings, combines their mappings, and **`xrefall <target>`
  scans every executable mapping in one call** - use it, not the
  single-range `xref`, for "does anything reference X". `maps` names the
  owning file. Validated before use: `xrefall` returns the same 7 LEA sites
  for `ioPresentmentFindAGDCMatch` that the old search found. Watch the
  protection bits - `VM_PROT_EXECUTE` is **4**, not 1 (`prot 5` =
  READ|EXECUTE); the first cut of `exec_ranges()` tested `& 1` and claimed
  "28 of 28 executable", which would have made a data scan look exhaustive.
  **`xref` matches ONLY `lea <reg>,[rip+disp32]`**, so its "0 LEA sites" is
  not "0 references"; **`ripall <target>` (2r4) closes that** - every
  RIP-relative encoding, all files, one `re.finditer` pass per mapping over
  `ripxref.py`'s opcode table. Use `ripall` for "does anything reference X".
  **AND THE RULE THAT COST THREE MILESTONES (2r4): WHEN XREFING A STRING,
  XREF THE STRING'S START.** 2r2/2r3 reported the AGDC error string as
  unreferenced cache-wide; it was not. I had searched for the *substring*
  `"Unable to get AGDC information"` and xrefed **its** address
  `0x7ff812ea2887`, while the C string starts seven bytes earlier at
  `0x7ff812ea2880` (`'Error: Unable to get AGDC information…'`). Code LEAs the
  START of a string and never the middle, so that search could only ever
  return zero - a false negative by construction, which a control over a
  *different* string cannot catch. Re-run on the real start: **3 sites, all in
  the main file.** Find the preceding NUL before xrefing any string. The VA space is global across subcaches, so code in `.01`
  can reference a string in the main file and the tool will never see it. **A
  hit is still a hit; a "0 hits" means only "not in this file".**
  **2r2's headline numbers are RETRACTED and must not be quoted again:** the
  "**0** LEA sites and **0** RIP-relative references to the AGDC error string"
  was the substring-offset bug above, not a property of the cache - the real
  start `0x7ff812ea2880` has **3** sites, each in its own outlined cold block
  with exactly one caller (2r5 lists them). The combining fix 2r2 asked for is
  **done** (`Cache` globs `dyld_shared_cache_x86_64h*` and `xrefall`/`ripall`
  resolve against one combined VA->(file, offset) map), and `dscsym.sh` /
  `dscsym_many.sh` now dlopen IOPresentment and IOKit as well, with a failed
  dlopen printed rather than silently renaming every address. `dscinv.sh`
  knows `IOPresentment` (base `0x7FF812E88000`, len `0x1C000` from
  `dyld_info -segments`): **449 functions**, and it is what showed that 2r4's
  `ioPresentmentFindAGDCMatch` is only an os_log string inside
  `ioPresentmentCreateAssociationMap`. Detail: JOURNAL **2r2**, **2r4**, **2r5**.
- **SKYLIGHT IS NOW READABLE, AND `CompositorSW` IS PINNED DOWN (2ps/2pt).**
  `tools/re/dsc.py` reads the dyld shared cache directly - VA <-> file offset
  from the header mappings, fast RIP-relative LEA xrefs, and a minimal Mach-O
  wrapper so `objdump` disassembles cache-resident code - because nothing else
  here can (`otool` will not open the path, `llvm-objdump` has no `-b binary`,
  no `llvm-mc`, no capstone). Symbolize **without root**: the cache slide is
  per-boot and system-wide, so dlopen SkyLight in a scratch process,
  `vmmap <pid> | grep SkyLight` for the `__TEXT` base, then `atos -p <pid>` on
  `unslid + (base - 0x7FF807B42000)`. **Measure the slide every boot** - reusing
  an old one makes `atos` answer `libBLAS` for everything.
  **Established:** `CompositorSW::CompositeLayersToDestination` is at unslid
  `0x7ff807c3e820` - the function 2pd measured at 48 % of WindowServer's main
  thread - and it gates on `dest->type`, an **int32 at destination+0x10, which
  must be 1 or 2** (type 1 served from `dest->0x18`, type 2 from `dest->0x98`),
  after two bounds checks (`0x2c <= 0x34`, `0x30 <= 0x38`).
  `SLDisplayStreamCreateCommon` writes type **3**.
  **CORRECTION, and it matters: the `Unsupported sw composite destination.` log
  line is NOT the display defect (2pt).** 2pr called it the real blocker on the
  strength of 46 994 hits in boot 143 and 31 299 in boot 144 - **both armed,
  with no unarmed control**. On a later boot with the hooks live (`dispclk`
  nominal 131.671 Hz; note `rbgpu-arm` gates only the NVRAM capture, **not** the
  hooks) the count over 20 minutes is **1**. It tracks machine activity, and
  type 3 is a **display stream** (screen capture). Also, CompositorSW plainly
  **accepts** the display's destination - 2pd measured it doing real work, and a
  rejected destination returns immediately - so the display destination's type
  is already 1 or 2. **The open question is why the display composite goes to
  `CompositorSW` instead of `CompositorMetal`**, not whether the destination is
  valid. `MetalContext::StartComposite(WSCompositeDestination*, MTLLoadAction,
  MTLStoreAction)` is the Metal entry point. Read 2ps and 2pt together; 2pr's
  headline is retracted there.
- **THE SOFTWARE-COMPOSITING CHAIN IS NOW CLOSED END TO END, AND ITS LAST LINK
  IS ONE USERSPACE CALL (2px/2py).** SkyLight routes a display to
  `CompositorSW` when its accelerator id is 0 (2pv), and the id comes from
  CoreDisplay, not from any framebuffer property:
  `MPAcceleratorForDisplayDevice` returns `mpPriv->accelerator`. **The field
  names are Apple's own**, decoded from
  `CoreDisplay::CreateMPPrivDict(MPPriv const*)`, which pairs each field with a
  CFString key - `+0x58 service`, **`+0x5c accelerator`**, `+0x60 unit`,
  `+0x64 aliasID`, `+0x74 deviceFlags`, `+0x98 fbIndex` - so nothing here rests
  on offset arithmetic. Its **single writer** is
  `CoreDisplay::Framebuffer::InitializeMPPrivAccelerator()`
  (`mpPriv->accelerator = fb->0x18`), and `fb->0x18` is written only by the
  `dispatch_once` block of `CoreDisplay::GPU::UpdateAcceleratorForMetal()` from
  **`CoreDisplay::FindAcceleratorWithMTLDevice()`**, which is just:
  `MTLCopyAllDevices()`, then the first device with
  `isLowPower && !isRemovable`, else the first `!isRemovable`, and
  `[d acceleratorPort]`; **0 if none**, in which case the fill loop never runs
  and every `mpPriv->accelerator` stays 0. **`make accelport` reproduces that
  exactly in userspace** - no kext, no boot-arg, one second. **It has NOT
  answered the question yet**: run on the session's boot it prints 0 and
  declares itself INCONCLUSIVE (`rc=3`), because that boot has **zero
  `IOAccelerator` services** (`AMDRadeonX6800_AMDRadeonHWLibsX6800` is
  `!registered, !matched`), so Metal has no device for an unrelated reason.
  **Run it in an armed boot.** Non-zero there means the id is fine and the gate
  is the loop's other precondition - `gpu->0x58 == gpu->0x60`, i.e. the
  GPUWrangler gave this `CoreDisplay::GPU` no `Framebuffer` at all - which
  points at `GPUController::ProcessGPUAdded` and `GPUWranglerDevices()`.
  **BOOT 145 ANSWERED IT, AND THE ANSWER RETRACTS 2px AND 2pv (2pz/2q0):
  `accelport` returns NON-ZERO** (10243/10755 across two boots), and
  `SLSDisplayCopyCurrentMetalDevice` hands back our 680M, so the `accelId == 0`
  test never fires and 2px's "the id is 0" was measured in an unaccelerated
  boot **and** through a client process whose code path is not
  `CDDisplay::accelerator`. Everything upstream of the compositor choice is
  healthy in an accelerated boot - accelerator service up, Metal device
  present, `GPU: FB: 1 of 1 opened`, **`isAcc=YES`**, capability flag 1
  (`__WSSystemCanCompositeWithMetal` sets it iff some Metal device is not
  `isHeadless`, and ours is `isHeadless=0`), "Metal compositor activated"
  logged twice - **and `CDDisplay::render_update` still gets `CompositorSW`,
  10 of 11 samples.** Do NOT reason "Metal ran somewhere, so the mode and
  capability globals must be set": `CaptureSurfaceMetal::Populate` calls
  `WSGetCompositorMetal` **directly** and is not one of
  `WSGetCompositorForAccelerator`'s nine callers, so that inference is unsound
  (2q0 retracts it). **BOTH of 2pz's surviving candidates are now DEAD and the
  frontier is one function (2qa).** `WSGetCompositorForAccelerator` has
  **six** paths to the software compositor and **five are closed by
  measurement**: the mode global is 0 (the string `"Metal compositor
  activated."` is LEA'd *inside* `activate_compositor_metal`, 48 bytes after
  its `mode = 0` store, so boot 149's log line measures it); the capability
  flag is 1; the debug "acceleration disabled" byte is `00` in the cache image
  and `WSInitialize` calls `debug_set_acceleration(mode == 0)`, which inverts
  (**so "nothing calls debug_set_acceleration normally" was wrong - it has
  four callers**); `CGXAcceleratorForDisplayDevice` reads the MPPriv from
  `dev->0x160`, the **same field `CreateDisplayDeviceDict` prints**, so
  `priv.accelerator = 0x14b23` is the decision's own value; and
  **`CDDisplay->0x158` CANNOT be NULL** - `all_displays()` has
  `testq %rax,%rax; je` at `0x7ff807d6fc8f`, so a NULL device skips
  construction. (Both inherited assumptions were re-checked: `dataxref` puts
  `&CDDisplay::accelerator()` at `0x7ff840a57a20` against a vtable pointer of
  `0x7ff840a579c0`, difference **exactly 0x60**; and `0x158` is the
  constructor's device argument, `movq %r12, 0x170(%rbx)` with the object at
  +0x18.) **The survivor is `CompositorMetal::new_compositor(uint)`
  (`0x7ff807f0045e`), which returns NULL unless all of
  `ctx->0x90/0xa8->vt[0x10]()/0x110/0x118/0x120/0x128` are non-NULL** - Metal
  pipeline states, queues and a `SurfacePool`. `WSGetCompositorMetal` then
  returns **NULL**, not the SW compositor, so `CDDisplay::compositor()` is
  NULL and the software path is a **fallback**, not a choice. **It fails
  silently** - no log string on the fail path, and 922 SkyLight lines in boot
  149 contain no Metal/pipeline/shader failure. That is why six milestones of
  reading flags found nothing.
  **2qb's oracle says it is NOT the shaders.** `make skyshaders` rebuilds, with
  public Metal APIs only, everything that function is observed to build:
  `SkyLightShaders.air64.metallib` (37 functions: 8 vertex, 24 fragment, 5
  kernel), the sampler state, the depth-stencil state, the shared event, the
  command queue, 5 of 5 compute pipelines - **all build** - plus
  `supportPriorityBand` YES, `maxTextureWidth2D/Height2D` 16384 (not 0),
  `acceleratorPort` 10755. A success does **not** clear WindowServer (different
  process, entitlements, ordering), but it kills that class of failure. The
  tool's render-pipeline section is a **tool limit, not evidence**: 6 of 14
  named shader pairs abort `validateWithDevice` with `'<private>'`,
  **identically in BGRA8 and RGBA16Float**, so it is the descriptor the tool
  builds; 8 build and nothing returns an NSError. `new_compositor` also
  special-cases `@"Intel"` and `@"NVIDIA"` by `rangeOfString:` on the device
  name; ours is neither.
  **2qb "corrected" 2q2 here and was WRONG; 2qd retracts it.** `-0xa00(%rbp)`
  is a **copy** of `-0x350(%rbp)` (`movq -0x350(%rbp),%rcx` /
  `movq %rcx,-0xa00(%rbp)` at `0x7ff807e32dbf`), the very slot 2q2 searched -
  and a re-run with the newer instrument confirms **0 stores, 1 load, address
  never taken**. 2q2 was right on the measurement and right to stop. What its
  negative means is now clear: **nothing in `UpdateDisplays` produces that
  `Compositor*`** - it arrives from outside, or through a pointer the frame
  stashes elsewhere. `UpdateDisplays` does call `compositor()` at three sites,
  but their results go to `%r14`/`%rbx` for `->0x10`, the MetalContext - a
  different purpose.
  **`callxref` finding zero callers of `CDDisplay::compositor()` was a missing
  instrument, and 2qb built it:** `tools/re/vcall.py` lists every
  `call *slot(%reg)`, which neither `callxref` (no direct branch) nor
  `ripxref` (displacement off a register) can see. Slots, against the vtable
  base `0x7ff840a579c0`: `accelerator()` **0x60**, `compositor()` **0x4b0**,
  `render_update()` **0x4f8**. Also `tools/re/rbpslot.py` - one rbp-relative
  stack slot, loads vs stores; `fldstore.py` skips `rm==5`, i.e. every
  rbp-relative form, which is why 2q2 could not have found it. **Next needs a
  runtime read of what `CDDisplay::compositor()` returns in WindowServer**
  (root, or an in-process probe); more disassembly of a 34 KB frame with reused
  slots is the instrument 2q2 correctly rejected.
  **CORRECTION to 2qa (2qc): ops slot 0x140 is `CGXServiceForDisplayDevice`,
  not the accelerator.** The thunk shape `movq 0x8(%rdi),%rax; movq
  SLOT(%rax),%rax` names the whole ops table (19 thunks): **0x140 =
  CGXServiceForDisplayDevice, 0x148 = CGXAcceleratorForDisplayDevice**,
  0x150/0x158 = CGXFramebufferForDisplayDevice, 0x160 =
  CGXFinishSurfaceForDisplayDevice, 0x1a8 = CGXDisplayGetVBLTiming, 0x208 =
  CGXMPSetCursorPosition. So
  `displayPipeIsAvailable = FindDisplayPipeForService(SERVICE(dev)) & 1` -
  which the function's *name* said all along - and `CGXAcquireDisplayDeviceSurface`'s
  `ops->0x158` is *get the framebuffer*, not *acquire a surface*. 2qa inferred
  both from argument shape and names instead of reading the slot. Neither
  changes 2qa's conclusion, since neither field is read by
  `WSGetCompositorForAccelerator`.
  **`make cdchain` is INCONCLUSIVE by measurement, not by argument (2qc):**
  `CGXNextDisplayDevice(0)` does `callq *[0x7ff843bb2280]`, and that global is
  **NULL outside WindowServer**, so the device list is *unreachable* from a
  client process, not empty - the first run segfaulted at address 0. Branch 5
  therefore cannot be re-measured by calling the function; the os_state dump
  stays the evidence. **Lesson: stdout redirected to a file is FULLY buffered,
  so a segfault discards every line printed before it** - the run looked like
  a tool that produced no output at all. Put
  `setvbuf(stdout, NULL, _IONBF, 0)` at the top of any probe that can fault.
  **The only remaining instrument for the frontier is the kext**: WindowServer
  is an Apple platform binary, so injection is stripped (2on), and `sample`
  needs root. The plugin already hooks `_task_set_main_thread_qos` per exec and
  writes verified bytes with `_vm_map_write_user`, so reading one return value
  out of WindowServer is a smaller change than the Metal patcher was - but it
  is a kext change on the process that owns the display, so it is the user's
  call and a mistake is a black screen.
- **BOOT 149, `sudo tools/compositor_check.sh`: THE COMPOSITOR IS A REAL SW
  OBJECT, AND THE CHAIN 2qa CLOSED IS NOT THE ONE IN USE (2qd).** The sample
  shows `render_update` (2998) -> `CompositorSW::CompositeLayersToDestination`
  (2986) -> `layer_blit_entry` -> `CGLayerKitFetch` -> `CARenderOGLRender` ->
  `CA::OGL::` - 2pd's 48 % seen from the top. So it is **not NULL**, and 2qa's
  prediction was wrong. Two branches are now closed *inside WindowServer*:
  **the capability flag is 1** (the same sample has
  `WSCALayerBacking::Flatten` -> `CaptureSurfaceMetal::Populate` ->
  `CompositorMetal::CompositeLayersToDestination`, and `WSGetCompositorMetal`
  returns NULL unless `cmpb $0x1, [0x7ff843ca08b0]` passes - which also means
  **`new_compositor` SUCCEEDS there**, so 2qa's frontier is not failing); and
  **the mode global is 0** - all three paths log
  (`'Metal compositor activated.'` in `activate_compositor_metal`,
  **`'Software compositor activated.'`** at `0x7ff807c9a900` in
  `activate_compositor_sw`, `'Noop compositor activated.'` in the mode-2 path)
  and boot 149 has the Metal line **twice and neither other**, with the Metal
  line as the control. Also read rather than trusted: `MPAcceleratorForDisplayDevice`
  takes **four** args and calls `MPDisplayDevicePriv(**dev->0x160**, which)`,
  and `MPDisplayDevicePriv` returns its argument unchanged when `which < 0`
  (or `unit == which`, or `unit > 0x20`); `unit = 0` here, so
  `*out = dev->0x160->0x5c` = the dumped `0x14b23`. And `UpdateDisplays` is
  **not** among the 8 readers of the SW global, so it does not substitute it on
  a NULL. **With mode 0, flag 1, a non-zero id and a working `new_compositor`,
  `CDDisplay::compositor()` CANNOT return the SW compositor - so that is not
  where `render_update`'s compositor comes from.** The open question is now
  exactly one: **who hands `UpdateDisplays` its `Compositor*`?** Read the
  **callers** of `WS::Updater::UpdateDisplays` - a smaller function than the
  34 KB one.
  `WorksWithCompositor` still has no direct callers because it is
  virtual - a limit of the instrument, not dead code. Still unanswered from
  2px: whether our display uses the **MP** or the **VFB** ops table, though
  `kCGDisplayIsVirtualDevice = 0` and a VFB **delegates** the accelerator query
  to the backing device's own ops table anyway.
  **Run `sudo tools/compositor_check.sh` in an armed boot** - it prints its
  preconditions, both oracles, the boot's CoreDisplay log and which caller got
  which compositor.
  **Build `tools/re/dscinv.sh <image>` FIRST** before any byte scan: a cached
  `atos` grid over the whole `__TEXT` names every function in seconds (2304 in
  CoreDisplay, 13069 in SkyLight) and a NAME has now found the answer three
  times where byte scans missed it. Its caveat: a function no grid point lands
  inside is absent, so an absent name is not an absent function. New cache tooling, all under `tools/re/`: `dscsym.sh` /
  `dscsym_many.sh` (symbolize unslid addresses, slide measured **every run**;
  the `_many` form symbolized a 0x40 grid over CoreDisplay's `__TEXT` into its
  complete **2304-function inventory** in three seconds, which is what found
  the names - cheaper than any byte scan), `dscdis.sh`, `ripxref.py` (all
  RIP-relative encodings, not just `lea`) and `fldstore.py` (every store whose
  written bytes **overlap** a field - 216 sites where 2px's single-encoding
  search found ten).
- **`-rbgpufbaccel`** (2q4/2q5) claims **`kDisplayModeAcceleratorBackedFlag`**,
  and it is the FIRST display change here that can plausibly blank the panel
  rather than merely report a wrong number. **OFF by default, implied by
  nothing.** Boot 148's `os_state` dump gave WindowServer's own view and killed
  the theory three milestones rested on: `priv.accelerator` is **0x10723**, not
  0, with a live Metal device. What is 0 instead - **`isAcceleratorBacked`**
  (bit 22 of `CGXDisplayDevice->0x64`), **`displayPipeIsAvailable`**
  (`dev->0x14c`), **`numDisplayBuffers`** (`dev->0x48`), `isBackBuffered`,
  `isPageFlipped`. Bit 22 is **`kDisplayModeAcceleratorBackedFlag = 0x00400000`
  in the PUBLIC `IOGraphicsTypes.h:177`**, a flag on
  `IODisplayModeInformation`, and Apple names it twice at the same position
  (the header constant, and `isAcceleratorBacked` in CoreDisplay's own debug
  dict). `MPRebuildDevice` copies that one bit verbatim from
  `mpPriv->0xb0`; `IOFBDriverPreflight` ORs it out of the reply to framebuffer
  user-client **selector 0x11**. **The field is ACCUMULATED, not overwritten**
  (2q7): the driver hands out `flags = 0` at our layer, something downstream
  ORs in `0xf`, and our `0x400000` rides through untouched - so both of those
  are contributors to one value, not competitors. 2q4 wrote "MEASURED: our mode
  reports `flags = 0xf`" from the state dump's *rendered* mode line and called
  it the driver's; `fbaccel_flags0 = 0` corrected it (2q6). The fix sets it inside **2ou's existing hook** on
  `IONDRVFramebuffer::getInformationForDisplayMode` - **rule 3, never a second
  `RouteRequest`** - at `flags` **offset 16** (from `offsetof`, not from
  counting header lines; refreshRate is 8). The constant is a local
  `constexpr` quoting the header, NOT an `#include`, because a compile-time
  constant cannot add an import and the 2oz trap must stay shut
  (`grep -c IOGraphicsFamily src/plugin/Info.plist` = 0). Writes **no DCN
  register**, so recovery is a reboot. **Why it is risky:** 2ou could only
  cause a wrong number to be *reported*; this advertises a **capability**. If
  macOS believes the mode is accelerator-backed it may page-flip into buffers
  that do not exist - 2pf measured `getDisplayPipeTransactionFlip` never firing
  once, and this is a candidate for what makes it fire. Rescue with
  `-rbgpuoff`. Rule 14 keys, all NVRAM-hot so a screenless boot still reports:
  `fbaccel_on`, `fbaccel_calls`, `fbaccel_set`, **`fbaccel_had`** (non-zero
  means the flag was ALREADY set and 2q4's premise is wrong),
  **`fbaccel_flags0`** (the first mode's ORIGINAL flags - if it is not `0xf`
  the machine was not in the state 2q4 measured and nothing else means
  anything). **BOOT 149: IT WORKS, AND IT CHANGES NOTHING DOWNSTREAM (2q7).**
  `isAcceleratorBacked` went **0 -> 1** against boot 148 as the control (both
  accelerated, both with a live Metal device, the arg the only difference), and
  the mode's rendered flags went `IOFlags 0xf` -> **`IOFlags 0x40000f`** with
  **Apple's own renderer now printing the word `AcceleratorBacked`**. Live keys
  from the IORegistry, not NVRAM: `fbaccel_on 1`, `fbaccel_set 3`,
  `fbaccel_flags0 0`, `fbaccel_had` absent. **No black panel** - the risk did
  not materialise. **But `displayPipeIsAvailable`, `numDisplayBuffers`,
  `isBackBuffered` and `isPageFlipped` are all still 0**, `bufferFlipCount` and
  `bufferSwapCount` still 0, and `fliprq_calls` still absent: advertising the
  capability did not make macOS flip. That is **exactly the partial result 2q5
  pre-stated as expected** - those fields have separate, untraced writers and
  sufficiency was never claimed. **2q9 traced both writers and 2qa then showed they do
  not matter**: `numDisplayBuffers` is the popcount of the low two bits of
  `IOAccelSurfaceMTL->0x13c`, a req-bits mask that starts at 0 and is only
  ORed by `add_drawable_to_list` - so the display surface has **no buffers
  attached** - and `displayPipeIsAvailable` is
  `FindDisplayPipeForService(acceleratorID) & 1`. But **neither field appears
  anywhere in `WSGetCompositorForAccelerator`**, so hooking either would have
  repeated 2pq. Find a consumer before writing to a field. Read the compositor verdict with
  `sudo tools/compositor_check.sh`; re-read the bit after a sysdiagnose with
  `tools/cdstate.sh`.
- **`-rbgpumcnew` / `-rbgpumcreg`** (2qx) are route C stage 2, **both OFF by
  default**, and they are different in kind, so do not conflate them.
  **`-rbgpumcnew` is safe**: it constructs an `OSMetaClass` at runtime with
  `sStalled == NULL`, which the kernel's own ctor handles by emitting one
  `OSKextLog` (*"preModLoad() wasn't called for class %s"*) and returning - no
  lock, no global, no panic - because every field store happens BEFORE that
  check. Expect that log line; it is confirmation, not a fault. Verdict
  `mcn_verdict 15` only: `mcn_f00` = `__ZTV11OSMetaClass + 0x10`, `mcn_f10` =
  `IOGraphicsDevice::gMetaClass`, `mcn_f20` = `0x1d0`,
  `mcn_name_is_literal 1`; `mcn_ready 0` means a symbol did not solve and
  nothing else means anything.
  **`-rbgpumcreg` is the user's decision and needs `nvram rbgpu-mcreg=1` as
  well** - and the failure mode is **a HANG, not a panic**, which is why the
  one-shot exists. `OSMetaClass::preModLoad` takes `_sStalledClassesLock` as
  its first instruction and **never unlocks on any path**, including the
  `kalloc` failure that returns 0; the only unlock lives in `postModLoad`
  (`0xffffff8000a4a3a5`), which every one of its paths reaches. So **if
  `postModLoad` is not reached the NEXT KEXT LOAD BLOCKS FOREVER** with no
  panic and no log, and **`postModLoad` must be called even when `preModLoad`
  returns NULL** - `if (!h) return;` would brick the next boot. Nothing may
  return, allocate, solve or write a diag key between the two calls.
  **THE PANIC TRAP, and it is not obvious:** `getClassName()`
  (`0xffffff8000a4a150`) does `sym = this->0x18; return sym->vt[0x148]()`, but
  the ctor stores a RAW `const char *` at +0x18 - so calling it on a fresh,
  unregistered metaclass jumps through the first eight bytes of a string
  literal. 2qw was safe only because IOFramebuffer's metaclass was already
  registered. Stage 2a therefore checks the name by **pointer identity** and
  reads `+0x00/+0x10/+0x20` as fields; only 2b calls `getClassName()`, after
  `postModLoad`. Read `mcr_ran` first (0 = one-shot unarmed, nothing
  registered), then **`mcr_post`, which must be 0**, then **`mcr_name_ok 1`**.
  `0xdc004001` = handle mismatch, `0xdc004004` = the stalled array could not
  grow. Registers nothing matchable: no personality, no provider, no
  `registerService`, class name `RembrandtGPUProbeClass`, superclass
  `IOGraphicsDevice`, size `0x1d0`, storage in our own BSS. Its vtable is the
  BASE `OSMetaClass` one, whose `alloc()` is pure virtual, so anything that
  allocated by that name would panic - nothing knows the name. Writes **no DCN
  register**. **And a registered class is NOT a published framebuffer**: 2qr
  closed the display chain at AGDC vendor info, not at the class name, so even
  `mcr_name_ok 1` buys a metaclass and nothing on screen.
  **BOOT 154: IT WORKED, AND THE ONE-SHOT DID NOT (2qy).** `mcr_post 0`
  (`kOSReturnSuccess`), `mcr_pre 1`, `mcr_check 1`, and **`mcr_name` =
  `RembrandtGPUProbeClass` with `mcr_name_ok 1`** - the class naming itself
  through Apple's own `getClassName()`. `mcn_f00` = `mcn_osmc_vt + 0x10`
  exactly, `mcn_f10` = `mcn_super_mc`, `mcn_super_off` `0x3f138` for a third
  boot running, `mcn_f20` `0x1d0`, and `mcn_f18` now a **heap** address - so
  `mcn_verdict` reads **14, which is CORRECT on the 2b path**: bit 1 is
  "+0x18 is still our literal", and `postModLoad` replacing it with an
  `OSSymbol` is what registration does. (Verdict 15 was pre-stated for stage
  **2a** only, which did not run - `mcn_on 0`.) **So an `OSMetaClass` CAN be
  constructed and registered at runtime from the boot KC, with a system-KC
  superclass and no declared IOGraphicsFamily dependency** - 2oz's blocker was
  **static linking** and nothing else. Control held: `fbvram 3`, and
  `diag_used 524` of `diag_max 768` so absence meant something.
  **BUT `mcr_oneshot 4`: the clear FAILED and `nvram rbgpu-mcreg` still reads
  1**, so the experiment ran with **no rescue in force** - had it hung, the
  next boot would have hung too. Cause NOT established; excluded by
  measurement: not the code (byte-identical to the `rbgpu-arm` path, which
  cleared in the same boot), not too early (arm runs earlier and works), not
  too late (`rbgpu-iop` worked seconds later in boot 150), not a stale cached
  `/options`. Hypothesis only: a second forced IODTNVRAM sync in the same
  window may be coalesced away. **The defect that WAS ours is fixed** - the
  function did `out(4); return true`, reporting the failure and telling the
  caller to proceed. It now retries three times and **fails closed**, so
  `why = 4` means *armed and we did NOT run*. Rule-26 gap also closed:
  `rbgpu-mcreg` was not in `plugin_check.sh`'s capture list, so only the
  `mcr_oneshot` key revealed this. **`sudo nvram -d rbgpu-mcreg` before any
  further boot.** Detail: JOURNAL **2qx**, **2qy**.
- **The "7 MB" is NOT a memory limit** and not why anything is choppy.
  `tools/vramtest` allocates and reads back 1 GB plus an 8K texture;
  `recommendedMaxWorkingSetSize`, `RCC_CONFIG_MEMSIZE` and
  `MMMC_VM_FB_LOCATION_TOP` all say 4096 MB. 8294400 bytes is exactly ONE
  1920x1080x4 buffer - `IONDRVFramebuffer`'s own scanout surface, which System
  Profiler falls back to because no `VRAM,totalMB` property exists (2or). The
  stock framebuffer IS the culprit for the Hz menu, brightness, the string and
  the pacing - but by timing, not by memory (2ov). **2pd refines this and the
  user was right to keep pushing:** the number is cosmetic, but what it
  indicates is not. One fixed CPU-mapped buffer is not a GPU surface, and that
  - not its size - is why WindowServer composites in software. "The 7 MB is
  cosmetic" was true of the label and wrong about the defect.

## Rescue

`-rbgpuoff` in boot-args: the kext still loads, no peer instance exists.
