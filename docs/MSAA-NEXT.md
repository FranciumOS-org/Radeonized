# MSAA colour surfaces — what to do when we come back to this

Full evidence: **311** in `docs/JOURNAL.md`. Reproducers: `tools/msaa/`
(`sz.m` is the decisive one — no sudo, no reboot, no boot).

## Where it stands

Browser pages are white because `getContext('webgl')` defaults to
`antialias: true`, which needs a multisampled **colour** buffer, and this
driver cannot allocate one. Everything else about WebGL works.

Measured, with controls, nothing to re-measure:

| | result |
|---|---|
| `supportsTextureSampleCount:` 2 / 4 / 8 | **YES** |
| MSAA **colour** (BGRA8, RGBA8, R8, RGBA16F; s2/s4; 16², 64²; array and not) | **nil** |
| MSAA **depth/stencil** (D32, D16, D32S8, S8) | OK |
| single-sample colour, every shape | OK |
| `heapTextureSizeAndAlignWithDescriptor:` MSAA colour | **size 0, align 0xD7739C01** |
| same call, single-sample colour / MSAA depth | 16384/4096 / 65536/65536 |

Size 0 with a garbage alignment is an **unwritten output struct**: the sizing
call failed and Metal passed it back untouched (rule 14, inside Apple's
stack). The failing image is Apple's `AMDRadeonX6000MTLDriver`, confirmed by
`DYLD_PRINT_LIBRARIES`, and it is **cache-resident** — `nm` on the on-disk
path returns 0 for *every* token including a known-present control, because
the file is not there (rule 26 caught that).

## The candidate, as a candidate

On gfx10 a multisampled **colour** surface needs **FMASK**/CMASK metadata
that depth does not; depth uses HTILE, which single-sample depth also uses
and which works here. The matrix split is exactly that shape. **Not
established:** no FMASK path read, the sizing function not disassembled, and
no machine here without the symptom (rule 39). It may touch the open `_X`
swizzle-family defect, since MSAA colour requires the `_X` modes — but that
is two facts adjacent, not a mechanism (rule 17).

## First task is the instrument, not the answer

`tools/re/dscinv.sh` takes a hardcoded list of five images and will not
accept `AMDRadeonX6000MTLDriver`. Extend it to an arbitrary image, keeping
the caching and the **0x40-grid caveat** (a function shorter than 0x40 bytes
that no grid point lands in is simply absent — print that, do not lose it
silently). Build `tools/re/dscbase.sh` first; the cache slide is per-boot and
must be measured every run (rule 41). Then inventory the image and find the
sizing path with `dscdis.sh` / `dscsym.sh` / `dsclook.py` / `dsc.py`.

## What counts as done

1. The **named** function that sizes an MSAA colour surface and the test
   inside it that fails for this ASIC — read from the binary, not inferred.
   If it gates on an ASIC family, chip id or capability-table entry, say
   which and what this chip supplies.
2. Whether our kext or plugin could legitimately influence that gate.
   **"Unreachable, and here is the evidence" closes the line and is worth as
   much as a fix.**
3. Any fix built the way this repo builds things: logic in `src/common` with
   refusal codes, a hosttest case per guard with a break-it check each
   (confirm the case FAILS when its guard is broken, and grep `error:` as
   well as `FAIL` — the `-Werror` trap has fired eight times), an opt-in
   token checked for rule 30's prefix hazard, and nothing written until the
   licence holds.

Verify with `tools/msaa/sz.m`, then `probe3.html` through the local
collector (`tools/msaa/README.md` — a page cannot write a file, so results
come back over POST). Write it up as **312** in the journal with one
paragraph on the card.

**Keep away from `docs/HANDOFF-NEXT.md`** — that is the external-display
line and it is active.
