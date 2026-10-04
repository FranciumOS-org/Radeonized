# Plan

The route: reuse Apple's own Navi 21 (RDNA2, gfx10.3) driver stack —
AMDRadeonX6000 accelerator, X6000 Metal driver, X6000Framebuffer — on the 680M,
whose graphics core is the same gfx10.3 generation, and replace what differs on
an APU. That is what NootedRed did for Vega APUs with Apple's Vega (X5000)
driver. Unlike Navi48-MacOS (RX 9070 XT), no command-stream translation should
be needed: Apple's Metal driver already emits gfx10.3 PM4.

## Step 0 — read-only probe (done: levels 1 and 2 ran, see docs/RULES.md)

Map the register BAR, read the IP discovery table and the PSP/SMU/memory-hub
state, change nothing. Answers: is the table where Linux looks for it, which IP
versions does this chip report, is the PSP's secure OS already running, does
the SMU answer, where is the carve-out in system RAM, does BAR0 window the
carve-out directly.

## Next steps (each its own kext stage, each read-mostly before it writes)

1. SMU messages: ask the SMU its firmware version, driver-interface version
   and enabled features (first register writes, via the mailbox Linux uses;
   yellow_carp_ppt.c). Built: rb_smu.c, `make kext SMU=1`.
2. PSP: check the TOC/TA state Linux checks before loading firmware
   (psp_v13_0.c); load nothing yet.
3. Hook Apple's X6000 Navi 21 accelerator from a Lilu plugin: identify the
   Navi 21 code paths that touch PSP 11 / SMU 11 / VRAM, map them against
   NootedRed's X6000FB.cpp, which already does part of this for Renoir.
4. Memory: Apple's driver assumes VRAM; the carve-out (step 0 tells where and
   how large) has to look like it.
5. Display: Apple's X6000Framebuffer knows DCN 3.0, the 680M has DCN 3.1.2.
   The least certain part.

## References

- Linux amdgpu (yellow_carp / Rembrandt paths) — the hardware truth.
- NootedRed (ChefKissInc) — APU patches on Apple's AMD drivers.
- Navi48-MacOS (Almosst-DEV) — hooks into AMDRadeonX6000 on macOS 26.6.2;
  offsets do not carry over to 15.8.1.
