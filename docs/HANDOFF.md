> **LATEST, 2026-10-25 (2r8/2r9).** 2r8 already ran the root `agdcinfo`
> suggested below: the same entitlement refusal, so that command is no longer
> pending. The display blocker remains the missing per-GPU AGDC service.
> **2r9 closes §2's second loose thread with a measurement:** plain Metal
> render targets reach `IOAccelResourceCreate` as **type 0x40, format 0**, a
> raw backing allocation, while their own GPU image descriptors carry
> **SW_MODE 6 or 10**. The kernel's resource histogram can read `m0` without
> making that client texture linear. Three sizes, traced and untraced,
> coordinate-coded rendering and full readback: **0 mismatches / 2,195,456
> pixels**, with `linear_force=1`. The IOSurface controls use **type 0x82**
> and descriptor mode 0. Tools: `make texturepath resourcewatch`; detail and
> limits: JOURNAL **2r9**. No kext behavior or boot configuration changed.
>
> The dated frontier entries below retain their chronology; later entries
> supersede earlier next-step suggestions.

> **FRONTIER, 2026-10-24 (2ql/2qm/2qn) - read this before anything on the
> display half.** The software-compositing chain is closed end to end and the
> remaining step is a DECISION, not a read. `Display::GetNextSurface` has two
> paths; the legacy one is dead (Apple rejects surface ids < 256, 2qk) and the
> IOSurface one is gated on `UseIOPresentment()`, which is false **only**
> because CoreDisplay classifies GPUs by `IOObjectCopyClass` on the
> IOFramebuffer and `IONDRVFramebuffer` matches none of
> Intel/AMD/NVDA/AppleParavirt, scoring the "unknown" bit 0x1 that the
> supported mask can never contain. Everything IOPresentment needs is live and
> paired - `DisplayPipeSupported=1`/`TransactionsSupported=1`, two live
> `IOAccelDisplayPipeUserClient2`s, and `AppleGPUWrangler`'s `debug-gpus`
> already listing **our** framebuffer (`0x100000548`) with **our** accelerator
> (`0x10000054a`). The client-side probe is permanently blocked: the GPUWrangler
> user client is entitlement-gated and `sudo` does not clear it
> (`kIOReturnNotPermitted` both ways). **Intervention A is now BUILT, and 2qo
> corrected it first:** the classifier `0x7ff80412cc38` has **two** callers,
> not one - the second is `CoreDisplay::Framebuffer::Framebuffer`, which stores
> the result as `MPPriv.gpuVendor`, read by the pipe-info allocator, the IOP
> gamma path and `WSMachineIsAMD`. So the patch is **five bytes at the call
> site `0x7ff80412ce97`** (`e8 9c fd ff ff` -> `b8 04 00 00 00`), which changes
> `gSystemGPUVendors` to 4 and nothing else. `-rbgpuiop` plus the **one-shot
> NVRAM `rbgpu-iop`**, so a black screen at login is recovered by a plain power
> cycle instead of a boot-arg edit from another OS. **Pending user decision -
> it is still a kext change on the process that owns the display.** Tools:
> `make useiop`, `make ioptest`. Detail: JOURNAL 2ql, 2qm, 2qn, **2qo**.
>
> **BOOT 150 RAN IT AND THE GATE OPENED (2qp).** `iop_verify 1` - the five
> bytes read back out of WindowServer's own memory - no black screen, and
> WindowServer logs Apple's own **"System set to use IOP but no display pipes
> are present"**, a string LEA'd immediately before a `movb $0x0, gUseIOP`, so
> reaching it proves the supported-mask test passed. **The class-name test is
> beaten.** The blocker is now one layer down and named, with a backtrace:
> `CGXMappedDisplayStart` -> `GPUWranglerRegisterEventBlock` ->
> `GPUController::GPUController` block -> `GPU::GPU` ->
> `Framebuffer::Framebuffer+3601` -> emplace into
> `unordered_map<unsigned long long, CoreDisplay::DisplayPipe>` **fails**, so
> zero pipes exist and IOP is switched back off. Nothing changed on screen -
> the legacy path still runs and `CompositorSW` still rasterises the desktop -
> which is the expected partial result, not a disappointment. **Keep
> `-rbgpuiop`:** without it the chain stops one step earlier and this layer is
> unreachable. Also measured: `gUseIOP` is a **latch** (read first, any failing
> path writes 0, and `CGXMappedDisplayStart` zeroes it at three more sites), so
> re-opening the vendor gate alone can never suffice if a later check fails;
> and `FB_Triage` is a **red herring** - its only readers are
> `DumpIORegistryFrameBufferInfo`/`AcceleratorInfo`, diagnostic dumps that run
> *because* the pipe failed. **FRONTIER, static, no boot:**
> `CoreDisplay::FindDisplayPipeForService` (`0x7ff804056d04`) gates on byte
> `0x7ff843bad2d8` then popcounts a table at `0x7ff843bad2b8`;
> `DisplayPipeForService` (`0x7ff804056c52`) logs *"Unable to find display
> pipe"*. Read **who populates that map**. Detail: JOURNAL **2qp**.
>
> **2qq READ IT, AND THE CAPABILITY REALLY IS ABSENT.** The pipe is built
> inline in the map's emplace: `IOPresentmentCreateForRegistryID(0,
> registryID)` -> **NULL** -> `CoreDisplay::Backtrace` thrown (which is boot
> 150's empty-message-plus-backtrace error exactly). Its worker has two NULL
> paths, each logged: `bad arg` when arg0==arg1, and **`no match %#llx %#llx`**
> - `ioPresentmentFindAGDCMatch` missing - which is the one that fired.
> **This answers 2qn:** the call that refused `ioptest` with
> `kIOReturnNotPermitted` ran inside WindowServer, which HAS the entitlement,
> and still returned NULL. The entitlement was never the obstacle; **2ql's
> verdict is confirmed by measurement**, and **2qm is contradicted** - AGDC's
> `debug-gpus` pairing exists and the match still fails. 2ql's prediction that
> forcing the gate would black-screen was **wrong**: boot 150 came up normally.
> **Free next measurement, written by Apple:** IOPresentment reads
> `IODeviceTree:/options` `boot-args` for the token **`IOPresentment`** to
> un-gate those two messages, so
> `sudo tools/efi.sh bootarg add IOPresentment` prints both ids. Logging only,
> no risk; needs `-rbgpuiop` armed in the same boot or the pipe is never
> attempted. Detail: JOURNAL **2qq**.
>
> **2qr CLOSED IT, AND IT ENDS WHERE EVERYTHING ELSE DOES.** 2qq's guess that
> the `no match` path fired was **wrong**, and boot 150's log already held the
> answer - it was filtered out by querying `subsystem ==
> "com.apple.CoreDisplay"` when IOPresentment logs under its own name:
> `01:33:39.865 WindowServer[161] (IOPresentment) Error: **Unable to get AGDC
> information.  Return code: 0x10000003**`, three milliseconds before the
> DisplayPipe backtrace, same process and thread. So the failure is the AGDC
> **information** query, one step before the map match, and IOPresentment's
> strings name what it wants: **`AGDCVendorInfo`** and **`AGDCGPUCapability`**.
> Live `debug-gpus` says both are empty - **`gpuc` = 0, `agdp` = 0** - with our
> framebuffer and accelerator correctly paired, which **corrects 2qn**: the
> association exists, its contents do not. Those fields are filled by the
> **vendor AGDC plugin**, i.e. `AMDSupport`/`AMDFramebuffer`, which never
> instantiates here (2oq: nothing matches `0x16811002`, and macOS has no DCN
> 3.1.2 code at all). **So the IOPresentment route terminates at the same root
> cause, and 2ql's structural verdict is confirmed twice from two directions.
> The next thing to supply is not a flag, a property or a five-byte patch - it
> is the AMD framebuffer stack.** Also checked and killed so nobody reopens
> them: `FB_Triage` (diagnostic dumps only) and the third-party NV kexts in
> `/Library/Extensions` (`kextstat`, control 67 - **none loaded**). The
> `IOPresentment` boot-arg token would only add detail to a cause Apple has
> already stated in one line, so it is **no longer worth a boot**. Keep
> `-rbgpuiop` in the tree: correct, verified, inert without its one-shot, and
> the only instrument that reaches this layer. Detail: JOURNAL **2qr**.

> **2qt/2qu — ROUTE C STAGE 1 IS BUILT AND HAS NOT YET ANSWERED.** 2qs scoped
> the framebuffer stack and found route C (a runtime-constructed `OSMetaClass`,
> no declared dependency) the only route no measurement closes; `-rbgpumcprobe`
> is its stage 1, read-only, constructing and registering nothing. **Boot 151
> produced no verdict because the probe's own guard refused its answer**:
> both symbols solved and rule 19's `>= 0xffffff8000000000` threw them away,
> since IOGraphicsFamily is in the **system** KC, below the kernel base. The
> rejection path also failed to record the address. Both faults are fixed, the
> "obvious" repair (the kext's own `[address, address+size)`) was caught
> statically as equally wrong, and the probe now carries a **numeric
> prediction**: `mcp_mc_off` must read `0x3f040` and `mcp_vt_off` `0x35248`,
> computed from `nm` before the boot. `mcp_verdict 15` stays the only
> meaningful verdict, and it would establish only that route C's **inputs** are
> reachable — never that a class can be **registered**, which is stage 2 and
> the part that can panic the next kext load. Rule 19 is corrected above, not
> retired. Detail: JOURNAL **2qt**, **2qu**.

> **2qv — ROUTE C's SYMBOLS ARE REACHABLE; THE OBJECT HAS TO BE READ LATER.**
> Boot 152 hit 2qu's pre-stated prediction **to the byte**: `mcp_mc_off`
> `0x3f040`, `mcp_vt_off` `0x35248`, each address exactly kext base + offset,
> both accepted. `__ZN13IOFramebuffer10gMetaClassE` and `__ZTV13IOFramebuffer`
> **are reachable from the boot KC at the addresses static analysis names** —
> that half of 2qs's question is answered. But the metaclass read back all
> zeros (`mcp_size 0`, name NULL, super NULL, `mcp_verdict 8`) because the
> symbol is in **`__DATA,__common`** — BSS — and Lilu's kext callback runs
> **before** IOGraphicsFamily's own `OSMetaClass` constructors. Control in the
> same boot: `fbvram 3`, i.e. `getMetaClass()->getClassName()` returning
> `IONDRVFramebuffer` at `enableController`, so the accessors and a populated
> metaclass work fine — later. The probe is now split: **solve at kext load,
> read from 2pa's existing `enableController` hook** (rule 3, same
> `RouteRequest`, wider gate), the one site where a populated metaclass is
> measured rather than assumed — so **keep `-rbgpufbvram`** for that control.
> `mcp_verdict 15` is still the only meaningful verdict and still establishes
> only that route C's **inputs** exist, never that a class can be
> **registered** (stage 2, the part that can panic the next kext load).
> **2qu's claim that a `[address, address+size)` check would have failed is
> retracted** — Lilu passes `0x42000`, not `kextstat`'s `0x2efea`. Detail:
> JOURNAL **2qv**.

> **2qw — ROUTE C's INPUTS ARE MEASURED PRESENT. ALL FOUR AGREE.** Boot 153,
> read live (no `plugin_check` was run, so the newest capture on disk is still
> boot 152's - rule 8): loaded UUID `89A5E227…`, `armed=1`, `mcp_deferred 1`,
> and then **`mcp_name` = `IOFramebuffer`, `mcp_size` = `0x1d0`,
> `mcp_super` = `IOGraphicsDevice`, vtable solved** - exactly what
> `IOFramebuffer::MetaClass::MetaClass()` passes to `OSMetaClass`'s
> constructor, read statically three milestones earlier. The control held
> (`fbvram 3`), so 2qv's "right address, wrong moment" is confirmed by moving
> only the moment. The offsets `0x3f040` / `0x35248` held across two boots with
> different KASLR bases. **And the superclass link is independently confirmed:**
> `mcp_super_addr - mcp_mc_addr` = `0xf8`, predicting
> `IOGraphicsDevice::gMetaClass` at `0x14fea138`, which is exactly what `nm`
> says. **So route C's INPUTS exist** - the superclass metaclass and the vtable
> to copy are reachable from the boot KC with valid contents. It still says
> **nothing** about whether a class can be **registered**: that is stage 2, it
> drives `OSMetaClass`'s stalled-class list, it panics the *next* kext load if
> wrong, and it needs its own arg and its own decision. **Caveat worth
> carrying: the diag table filled at 448 of 448** and silently dropped
> `mcp_verdict`, `mcp_say` and `mcp_super_size` - the keys holding the
> conclusion - while every component survived. Cap raised to 768 and the table
> now publishes `diag_max` / `diag_used` in slots 0 and 1 so this is visible in
> the data; see rule 14. Detail: JOURNAL **2qw**.

> **2qx — ROUTE C STAGE 2 IS BUILT, UNRUN, AND SPLIT IN TWO.** `-rbgpumcnew`
> constructs an `OSMetaClass` at runtime and is **safe** (the kernel's ctor
> handles `sStalled == NULL` with one log line and a return; every field is
> stored before that check). `-rbgpumcreg` registers it and is **the user's
> decision**, needing `nvram rbgpu-mcreg=1` as well. Two things were found by
> reading `build/re/kernel` first, and both would have cost a machine:
> **`getClassName()` dereferences `+0x18` as an `OSSymbol` while the ctor
> stores a raw `const char *` there**, so the obvious read-back would have
> jumped through a string literal in the kernel; and **`preModLoad` takes
> `_sStalledClassesLock` and never unlocks on any path**, so the real failure
> mode is the **next kext load hanging forever**, not a panic — and
> `postModLoad` must be called even when `preModLoad` returns NULL. Pre-stated:
> `mcn_verdict 15` for 2a; `mcr_post 0` then `mcr_name_ok 1` for 2b. A
> registered class is still not a published framebuffer (2qr closed the chain
> at AGDC vendor info), so this answers only "can it be done at all". Detail:
> JOURNAL **2qx**.

> **2qy — BOOT 154: A CLASS CAN BE REGISTERED AT RUNTIME FROM THE BOOT KC.**
> `mcr_post 0`, `mcr_name` = `RembrandtGPUProbeClass`, `mcr_name_ok 1`, with
> `mcn_f00` = vtable + 0x10, `mcn_f10` = `IOGraphicsDevice::gMetaClass` and
> `mcn_super_off` `0x3f138` for a third boot. `mcn_verdict 14` is correct here
> (bit 1 clears because `postModLoad` swapped `+0x18` for an `OSSymbol`; 15 was
> pre-stated for stage 2a, which did not run). **2oz's blocker was static
> linking, nothing else** — route C's two cheap questions are both answered yes
> now, and everything expensive is still ahead. It buys **no pixel**: 2qr
> closed the chain at AGDC vendor info. **AND THE ONE-SHOT'S CLEAR FAILED** —
> `mcr_oneshot 4`, `nvram rbgpu-mcreg` still 1 — so this ran with no rescue in
> force. Cause unestablished (not the code, not timing, not a stale
> `/options`); the defect that *was* ours is fixed — it reported the failure
> and returned "proceed", and now retries and **fails closed**.
> **Run `sudo nvram -d rbgpu-mcreg`.** Detail: JOURNAL **2qy**.

> **2qz — THE AGDC FIELD HAS ONE WRITER, AND IT IS A SERVICE REGISTERING
> ITSELF. ROUTE C'S EXPENSIVE HALF IS NOT THE NEXT STEP.** `AppleGPUWrangler`
> is only `0x58aa` of code, so it was read whole with no boot. `agdp` =
> registry ID of an `IORegistryEntry *` at `gpuStruct+0x68`, and that field has
> **exactly one store** (`0x13df4d34`, in the function at `0x13df4c96`):
> `safeMetaCast`, find the object's per-GPU struct, test its class against one
> of two globals, store once and retain. The wrangler imports exactly three
> `gMetaClass` symbols — `AppleGraphicsDeviceControl`,
> `AppleGraphicsDeviceControlClient`, `OSObject` — so the test is for AGDC, and
> the function has **zero callers inside the kext**, so **AGDC registers itself
> per GPU**. No `IOFramebuffer`, no vtable, no subclass on that path.
> **2qr corrected:** the AGDC kext IS loaded (4 dependents) and
> `AppleGraphicsDeviceControlPlugin` IS registered and matched — but on
> `AppleGPUWrangler`/`IOResources`, machine-level, not per-GPU. What is missing
> is a per-GPU AGDC instance. Unread: which global is which, and whether the
> `cmpq` is an exact-class test. **Next, static and free: what calls
> `0x13df4c96`.** Detail: JOURNAL **2qz**.

> **2r0 — THE AGDC PAIRING IS A MATCHING NOTIFICATION AND ITS CLASS TEST IS
> `safeMetaCast`, SO A SUBCLASS QUALIFIES.** Nothing calls `0x13df4c96`
> because the kernel does: the wrangler installs **two** notifications, both
> `serviceMatching("IOService")`, same handler `0x13df3194`, with the
> discriminating metaclass as the refCon. The handler admits via five
> `safeMetaCast`/`metaCast` tests — all superclass-chain walks — so 2qz's
> exact-class worry is settled in our favour. The wrangler imports only
> `AppleGraphicsDeviceControl`, its `Client` and `OSObject` (plus **both AGDC
> constructors**, so it subclasses AGDC itself), and **no `IOFramebuffer` or
> `IOAccelerator` metaclass at all**. AGDC is 12 KB and never calls the
> wrangler. **Route to `agdp`: publish an IOService that `safeMetaCast`s to
> `AppleGraphicsDeviceControl` — no framebuffer subclass, no 311-slot vtable —
> and 2qy already proved runtime class registration with a system-KC
> superclass.** Unestablished: which of the four globals is which; **`gpuc`
> (+0x10) still has no identified writer** (14 stores, none near the struct
> finder) and IOPresentment wants both fields; and the wrangler calls **into**
> AGDC with our object right after the store, so a runtime subclass must
> actually behave like an AGDC. Detail: JOURNAL **2r0**.

> **2r1 — `gpuc` HAS NO WRITER, AND `gpuc`/`agdp` ARE NOT WHAT IOPRESENTMENT
> ASKS FOR. 2qr's inference was a name resemblance.** `gpuStruct+0x10` is read
> by the debug dictionary and written **nowhere** in AppleGPUWrangler: all 14
> plain `0x10(%rX)` stores belong to other structs, there are **zero** SSE
> stores in the kext (control 47 `movzwl`), and all **eight**
> `filtered_doDeviceAttribute` calls write stack or caller buffers. The `0x301`
> call I proposed as the answer is a **set**, pushing the GPU `id` from `+0xc`
> into AGDC's own `+0xb0`. **And `AGDCVendorInfo`/`AGDCGPUCapability` appear 0
> times in BOTH the wrangler and AGDC** (controls held) — they are
> IOPresentment's strings, while `gpuc`/`agdp` are the wrangler's debug-dict
> keys. 2qr joined the two on the names alone. **2r0's mechanism stands; its
> motivation does not** — do not build an AGDC subclass expecting it to unblock
> IOPresentment. **Next: read IOPresentment's own path to `0x10000003`**, not
> more wrangler archaeology. Detail: JOURNAL **2r1**.

> **2r2 — THE AGDC ERROR STRING HAS NO REFERENCE ANYWHERE `dsc.py` CAN LOOK,
> AND THAT IS AN INSTRUMENT DEFECT.** The format string behind boot 150's log
> line is at `0x7ff812ea2887` (VA verified by reading it back), with
> IOPresentment's code located nearby (7 LEA sites on
> `ioPresentmentFindAGDCMatch`). Yet it has **0** references: 0 LEA sites over
> the whole `0x25198000` exec mapping, and 0 of **any** RIP-relative form in
> 198k instructions of objdump — controls held at 7 every time. The reason:
> **the cache is NINE files, each subcache with its own executable mapping at a
> distinct VA base**, and `dsc.py` reads only the file it is given. **Every
> `dsc.py` negative in this project is only one-ninth wide.** This also
> **corrects 2qo**, whose "occurs once in the cache's entire 0x25198000-byte
> executable mapping" was the main file's size — the patch is not in doubt
> (unique match within WindowServer's text, verified from its own memory), only
> the stated basis. **Next is tooling, not archaeology: teach `dsc.py` all nine
> files, then re-run the xref.** Detail: JOURNAL **2r2**.

> **2r3 — `dsc.py` NOW READS THE WHOLE CACHE; THE AGDC STRING IS STILL
> UNREFERENCED.** `Cache` globs all **seven** cache files (2r2's "nine" was
> `ls` counting `.atlas`/`.map`), combines their mappings, and the new
> **`xrefall`** scans every executable mapping in one call — validated by
> returning the same 7 LEA sites for `ioPresentmentFindAGDCMatch`. A bug in
> this change was caught by its own output: `VM_PROT_EXECUTE` is 4, not 1, so
> the first `exec_ranges()` claimed "28 of 28 executable". Re-run:
> **0 LEA sites cache-wide** for the AGDC error string, with exactly **one**
> copy of it in the cache (all files, every occurrence) and neither it nor the
> control present in the kernel collections. The absolute-address search is
> invalid here — it returns 0 for the control too. **The one live hypothesis
> left: a non-LEA RIP-relative reference**, since `xref` matches only
> `lea [rip+disp32]` — the same gap `ripxref.py` fixes on the `.dis` path.
> Next: extend that opcode set and re-run. Detail: JOURNAL **2r3**.

> **2r4 — FOUND IT, AND 2r2's DIAGNOSIS WAS WRONG.** The AGDC error string was
> never unreferenced: I had xrefed `0x7ff812ea2887`, the address of my search
> *substring*, while the C string starts at **`0x7ff812ea2880`**. Code LEAs a
> string's start, so that was a false negative by construction. Re-run: **3
> sites, all in the main cache file.** (`dsc.py`'s one-file defect was real and
> the fix stands — it just wasn't the cause.) Also added **`ripall`**: every
> RIP-relative form, all files, validated against the known 7-hit control.
> **The chain, every stub symbolized by measurement:**
> `ioPresentmentFindAGDCMatch` does `IOServiceOpen(service, task, **type=2**,
> &conn)` then `IOConnectCallMethod(conn, **selector 1**, no input,
> outputStruct=&-0x120, outputStructCnt=&-0x2568)`, and errors on a non-zero
> return **or** a zero-length reply. **`0x10000003` is NOT an `IOReturn`** —
> those are all `0xe00002xx` — so it is the user client's own status, not a
> missing property, which is how 2qr framed it. **What must exist is a type-2
> user client on the AGDC service with a selector-1 method returning a
> struct** — the same `newUserClient` type switch 2ql decoded for
> `IOGraphicsAccelerator2`. Next: trace which service `%r13d` holds at
> `0x7ff812e8ad03`, then who implements selector 1 in
> `AppleGraphicsDeviceControl`. Detail: JOURNAL **2r4**.

> **2r5 — BOTH OF 2r4's READS, ANSWERED. THE AGDC ASSOCIATION MAP IS EMPTY, AND
> `0x10000003` IS A MACH SEND TO PORT 0.** The enclosing symbol at 2r4's
> addresses is **`ioPresentmentCreateAssociationMap`** (`0x7ff812e8a730`), not
> `ioPresentmentFindAGDCMatch` — that name survives only as the **os_log
> function string** (`0x7ff812ea14f7`) of a helper inlined into it. `%r13d` is
> `IOIteratorNext` over
> `IOServiceGetMatchingServices(IOServiceMatching("AppleGraphicsDeviceControl"))`,
> **minus the entry named `AppleGPUWrangler`** — an exact 17-byte compare done
> as three xors against `"AppleGPU"`, `"Wrangler"` and the NUL at `[16]`
> (`0x7ff812e8ad6c`–`0x7ff812e8ada1`). **`make agdcinfo` measures it live,
> non-root, in a second: exactly ONE service matches, and it is the wrangler,
> so zero candidates survive and the map is EMPTY.** (So `AppleGPUWrangler`
> **is** an `AppleGraphicsDeviceControl` subclass and
> `AppleGraphicsDeviceControlPlugin` is **not** — it is live and does not
> match. Instrument control, rule 26: `ioreg -c IOUserClient -r -d1` matches
> **431** services, so `-c` does follow the chain; `-d1` *without* `-r` prints
> only the root and proves nothing.) **`0x10000003` is
> `MACH_SEND_INVALID_DEST`** (SDK `mach/message.h:807`), not a user-client
> status as 2r4 supposed: the second inlined query,
> `ioPresentmentPopulateIOFramebufferEntries`, looks our framebuffer's GPU up
> in that map (stride 0x30), finds nothing, keeps service id **0**
> (`0x7ff812e8b3be`) and opens *that* — and the tool's control measures
> `IOServiceOpen(MACH_PORT_NULL, …, 2, …)` returning exactly `0x10000003`.
> **The two selectors name themselves** in the os_log formats three bytes from
> the failing branch: **selector 1 = `kAGDCVendorInfo`** (`0x7ff812ea1512`,
> 0x2c-byte reply, `+0x28` ∈ {1,2}), **selector 0x980 = `kAGDCGPUCapability`**
> (`0x7ff812ea1625`, 0xdc bytes, non-zero count at `+0x30`) — so 2qr's
> motivation is restored while 2r1's measurement (0 hits in either kext)
> stands. **KERNEL SIDE: there is no type-2 user client to implement.**
> `AppleGraphicsDeviceControl::newUserClient` (`0x13dfd45e`) has **no switch on
> the type at all**; it is IOService's canonical body —
> `allocClassWithName(this's IOUserClientClass property)`, cast to
> `AppleGraphicsDeviceControlClient`, `initWithTask` / `authorizeTask` /
> `attach` / `start` — with the type passed only to the first two
> (`authorizeTask`'s failure is `0xE00002E2`, exactly what `agdcinfo` measures
> unprivileged). `AppleGraphicsDeviceControlClient::externalMethod` forwards
> **every** selector to `UserKernelTransfer` →
> `filtered_doDeviceAttribute(api=3, attribute=selector, …)`, so **the
> selectors are AGDC device attributes**. `0x980` is explicit (require
> `*outLen == 0xdc`, call **virtual slot `0x858`**); **attribute 1 is NOT** —
> the `attr<=0x7f` group handles only 3, 0x10 and 0x20, so it falls to the
> plugin path: 16-slot table at `0x13e00940` indexed `(attr>>2)&0x3c0`, slot
> `+0x18` ≥ attr, `agdc->0xa0` **and** `->0xa8` non-NULL, then
> `AGDCPlugin::doPluginAttribute`. **COST, before anyone builds:**
> `__ZTV26AppleGraphicsDeviceControl` spans **0x8b8 = 279 pointers**, so ~277
> instance methods **plus** the metaclass vtable 2qy showed is also required,
> in a class whose superclass is in another kernel collection, with Apple's
> code calling our methods and **no fail-safe** — and the post-gate question
> (`IONDRVFramebuffer` implements neither
> `IOPresentmentBindIOAccelSurfaceToResource` nor `SetPlaneState`) is still
> unanswered behind it. **So the display half is now a DECISION, not a read.**
> One cheap thing is left and it is the user's: `sudo ./build/out/agdcinfo`,
> which says whether an AGDC user client here answers attribute 1 at all.
> Detail: JOURNAL **2r5**.

# RembrandtGPU — handoff state (2026-10-25, after 2r9)

> §1–§3 and §5–§9 were written 2026-10-23 after 2op and still hold (they are
> about the rendering half, which is **done**). §1's compositing sentence, §4
> and §10 were corrected on 2026-10-25 for 2pa–2qn; anything else here that
> predates those milestones is history, not current state. The chronological
> record is `docs/JOURNAL.md`, grepped by milestone tag.

Apple's Navi 21 macOS GPU stack drives a **Radeon 680M** (Rembrandt APU,
`1002:1681` rev 0a, GC 10.3.3 / DCN 3.1.2 / PSP+SMU 13.0.x / SDMA 5.2.3) on
macOS 15.8.1 (24H32), ASUS TUF A15 FA507NU, via a Lilu plugin booted from the
EFI.

This file is the current **state**. `docs/JOURNAL.md` (~17 000 lines) is the
chronological evidence journal — authoritative for what was measured, in
order, including every withdrawn reading, and **not to be trimmed**. Grep it by
milestone tag (`2ks`, `2lg`) for the history of one step; never read it whole.
`docs/HANDOFF-FULL.md` is a stale dump from 2026-10-05 — ignore it.

---

## 1. State in one paragraph

**An armed boot is a usable, accelerated Mac.** The user logs in, uses the
desktop, and texts from it. The CP halt that blocked this project for its
entire 2e–2k history is **fixed** (2ks/2ku/2kw, boot 89 onward): one missing
addition, `vram_base_offset`, applied to Apple's `adjustVRAMAddress` at three
call sites. Zero VM faults, no halt, no freeze, VMIDs 1–8 all translate.
A 4096-element compute kernel returns **bit-exact** results. **But
WindowServer does NOT composite through Metal** - that line stood here until
2026-10-25 and was wrong from 2pd onward. The desktop is rasterised on the CPU
by `CompositorSW` (~48 % of WindowServer's main thread, ~9 fps); see §4.

**THE RENDERING DEFECTS ARE FIXED (2on/2oo/2op, boots 130–132).** The user on
boot 132: *"everything is flawless, not a single artifact or even an off
pixel"*. It took **two** halves, both now on by default with no boot-arg and
no setup:

* **Userspace** — the kext places a 224-byte cave in the loaded
  `AMDRadeonX6000MTLDriver`'s `__TEXT,__const` and a 5-byte detour over
  `amdMtl_HWL_DetermineSwizzleMode`, in **every process on the system**,
  Safari and other platform binaries included. It hooks
  `_vm_shared_region_slide` for the cache slide and
  `_task_set_main_thread_qos` for a per-process point at exec, and writes with
  `_vm_map_write_user`. No wrapper, no injected dylib, no change to any app
  bundle — so it survives app updates. ~2000 execs per boot, ~99.5 % patched.
  **Lilu's user patcher is irrelevant: it is disabled on Big Sur and newer and
  cannot be re-enabled (rule 22).**
* **Kernel-side** — the linear force, on unless `-rbgpunolinear`. Boot 131
  removed it and the desktop regressed: apps correct at launch, wrong again
  after idle or a tab switch, Terminal broken regardless. Native AppKit apps
  never reach the userspace chooser, and reallocated surfaces are only covered
  here.

**Defect B is closed too (2op)**: screenprobe is 32/32 CLEAN against 20/32 and
18/32 scrambled on the identical instrument before the fix.

§2 below is the *history* of how this was found, kept because its evidence
table is still true and its refuted hypotheses must not be re-run. **What is
still unexplained is WHY** — see §2's closing note and §6.

The **display half has never started**: no framebuffer kext loads, because
Apple's controller personality matches on Navi 21 PCI IDs and ours is
`0x16811002`. The desktop we have runs on the boot framebuffer with the
accelerator attached, which is why *About This Mac* still reads 7 MB.

---

## 2. SOLVED — userspace texture tiling (history + the evidence that holds)

**This section is no longer the frontier.** The fix landed in 2on–2op; §1 has
it. What follows is the evidence trail, kept for two reasons: the table below
is still measured truth, and the three refuted hypotheses must not be re-run.
The policy that works is `fix` — LINEAR for non-render-targets, render targets
mapped **off the `_X` family** (21→5, 22→6, 23→6, 25→9, 26→10, 27→10; Z modes
20/24 untouched), keyed on `usage` OR'd from descriptor **+0x68 and +0xb0**.
**WHY the `_X` family is wrong is still NOT established.**

### What is established

| Evidence | Reading |
|---|---|
| `-rbgpulinear` fixes widgets/Dock/menu bar/chrome, not app text | two different layout paths |
| `getSurfaceInfo2 calls=68` == `getPreferredSwizzleMode2 calls=68`, against `rbgpu-res calls=5458` | the kernel lays out 68 surfaces; clients create 5458 |
| `modes=00000001` | every kernel-side surface is mode 0 (LINEAR) under the flag |
| `IOAccelDevice2::get_config` is a 0x40-byte struct with **no geometry** (2lh) | userspace does not learn the tiling config from the kernel |
| `dcc calls=0` with `hk=1` | DCC metadata excluded |
| `a0=242` — the chip's own `GB_ADDR_CONFIG`: 4 pipes, 4 packers, 1 SE | kernel AddrLib geometry is **correct** |
| rotation remap 27→25, `remapped=9`, no visual change (2lc) | rotation excluded |
| compute PASS, bit-exact, 4096 elements | the shader cores and the memory path are fine |
| the photo: correct window chrome **and** scrambled glyphs in one surface | not a global layout parameter |

**2lg/2lh concluded "computed in userspace, out of this plugin's reach".
2lk REFUTES that.** 2lh read one channel — `IOAccelDevice2::get_config`, a
0x40-byte struct with no geometry — and generalised. There is a second
channel, and it carries exactly the geometry AddrLib needs:

```
AMDRadeonX6000_AMDAccelDevice::getHardwareInfo(_sAMD_GET_HW_INFO_VALUES*)
  0xbe47ca4   memcpy(vals, (hw->0x1a38)->vtable[0x1c0](), 0x204)

gfx10_HwInfoInitData          0x7ffb08cb4218  movl 0xa0(%rsi), %r9d
  (userspace)                 0x7ffb08cb421f  movl %r9d, 0xa4(%rdi)
amdMtl_InitAddrLibInterface   0x7ffb08c9409f  chipFamily   = hwInfo->0x00
                                              chipRevision = hwInfo->0x10
amdMtl_HWL_InitAddrLibHwRegFields
                              0x7ffb08bfde4d  regValue.gbAddrConfig
                                                = hwInfo->0xa4
  -> AddrCreate -> Addr2ComputeSurfaceInfo -> every client texture
```

**The swizzle equation every client texture uses comes from one dword:
`_sAMD_GET_HW_INFO_VALUES + 0xa0`**, copied verbatim with no transformation
from a struct this plugin can reach. The struct's fields are named by Apple's
own validation strings (0xbf3deda, 0xbf3df56):

```
+0x00  uint16  deviceID            +0xc0  uint64  refClkFreq
+0x08  uint32  (-> chipFamily)     +0xc8  uint64  sysClkFreq
+0x18  uint64  numRasterPipe       +0xd0  uint64  memClkFreq
+0x20  uint64  numShaderPipes      +0xd8  uint64  cgRefClkFreq
+0xa0  uint32  GB_ADDR_CONFIG
```

Apple **rejects its own record** with `0xE00002BC` if any of those is zero,
which makes this one boot answer a second question too: `gfx_min = gfx_max =
0` has been standing since 2di, so `sysClkFreq = 0` is live and would mean
userspace never gets a valid record at all.

**Boot 102 measured it, and the geometry is RIGHT on both sides.**

```
rbgpu-hwi = b=102 hk=1 calls=40 null=0 ret=00000000 dev=1681 fam=00000028
            ras=4 shd=6 gbac=00000242 hw=00000242 MATCH(hypothesis dead)
            | ref=10000 sys=20000 mem=20000 cg=10000
```

`gbac = hw = 0x242` — userspace receives the chip's own `GB_ADDR_CONFIG`.
The fallback lead (`chipFamily`/`chipRevision`) died statically in the same
step: `gfx10_HwInfoInitData`'s family selection is a `cmpl N / ja` **range**
cascade, and `kernel->0x08 = 0x28` takes the `<= 0x31` arm, which writes
`chipFamily = 0x8f` — AddrLib's `FAMILY_NV`, exactly what 2kz measured the
kernel's own AddrLib getting. All four inputs agree:

| input | kernel (2kz) | userspace (2lk) |
|---|---|---|
| chipEngine | 0xd | 0xd (hardcoded) |
| chipFamily | 0x8f | 0x8f (immediate) |
| chipRevision | 0x28 | 0x28 |
| gbAddrConfig | 0x242 | 0x242 |

**There is no AddrLib configuration difference anywhere in this stack.**

Settled for free: `ret=00000000` means Apple **accepts** its own record and
all four clocks are nonzero, so `gfx_min = gfx_max = 0` (2di) does not reach
this struct — that path is closed. And `ras=4 shd=6` is the 680M's real
topology (6 WGPs = 12 CUs), not Navi 21's.

**That is the fourth global-parameter hypothesis to die** — 2kx tiling
params, 2la kernel AddrLib geometry, 2lc rotation, 2lk userspace geometry —
and all four were excluded in advance by the same photograph: correct chrome
and scrambled glyphs *in one surface*. The frontier is not a global layout
parameter and never was.

Tooling, new and reusable: `tools/re/dyldcache.py` reads any image out of the
dyld shared cache (no Xcode on this machine, no `dyld_shared_cache_util`) and
writes a compact Mach-O `objdump` reads at the cache's own VM addresses;
`tools/re/mtldis.sh` disassembles one symbol by regex.
`build/re/mtl-syms.txt` holds all 20 282 symbols.

### Three hypotheses already refuted — do not re-run them

Each aimed at a *global* layout parameter, which the photo had already
excluded. This is the recurring error in a new dress: **failing to read the
evidence already in hand before designing the next experiment.**

- **2kx** tiling params — correct.
- **2la** AddrLib geometry (kernel side) — correct.
- **2lc** rotation — no effect.

### How it was found (history — the step that led to 2on)

Both AddrLibs agree, so a client texture is written by the CPU and read by
the GPU through the **same** equation. A self-consistent surface cannot
scramble from a layout mismatch. So either the surfaces that scramble are not
laid out through AddrLib at all, or they are laid out correctly and corrupted
by something else — the glyph atlas's producer, a blit/compute path's own
addressing, or DCC metadata on a render target (`dcc calls=0` excluded DCC
for the 74 **kernel** surfaces only, never for client ones).

**A userspace observation hook cannot exist.** Lilu's user patcher offers
only `BinaryModInfo`/`BinaryModPatch` — a byte find/replace, with
`onProcLoad`'s callback firing once per binary load in the kernel, never per
call in userspace. Userspace can be **forced**, not **read**.

So observation moved kernel-side, to where userspace's choice arrives anyway:

- **2ll** (built, observe-only, this boot): `res + 0x1dc` bits 0:4 are a
  client resource's swizzle mode, read off Apple's own
  `andl $0x1f` at `be569d8` in `AMDAccelResourceAddr2::initialize`. A
  histogram over all ~4287 resources, in note `rbgpu-mode`. It rides **inside
  the existing 2le hook**, because a second route on an already-routed
  function panics Lilu (2kn).
- **2lm** (built, **unarmed**): force `amdMtl_HWL_DetermineSwizzleMode`
  (0x7ffb08d35a11) to return LINEAR — the userspace counterpart of
  `-rbgpulinear`. The return value *is* the mode (all nine call sites pass
  `%eax` on as one), the `uint&` is a separate out-field at `rsp+8` on entry,
  and the find pattern is 32 bytes because 14 matches **2099** functions in
  that driver. Scoped to Terminal alone, since a wrong stub in a system Metal
  driver costs the login session. Behind `-rbgpumtllinear`, OUT by default.

**Boot 104 settled it with `-rbgpulinear` OUT: the histogram SPREADS**, so
`res + 0x1dc` is genuinely per-resource.

```
rbgpu-mode = b=104 seen=4788 bad=0 | m0=4598 m2=8 m22=125 m27=57 | lin=2
rbgpu-si   = b=104 calls=83 forced=0 already=0 modes=08400004
```

`0x08400004` is bits 2, 22 and 27 — exactly the histogram's non-zero modes,
from an independent hook. **190 of 4788 client resources are tiled**: 125 in
mode 22 (4KB_D_X), 57 in mode 27 (64KB_R_X), 8 in mode 2. The other 4598 are
linear with nothing forcing them.

This also **corrects boot 103's writeup**, which argued `-rbgpulinear` could
not reach this field because 2la's hook fired 70 times against 12224
resources. Boot 103 recorded *zero* non-`m0` out of 12224; at boot 104's
proportion ~500 tiled should have appeared. So the arg does drive client
resources linear, and `vt[0x1b8]` reads a mode the align manager **cached**,
which those 70 calls set and many resources share. A call-count mismatch is
not proof of independence when an unexcluded cache sits between.

**The fix shape improves:** the proper fix touches ~4% of resources and keeps
tiling everywhere else, instead of forcing all 4788.

**That question — whether the force still earns its place — was answered by
removing it.** Boot 131 took it out and the desktop regressed (apps correct at
launch, wrong after idle or a tab switch, Terminal broken regardless), so it
earns its place and is **ON BY DEFAULT since 2oo**; `-rbgpulinear` is no
longer read anywhere and `-rbgpunolinear` turns it off.

### What 2on–2op did NOT explain (updated through 2r9)

One thing remains unexplained; the second was answered in 2r9:

1. **WHY the `_X` swizzle family is wrong on this chip.** 2nr blamed Navi 21's
   pipe count and 2ns measured that false: AddrLib's whole configuration is
   `AMDCommonHwInfoRec+0xa4` = gbAddrConfig, which reads **0x242** and agrees
   with the hardware exactly. 2na's XOR-linear bit matrix still points at the
   `_X` XOR term, just not at the pipe count. The fix is a policy that works,
   not an understanding.
2. **CLOSED for the measured plain-texture path (2r9): why tiled render
   targets coexist with the kernel's linear force.** The kernel allocates
   raw memory (`IOAccelResourceCreate` type 0x40, `args+0x68` format 0);
   `AMDAccelResourceAddr2::initialize` branches past its swizzle lookup when
   that format is zero (`0xbe569a6`–`0xbe569b2`). The client's own GPU image
   descriptor still carries mode 6/10. At 64×64, 640×480 and 1024×768, with
   `linear_force=1`, the descriptor was read directly and coordinate-coded
   render/readback passed with and without interposition. **`rbgpu-mode m0`
   is not a census of GPU texture descriptors.** 2oo's blanket explanation
   that the kernel recomputes every client texture linear is withdrawn;
   both fixes are still required by the boot-131/132 experiment. The related
   question — which backing surface makes the kernel force fix Terminal —
   remains open; these scratch allocations do not trace AppKit's backing
   store lifecycle.

---

## 3. THE FIX THAT MADE THE DESKTOP WORK (2ks/2ku/2kw)

Apple's `AMDHWMemory::adjustVRAMAddress` (0xbe8cb90, nine instructions)
returns `mc - vram_start`. The GPUVM walker needs
`mc - vram_start + vram_base_offset` — Linux's `amdgpu_gmc_vram_mc2pa`.
`-rbgpuvramadj` adds that term, **only** for callers inside three named
functions:

| site | what it programs | boot 93 |
|---|---|---|
| `AMDHWVMContext::mapVMPT` (0xbe8e624) | page-table tier entries | `map=12086` |
| `AMDHWVMM::assignVMID` (0xbe91376) | `info->0x18`, the PD base in the invalidate request | `asg=11` |
| `AMDGFX10HIQHWChannel::fillMapProcessPacket` (0xbec7de6) | the PD base in the MES `PM4_MES_MAP_PROCESS` packet | `mes=29` |

`calls=12126 inmap=12126 fixed=12126`, no out-of-scope callers.
**The third site matters because the MES hands out its own VMIDs** and
`assignVMID` never sees them — `vmids` went `000e` → `01fe`.

The chain it closed: `allocVMBlock` hands out a page-table block as an MC
address → `mapVMPT` strips the MC base through `adjustVRAMAddress` →
`getPDEValue(level, 0)` adds **flags only** → the entry carries a
carve-out-relative address short by `0x7_8000_0000` → the walk fails → CPF
cannot fetch the IB → the CP's engines park at `CP_ME_CNTL = 0x15000000` (the
power-on value, never a write) → the timestamp never retires →
`resetHardwareAndReplay` waits forever → WindowServer blocks on its mutex →
freeze.

Boot 93, every indicator clean:

```
rbgpu-fault = c=0 f=0 r=0 o=0 fw=0      vmfc=1ffc (reporting fully on)
rbgpu-mew   = n=24 NEVER HALTED 0.5-12s
rbgpu-ring  = r==w at all phases, w advancing
```

**2em and 2ki are retroactively explained**: both were undoing this one
subtraction downstream, after the fact. 2ki (`-rbgpupdefix`) is now harmful —
it composes with the upstream fix (`87b000000 -> ffb000001`, boot 91) and
brings the halt back. It must stay OUT.

---

## 4. The display half

`AMDFramebuffer` matches `IOProviderClass = AtiFbStub`. No `AtiFbStub` is ever
published, because no controller personality matches our device:

```
AMDRadeonNavi21Controller  IOPCIMatch 0x73A01002 0x73A21002 0x73A31002
                                      0x73AB1002 0x73AE1002 0x73AF1002 0x73BF1002
ours                                  0x16811002
```

Chain: **controller → `AtiFbStub` → `AMDFramebuffer` → DCN.** `AMDSupport` has
no children; no framebuffer kext is loaded.

**Spoofing `device-id` does NOT work** (2lj, boot 101). `-rbgpufbmatch`
injected `device-id = 0x73A0`; HWServicesNavi matched, and **the accelerator
never instantiated**. This plugin works by making Apple **ACCEPT** `0x1681`
(`deviceTypeTable`, `DeviceCapabilityTbl`, the TTL spoof's `dev_id == 0x1681`
gate); reporting another id bypasses every one of those patches. It also
produced no `AtiFbStub`, so it bought nothing. **Must stay OUT.**

**2oq answered the prerequisite, and then made the whole personality route
pointless.** Statically, read-only, no boot:

* The linkability worry was unfounded — `AMDRadeonX6000Framebuffer.kext`'s
  `OSBundleLibraries` are all public KPIs (AppleGraphicsDeviceControl,
  IOACPIFamily, IOGraphicsFamily, IOPCIFamily, `kpi.*`), and Apple's own kext
  imports 66 `IOFramebuffer` symbols from IOGraphicsFamily, so `IOFramebuffer`
  is a class we can subclass ourselves.
* **But macOS contains no DCN 3.1.2 code at all.** Apple's DAL compiles
  `dcn10/20/21/30/302` and DML `dcn20/21/30` and stops. `dcn31`, `dcn312`,
  `vba_31`, `yellow_carp` and `rembrandt` are **0 hits** in every AMD kext,
  against a control of 58 for `dcn30`. This chip is DCN 3.1.2 / DMU 3.1.3.
  A matching personality would instantiate a Navi 21 controller with no
  resource pool, hwseq, DML or clk_mgr for this chip — it would fail, or
  program a 3.0 register layout into a 3.1.2 pipe.
* `AMDRadeonX6000Framebuffer` is **not loaded** at all (`kextstat`), so its
  OSMetaClasses are not registered either.

**What the desktop runs on now:** `.Display_boot`, class `IONDRVFramebuffer`,
`IOFBConfig = { IOFB0Hz = Yes, one mode 1920×1080 }`, `IOFBMemorySize =
8294400`, and no vblank/flip/refresh property anywhere. `system_profiler`
prints no refresh rate. **No vblank is why animations are timer-paced**, one
fixed mode is why nothing can be switched, and the 7 MB is the same fact
(Metal itself reports `recommendedMax = 4096 MB`).

**The hardware is already up**, from a level-2 probe this project already
took: `OTG0_CONTROL = 0x00011201` (master enable set) with OTG1–3 off and
OTG4/5 absent; `DCHUBP_CNTL0` distinct from HUBP1–3; DMCUB firmware
**4.0.15.47 running**. The firmware brought DCN 3.1.2 up and is scanning out
right now, and `rb_regs.h` already carries the OTG and HUBP registers needed.

**That route - adopt the running pipe, publish an IOFramebuffer, take OTG0's
vblank, flip `HUBP0_PRIMARY_SURFACE_ADDRESS` - is SUPERSEDED. Do not start
it.** Two measurements killed it and a third explained why:

* **2pf**: with `-rbgpuflipprobe` armed and routed,
  `AMDRadeonX6000_AMDHWDisplay::getDisplayPipeTransactionFlip` was **never
  called once** in a whole boot. The flip is downstream of a decision already
  lost.
* **2pr**: `-rbgpufbvramrange` gave `find_vram_descriptor` a 64 MB range
  instead of 8294400 bytes, at exactly the intended call site. `bufferFlipCount`
  and `bufferSwapCount` stayed 0. Room was never what was missing.
* **2oz**: the vblank *was* taken, through the documented API, and boot 138
  delivered all 23553 frames through the fallback timer. Pacing is fixed
  (30.000 -> 129.267 Hz nominal). It did not touch the compositing.

**What is actually wrong, traced end to end (2pd -> 2qn):** the desktop
composites in software because its `CoreDisplay::DisplaySurface` has **no
IOSurface**. `CDSurface::prepare_for_update` sets `cdsurf->0x40 = 1` when
`CoreDisplay_DisplaySurface_GetIOSurface` returns NULL, and `begin_update`
then overwrites `Update->0x1c0` - a perfectly good `CompositorMetal` - with the
software compositor. The surface comes from
`CoreDisplay::Display::GetNextSurface`, which has exactly **two** paths and
both are closed:

1. **legacy** (`CGXAcquireDisplayDeviceSurface`): ends in
   `IOAccelSurface::set_id_mode`, which rejects every surface id below 256,
   and `MPAcquireSurfaceID()` can only ever return 0..255. Apple's own kernel
   log says so. Mechanically (2ql): `newUserClient` type 0 goes to the vendor's
   `newSurface()` -> `AMDRadeonX6000_AMDAccelSurface`, which does not override
   `set_id_mode`; type 0x20 -> `IOAccelSurfaceMTL`, which has no id check.
2. **IOPresentment** (`DisplaySurfacePool` -> `CD_IOSurfaceCreate`, a real
   IOSurface): gated on `UseIOPresentment()`, false **only** because
   CoreDisplay classifies GPUs by `IOObjectCopyClass` on the IOFramebuffer -
   `Intel` 0x2, `AMD` 0x4, `NVDA` 0x8, `AppleParavirt` 0x10, **no match 0x1** -
   and `IONDRVFramebuffer` matches none of them. The supported mask is built
   only from bits 0x2/0x4/0x8/0x10, so bit 0x1 can never be in it, for any
   value of any preference. `make useiop` measures it in a second, non-root.

**Everything IOPresentment needs is already live and paired (2qm/2qn)**, so
path 2 is not a fantasy: `IOAccelIndex 0` and `IOAccelRevision 2` on
`.Display_boot`, `IOAccelDisplayPipeCapabilities = {DisplayPipeSupported=1,
TransactionsSupported=1}` on the accelerator, two live
`IOAccelDisplayPipeUserClient2` instances, `IOPresentment.framework` on disk
and loadable, and `AppleGPUWrangler`'s `debug-gpus` already listing **our**
framebuffer (`0x100000548`) together with **our** accelerator (`0x10000054a`) -
which is the pairing `ioPresentmentFindAGDCMatch()` searches for. The
client-side probe (`make ioptest`) is permanently blocked: the GPUWrangler user
client is entitlement-gated and `sudo` does **not** clear it
(`kIOReturnNotPermitted` both ways, measured).

**PENDING USER DECISION - nothing proceeds without it. Intervention A is built
(2qo), and building it refuted how 2qm described it.**

2qm said the classifier `0x7ff80412cc38` had one caller, so patching the
function would only set `gSystemGPUVendors = 0x4`. `dsc.py callxref` finds
**two**: the vendors block, and `CoreDisplay::Framebuffer::Framebuffer` +0x8ec,
which stores the result at `fb+0x1f0` = `MPPriv+0x180` = Apple's own
**`gpuVendor`** key, which the state dump prints as 1. That field is MP ops
**slot 0x200** (`MPGetGPUVendor`) and four sites read it, so the function-level
patch would also have changed the pipe-info allocator
(`access_display_pipe_info` does `testb $0x1c,%al; je <allocate>`, so an AMD
vendor makes it return **NULL** where it returns a real 0x3018-byte structure
today), the IOP gamma path, HDR support and the mirroring style. **A blast
radius is a measurement, not a description.**

**What is built instead - five bytes at the call site, not the function:**

```
0x7ff80412ce97:  e8 9c fd ff ff   callq GetGPUVendorForFramebufferService
           ->    b8 04 00 00 00   movl  $0x4, %eax
0x7ff80412ce9c:  09 05 ...        orl   %eax, gSystemGPUVendors(%rip)
```

`gSystemGPUVendors` reads 4; `gpuVendor` stays 1; all four decisions above keep
their present behaviour. One whole instruction, no branch targets a byte inside
it, nothing extra leaks (the classifier releases its own string, and the block
never released the service), and the 27-byte anchored window occurs **once in
the cache's entire 0x25198000-byte executable mapping**. Both byte arrays were
verified *in the built kext* against the live cache, not by eye.

**Two gates, and the second is the safety property.** `-rbgpuiop` in boot-args
**and** the one-shot NVRAM variable **`rbgpu-iop`**, consumed and flushed to
flash immediately before the write, checked last so a failed precondition does
not burn the arming. 2qn's rescue was `-rbgpuoff` + reboot, which needs a
boot-arg edit from another OS if the login screen never appears; **a one-shot
makes a black screen recoverable by a plain power cycle.** Scoped to
`WindowServer` by exact name, placed through the existing verified
`rbgpu_mtlk_place` at `_task_set_main_thread_qos`, before the process executes
any instruction (`GetSystemGPUVendors` is a `dispatch_once` inside
`CGXMappedDisplayStart`). Writes **no DCN register**.

**NVRAM-hot rule-14 keys, in reading order:** `iop_oneshot` (0 armed and
consumed, 1 /options unreachable, 2 not armed, 3 literal "0", **4 armed but the
clear did not take - the next boot is armed too**), `iop_saw_ws` (absent means
WindowServer never execed while the hook was live, so nothing was attempted and
nothing is proven), `iop_ready`, `iop_match` (1 the five bytes were the call,
2 already patched, 0 neither - an OS update moved the site), and
**`iop_verify 1`, the only line that proves the write landed.** One
precondition: `-rbgpumtlkoff` disables the install that routes
`_task_set_main_thread_qos`, so this patch shares that hook and an absent
`iop_on` alongside `mtlk_stage 0` means the hook was never installed, not that
the patch failed.

**The risk, unchanged and unhidden:** if IOPresentment fails *past* the gate -
`IONDRVFramebuffer` implements none of
`IOPresentmentBindIOAccelSurfaceToResource` / `IOPresentmentSetPlaneState` -
WindowServer may not bring the display up. 2qn proved no client-side probe can
rule that out, because the GPUWrangler user client is entitlement-gated and
root does not clear it. **This is the experiment that answers it.** 2ql's "do
NOT force `UseIOPresentment`" stands untouched: this patches none of its 72
call sites, only one input it reads, to a value true of the hardware. Full
detail: JOURNAL **2ql**, **2qm**, **2qn**, **2qo**.

(Intervention B - renaming `IONDRVFramebuffer`'s class via `gMetaClass + 0x18` -
is recorded in 2qm and is **strictly worse** than A for the same effect: it
changes every `IOObjectCopyClass`, ioreg and panic-log consumer system-wide to
fix one string comparison in one userspace function.)

---

## 5. Verified on hardware — do NOT re-derive

- **The chip.** GC 10.3.3, DMU 3.1.3, MMHUB/ATHUB 2.4.0, MP0/MP1 13.0.3,
  SDMA 5.2.3 (**one instance**), VCN 3.1.1. 1 SE × 2 SA × 3 WGP = 12 CUs.
- **Carve-out.** MC `0xF4_0000_0000`, system-physical `0x7_8000_0000`
  (`GCMC_VM_FB_OFFSET` = 0x780), BAR0 256 MB at `0x8_8000_0000`.
  Apple's GPUVM space is `0xFF_BF..`. `mc2pa(mc) = mc - vram_start +
  vram_base_offset`.
- **seg0 = 0x1260** for GC `BASE_IDX 0`, agreed four independent ways;
  seg1 = 0xa000. Byte address is `(seg + dword_offset) * 4`. Apple's encoded
  register ids are `(dword offset << 32) | 11`, 11 = GC_HWID.
- **The accessor slots** on `pm4+0x30` / `hw->0x368`: `[0x148]` resolves an
  encoded id to a dword offset, `[0x118]` READS it, `[0x120]` WRITES it.
- **The hw engine array** is `hw + 0x3b0 + engine*8`: `[0]` PM4/GFX,
  `[1..4]` the four SDMA engines. The SDMA *walk* bases at `hw + 0x3b8`, so
  `engine[i]` is hw index `i + 1`.
- **`GB_ADDR_CONFIG`** is dword `0x13de`, BASE_IDX 0; this chip reads `0x242`.
  Fields per `gc_10_3_0_sh_mask.h`: NUM_PIPES[2:0], PIPE_INTERLEAVE_SIZE[5:3],
  MAX_COMPRESSED_FRAGS[7:6], NUM_PKRS[10:8], NUM_SHADER_ENGINES[21:19],
  NUM_RB_PER_SE[27:26].
- **`AddrSwizzleMode`**: 0 = LINEAR, 9 = 64KB_S, 22 = 4KB_D_X, 25 = 64KB_S_X,
  27 = 64KB_R_X. Apple's `getAddrSwizzleMode` table (0xbf4efa0) is identity
  for supported modes, `0x21` = unsupported.
- **`getPDEValue` returns FLAGS ONLY** (2ko): over 108 calls,
  `L0 f=2000000000000001`, `L1 f=100000000000001`, address bits 47:12 zero.
  The address is ORed in by the caller. Never hook it to correct an address.
- **`BOOTLOAD_COMPLETE = 1`** since 2jq: Apple's `_gc_10_3_get_fw_constants`
  asks for 21 firmware entries and 2aj supplied 7, so the RLC had been booting
  **Navi 21's** `rlc_lx6_iram`/`rlc_lx6_dram`. `-rbgpurlcsub` supplies
  yellow_carp's own as sub-image ids 26/27/28/29/30. Never remove it.
- **`powerUpHW` succeeds**, eight consecutive boots.
- **Request 0x1B completes** (2ha): its `0xE00002C7` is set on the *success*
  path at 0xbe418cf. Both failure paths return `0xE00002D8`.
- **Metal works.** `MTLCopyAllDevices` returns 1 device; `newCommandQueue`
  succeeds; a runtime-compiled 4096-thread kernel returns correct results.
- **`-rbgpuforceenable`** sets `fEnabled` (bit 0x2 at `accel+0xc78`), whose
  only setter is `IOGraphicsAccelerator2::enableAccelerator()` — unreachable
  because 2ct refuses the display pipe, which is why every armed boot was
  black before 2hh.

### Key addresses (AMDRadeonX6000 unless noted)

```
0xbe8cb90  AMDHWMemory::adjustVRAMAddress        <- THE FIX SITE
0xbe8e624  AMDHWVMContext::mapVMPT
0xbe91376  AMDHWVMM::assignVMID
0xbec7de6  AMDGFX10HIQHWChannel::fillMapProcessPacket
0xbe9b9de  AMDGFX10VMM::getPDEValue              (flags only)
0xbe99342  AMDHWAlignManager2::init
0xbf4efa0  getAddrSwizzleMode table
0xbea1190  AMDGFX10PM4Engine::doStart
0xbea8ec9  powerUpHWEngines      walks hw+0x3B0, 11 entries, vtable[0x138]
0xbea8fea  startHWEngines        walks hw+0x3B8, hw->0xc2*2+2, vtable[0x148]
0xbeca1f0  Navi21Hardware::allocateHWEngines
0xbe85536  waitForHwStamp
0x126b002e _gc_get_tiling_params_10_1            (HWLibs; reads GB_ADDR_CONFIG)
0x14e20282 IOAccelDevice2::get_config            (IOAcceleratorFamily2)
```

Extracted kexts in `build/re/`: `AMDRadeonX6000`, `AMDRadeonX6800HWLibs`,
`AMDRadeonX6000HWServicesNavi`, `IOAcceleratorFamily2`, plus (from
`SystemKernelExtensions.kc`, not the boot collection) `AMDFramebuffer`,
`AMDSupport`, `AMDRadeonX6000Framebuffer`.

---

## 6. Open, unexplained, owed

- ~~**`rbgpu-vmid` reads `av=N fail=N` equal.**~~ **PAID AND RETRACTED (2r7).**
  `AMDRadeonX6000_AMDHWVMM::assignVMID` (`0xbe91376`) has exactly **one**
  `retq` (`0xbe914f3`), and the instruction before it is `movb $0x1, %al`
  (`0xbe914e3`), reached from every path. **It returns `bool` and always
  returns true**, so there is no failure return to count: `av == fail` meant
  *every call succeeded*. The hook declared the return `uint32_t`, which the
  mangled name does not carry, and the SysV ABI defines only `%al` for a bool
  — so the upper bits were the previous computation's (the assigned VMID),
  which is why `fail` could read a value that was not even 1. Exactly 2ha.
  The counter is kept under its old key, now counting `r == false`, which the
  disassembly says cannot happen: a nonzero `fail` is now real news that the
  driver changed.
- **`gfx_min = gfx_max = 0`** since 2di: the SMU reports no GFX clock range,
  GFX DPM never started. 2hl measured the CP clocked and executing, so this is
  **not** the mechanism of anything currently broken. Why it reads zero is
  still open.
- ~~**The diag table is full at `d=73`.**~~ **Stale — fixed in 2qw.** The cap
  is **768** and the table now seeds `diag_max` and `diag_used` into slots 0
  and 1 before any real key, so the two numbers that say whether the data is
  trustworthy cannot be evicted by the table filling up. **Rule 14 still
  applies: check `diag_used` against `diag_max` before concluding anything
  from an absent key** — equal means absence carries no information.
- **MMHUB's fault status register reads all-ones** on this chip, and Apple's
  `getVMFaultInfo` fills its struct whenever `(addr != 0 || status != 0)`, so
  Apple reports a **phantom hub-1 fault** on every poll. Never treat an Apple
  hub-1 fault report on this chip as real. (Downstream of the fault stream;
  gone since 2kw, `fw=0`.)
- **The peer's `/options` lookup is tried once** (`IORegistryEntry::fromPath`).
  On a boot where the peer runs early (0.52 s vs the usual 1.9 s) it loses the
  race, logs `/options not found`, and silently does not arm —
  indistinguishable from the user forgetting. `rbgpu-arm` stays set, so a plain
  reboot recovers. A bounded retry is owed. [2gy]
- **A panic can lose an NVRAM note** whose nonce would otherwise make it
  readable (2gg), and a note's write can fail to survive a power cycle (2gc).
  Cause unknown.

### Bypasses never justified

- **2am / 2an / 2ao** — `_gc_fw_autoload_is_completed`,
  `_gc_check_register_equal_ext` (RLC safe mode) and
  `_sdma_5_2_1_fw_autoload_is_completed` forced true, blanket. 2jq largely
  explains the first (the firmware tables were wrong, not the latch), and 2iv
  found `RLC_SAFE_MODE` genuinely stuck with Linux's own exit sequence unable
  to clear it across 1000 polls. 2an is **not** vindicated.
- **2cc** — `AMDVCN2EncChannel::allocateMemoryResources` stubbed true.
- **2be** — the accelerator's `stop()` patched to RET; the trap-3 guard it
  silences was never understood.
- **2ct** — the display pipe refused outright. `-rbgpudisplaypipe` (2kt) now
  adopts it instead; 2ct's premise died when `powerUpHW` began succeeding.

### Not started

VCN, GFX DPM, DCN 3.1.2 proper, the real framebuffer.

---

## 7. The recurring error — read before concluding anything

**Twenty-six recorded instances.** The shape is always the same: *a value is
read without first establishing what would make it meaningful*, or *absence is
read as a result*, or *a table is built from n=1 rows*. The 2kx/2la/2lc trio
above is the most recent, and is the variant worth naming:

> **Read the evidence already in hand before designing the next experiment.**
> All three experiments tested a *global* layout parameter that the user's
> photograph — correct chrome and scrambled glyphs in the same surface — had
> already excluded.

### The rules in force

1. **Say in the code what would have to be true for a reading to mean
   anything, and log it when it is not.**
2. **Absence is not a result.** Every path out of a probe writes a record
   naming which path it took. A silent `return`, a full diag table and a
   missing NVRAM write all look identical to "the code never got there".
3. **A measurement behind a varying gate inherits the variance.** Put probes
   where the code always reaches.
4. **A diagnostic that substitutes its own value must never narrate that value
   as a hardware result.** [2cy, which cost fifteen milestones]
5. **An overwrite NVRAM variable is this boot's only if `b=` is new.** [2gd]
6. **One variable per boot.**
7. **The instrument is a suspect.** When a measurement and a suspect are both
   new, remove the suspect rather than reasoning about it.
8. **A return code is not a result until the code that sets it has been
   read.** A named constant tells you what the NAME means, not what the
   function meant by it. [2ha]
9. **A stale note is no evidence about where a boot got to** — not weak
   evidence of an early stop. A user rebooting a black screen looks exactly
   like a hang. [2gw]
10. **A NULL check is not a pointer check** — and the yardstick depends on
    what the pointer IS (corrected 2qu). `>= 0xffffff8000000000` is the
    **kernel's** base: right for a kalloc'd kernel object, **wrong for a symbol
    in a kext**, because kexts sit on both sides of it (180 here, 50 below /
    130 above; IOGraphicsFamily `0xffffff7f95c94000` system KC, ours
    `0xffffff8004043000` boot KC). For a kext symbol use
    **`0xffffff7f80000000`**. Boot 151 was spent on this. The lesson that
    generalises: **a refusal path must publish the value it refused** — this
    one reported the rejected address as 0. (2qu also said the kext's own
    `[address, address+size)` would reject valid symbols; boot 152 measured
    that false — Lilu passes `0x42000`, spanning `__TEXT + __DATA +
    __DATA_CONST`, not `kextstat`'s `__TEXT`-only `0x2efea`. Publish the
    parameter instead of asserting it.) [2gh, 2qu, 2qv]
11. **When a fix moves upstream, REMOVE the downstream patch it replaces, in
    the same change.** "Redundant" and "inert" are different things. [2kv]

### Routing Apple's code — five rules, each paid for on hardware

1. **Never route a name unless `nm -m build/re/<kext>` shows it DEFINED
   there.** `solveSymbol` does not skip imports; it returns the bare slide and
   `routeMultiple` writes a jump there. [2c]
2. **Disassemble the first 16 bytes and refuse if a rel8/rel32 branch begins
   within the first 5.** Lilu copies displaced instructions byte-for-byte into
   a trampoline, so a short branch keeps its original displacement and lands in
   Lilu's own buffer. [2gb, "rule 9"]
3. **Grep the mangled symbol across the whole plugin before adding a route.**
   A second `RouteRequest` for an already-routed function **panics the machine
   outright** — Lilu finds the first route's `E9` jmp and refuses. A new
   transform goes **inside the existing hook**. [2kn]
4. **`RouteRequest::from` is NOT written back** by Lilu 1.7.3, despite
   `kern_patcher.hpp` documenting it as such. Call `solveSymbol` yourself.
   [2kp]
5. **Cross-check every hand-written mangled name against `nm`.** Three length
   prefixes were wrong in one change; each would have made `solveSymbol`
   return 0 and silently unscope the fix. [2kr]

vtable dumps: **call offset = dump offset − 0x10** (the address point is
symbol + 0x10). A *non-virtual* call through a vtable **symbol** uses dump
offsets directly. [2gp]

---

## 8. Procedure — who runs what

**Claude never runs `efi.sh`, `load.sh` or `unload.sh`, and never asks for the
password.** Claude may commit at any time without asking; **pushing needs
asking**. **Loading or unloading a kext needs asking.** Deliberate diagnostic
panics are off the table (user's decision, 2026-10-12). Adding an SMU message
group is the user's decision — say what it changes on the chip.

**License constraint:** NootedRed and NootRX are under the ChefKiss Open
Knowledge License v1, whose §2 and §3 forbid AI reading, summarizing,
analyzing or deriving output from the work. **Do not read or analyze them.**
Findings from those projects must come from the user in their own words.

### Build

```
make plugin          # the Lilu plugin. `make kext tools` does NOT rebuild it.
make hosttest        # must pass before any load
dwarfdump --uuid build/out/RembrandtGPU.kext/Contents/MacOS/RembrandtGPU
```

**Always check the UUID moved.** Two boots have been lost to a stale build.

### Arm a boot

```
sudo tools/efi.sh install && sudo tools/efi.sh status
sudo tools/efi.sh on && sudo nvram rbgpu-arm=1 && sudo reboot
```

**`rbgpu-arm` is one-shot** — the peer consumes it and deletes it. Every armed
boot needs it set again, and an install that only replaces the kext and edits
boot-args does **not** arm. After a panic, check `nvram rbgpu-arm` anyway
(2gg found it still at 1).

### Read a boot — in the ARMED boot, before any reboot

```
sudo tools/plugin_check.sh
```

It writes `logs/<timestamp>-plugin/` to **disk**, which survives the power
cycle; live NVRAM read after a reboot does not. Boots 57/58/59 each wrote an
early note and lost every late one, and three experiments were spent blaming
the code. The user cannot run Claude from an armed boot, so the capture has to
be a file. **Look in `logs/<timestamp>-plugin/` before reading live NVRAM.**
A 0-byte `pci.txt` means the IGPU registry subtree was locked — the hard-wedge
fingerprint.

### Before interpreting ANY boot

```
nvram rbgpu-boot                        FIRST. If it has not increased since
                                        the last capture, the boot was never
                                        armed and NOTHING in NVRAM is this
                                        boot's. `rbgpu-diag` carries no nonce,
                                        so "armed=1 reads live" certifies a
                                        STALE capture. [2iz]
kmutil showloaded | grep rembrandt      vs dwarfdump --uuid build/out/...
nvram boot-args                         which flags were ACTUALLY set
ls -lt /Library/Logs/DiagnosticReports/*.panic | head
grep -o 'com.rembrandtgpu.plugin(1.0)\[[0-9A-F-]*\]' <the .panic>
                                        WHICH BUILD panicked — not your memory
                                        of it.
```

### Rescue

`-rbgpuoff` in boot-args: the kext still loads, no peer instance exists.

### NVRAM channels (the ones that matter now)

```
rbgpu-boot    per-boot sequence counter; every note is prefixed b=<n>
rbgpu-arm     one-shot arming
rbgpu-adj     the mc2pa fix: calls / inmap / fixed / out-of-scope callers
rbgpu-fault   Apple's own VM fault records (c/f/r/o/fw)
rbgpu-ring    ring pointers, CP_ME_CNTL, GRBM_STATUS at three phases
rbgpu-surf    getSurfaceInfo2 / swizzle modes
rbgpu-res     IOAccelNewResourceArgs dumps
rbgpu-si      the surface-info rewrite counters
rbgpu-addr    GB_ADDR_CONFIG, decoded
rbgpu-disp    the display-pipe adoption
rbgpu-diag    the diag summary — FULL at d=73, carries no nonce
```

---

## 9. Boot-args — what must stay IN and OUT

**IN**, each proven:

```
-rbgpusync570 -rbgpuspoofttl -rbgpupsphw     the TTL/PSP path
-rbgpusmubringup -rbgpuptbase                the SMU link and the GART fix
-rbgpustampwait                              the bounded (2en) real wait
-rbgpunogates                                the configuration that works
-rbgpusdmaone                                what makes powerUpHW succeed
-rbgpuvmpteng1                               engine-2 requests -> engine 1,
                                             the one SDMA instance (2gw)
-rbgpuforce1b                                sends request 0x1B
-rbgpurlcsub                                 what makes the RLC autoload
                                             COMPLETE (2jq) — never remove
-rbgpunocgcg                                 CGCG off; takes, but NOT a fix
-rbgpumewatch                                the timed CP/RLC/GFXOFF samples
-rbgpuforceenable + wdt=-1                   the accelerated display.
                                             NEVER arm the first without
                                             the second — without it the
                                             frozen boot panics at 120 s with
                                             no SSH window (2hh)
-rbgpuvramadj                                2ks/2ku/2kw: THE FIX
-rbgpudisplaypipe                            2kt: adopt the display pipe
(-rbgpulinear and -rbgpumtlkfix are GONE as flags since 2oo, and
 -rbgpumtllinear, -rbgpuhwblt, -rbgpumtlcave and -rbgpumtlfix since
 2op, which deleted the Lilu code that read them. None of the six is
 read anywhere in the plugin; the first two are now DEFAULT-ON. Leaving them in boot-args is harmless and enables
 nothing. The off-switches are -rbgpunolinear and -rbgpumtlkoff, and
 rbgpumtlkscope= / rbgpumtlkexclude= still override the universal
 scope. Boot 132 ran both halves from their defaults and the desktop
 was flawless, Defect B included - 2op.)
```

**ARMED ONLY FOR ONE EXPERIMENT, and it needs an NVRAM one-shot as well:**

| flag | what it does |
|---|---|
| `-rbgpuiop` | 2qo. Five bytes in WindowServer make `gSystemGPUVendors` read 4, which is what `UseIOPresentment()` tests. Inert on its own: it also needs `sudo nvram rbgpu-iop=1`, consumed and flushed before the write, so **a black screen at login is recovered by a plain power cycle**. Read `iop_verify` - 1 is the only line that proves the write landed. §4 has the risk. |

**OUT**, each proven:

| flag | why |
|---|---|
| `-rbgpufbmatch` | spoofs `device-id`; **kills the accelerator** (2lj) |
| `-rbgpupdefix` | 2ki; composes with the upstream fix, halt returns (2kv) |
| `-rbgpunodisppanic` | publishes `IOPCITunnelled`, costs the UI (2cv) |
| `-rbgpusdmahook` | routes a function whose prologue breaks rule 9 (2gb) |
| `-rbgpustampraw` | Apple's unbounded wait; wedged the machine (2en) |
| `-rbgpuregdump` | the GC register dump stalls non-deterministically (2fe) |
| `debug=0x100` | halts on the panic screen instead of writing a report |

`wdt=-1` disables Apple's **userspace** watchdog — read, not assumed:
`IOWatchdog::start` tests bit 0 of `wdt`, logs "watchdog disabled by boot-arg"
and zeroes `+0x89/+0xa8/+0xac`. It changes nothing on the chip.

---

## 10. Where to go next

**Rendering is done** (§1, 2on–2op): flawless desktop, Defect B closed, both
halves on by default, no boot-arg and no setup. Nothing on this list is
blocking a usable machine any more.

1. **The display chain now stops at the missing per-GPU AGDC service
   (2r5/2r8).** Boot 150 already ran Intervention A and passed the vendor
   gate. IOPresentment's association map remains empty because it skips the
   only matching service, `AppleGPUWrangler`; its next query opens port 0.
   Root `agdcinfo` has also already run and was refused for missing private
   entitlements. Neither command is pending. The substantial candidate work
   is a runtime `AppleGraphicsDeviceControl` subclass plus its vendor-info
   and GPU-capability attribute protocol; post-gate presentation remains
   unproved. That kernel experiment still needs the user's decision.
   Keep the verified IOP instrument for that work. The old direct-flip plan
   remains superseded: 2pf measured no flip request, and 2pr showed extra
   VRAM room bought nothing. `-rbgpufbmatch` must stay OUT (§9).
2. **Mode setting, external displays, brightness, DSC and PSR** are all
   beyond step (d) and all need the DCN 3.1.2 code that does not exist here.
   That is where a real port would begin, and it is large: Linux's `dcn31` is
   the only reference.
3. **Why the `_X` family is wrong** remains unexplained. **The tiled-render-
   target disagreement is closed for the measured path (2r9, §2):** raw
   kernel backing allocations and client GPU texture descriptors are separate
   layouts. Still open: tracing which backing surface makes the kernel force
   fix Terminal; the scratch-process probe does not establish that lifecycle.
4. ~~**Hardening the universal fix.**~~ **Done in 2r6, not yet seen on
   hardware.** The silent case was worse than "loud in `mtlk_failed`": the
   counters were published only after the stage-B detour, so when the cave
   probe missed, `mtlk_patched`, `mtlk_failed` and `mtlk_cavehits` were **all
   absent**. Now `mtlk_moved` counts the OS-update signature specifically (the
   read succeeded and neither pattern is present), the counters go out on
   every path, and **`mtlk_moved_verdict 1`** is the one-per-boot signal, with
   a log line carrying both unslid addresses and the slide. A normal boot must
   show `mtlk_moved` and `mtlk_moved_verdict` **absent** with `mtlk_patched`
   tracking `mtlk_cavehits`.
5. ~~**The `av=N fail=N` debt.**~~ **Paid in 2r7** — see §6: the reading was
   wrong, not the driver.
6. ~~**Housekeeping.**~~ Done in 2op: the 2lm/2lp/2oa/2oc/2od Lilu
   `BinaryModInfo` paths are deleted (~590 lines, rule 6), along with
   `tools/rbgpu-app.sh`, `tools/mtllinear-app.sh` and `tools/gen_stencil.py`.
   The four boot-args they read - `-rbgpumtllinear`, `-rbgpuhwblt`,
   `-rbgpumtlcave`, `-rbgpumtlfix` - are read nowhere now. **Kept
   deliberately**: `tools/cachepatch.py` (a live dependency of
   `tools/gen_cavepatch.py`), `cachepatch.sh` / `cachepatch-recovery.sh` /
   `docs/CACHE-RECOVERY.md` (they are the *undo* for a cache install, and an
   undo is worth keeping even for something rule 24 says cannot succeed),
   `tools/iosurftex-launchd.sh` (already neutered to recovery-only, and it is
   the undo for the 2mq disaster) and `tools/mtllinear.c` (still a live option
   of `screenprobe.sh`, `MTLLINEAR=1`).
