# Radeonized

Graphics acceleration for the AMD Radeon 680M / 660M / 610M (Rembrandt APU, PCI
IDs `1002:1681` and `1002:164d`) on macOS, as an OpenCore-booted AMD-based
hackintosh. These are the integrated GPUs in Ryzen 6000 and 7x35 laptops.
Apple never shipped a driver for them, so macOS falls back to an unaccelerated
framebuffer: no Metal, no hardware video, a sluggish desktop and no brightness
control.

Radeonized teaches Apple's existing `AMDRadeonX6000` stack to drive the chip,
through a Lilu plugin and a framebuffer kext.

Nothing here is supported by Apple. Installing the kexts changes your boot
configuration. Keep a known-good OpenCore boot entry before you install
anything.

## What works

Measured on a Ryzen 7 7735HS laptop running macOS Tahoe 26.7, with the
firmware set to iGPU-only (Eco) mode. Everything is recorded in
`docs/JOURNAL.md`.

- **Full Metal acceleration.** The GPU appears in System Profiler as
  `AMD Radeon 680M`, Metal 3, 4 GB. Metal output is bit-exact against the
  reference, and Geekbench Metal lands around 22k once VRAM is raised past the
  512 MB default.
- **The built-in panel.** `RBAMDFramebuffer` owns the laptop's screen, with
  brightness control and a refresh ladder derived from the panel's own
  timings — roughly 91–140 fps on the desktop depending on refresh state.
- **Apple's compositor runs on the GPU**, the same way it would on a supported
  Mac, rather than through a software path.
- **OpenGL apps render correctly.** OpenGL draws through a *second* driver
  image with its own tiling logic that the Metal path never touches — the
  reason Java/JavaFX apps and Steam's built-in browser used to show eaten
  glyphs and scrambled images. The fix for that image is on by default.
- **USB runs on real interrupts** rather than polling.
- **It gets out of the way when it should.** Recovery and the macOS installer
  come up completely stock, so they stay usable as a way back, and a boot that
  fails to survive 60 seconds makes the next one stock automatically.

## Work in progress

- **External displays over USB-C DisplayPort: not working yet.** DP alt mode
  is granted, the sink answers over AUX, the pixel clock runs — and the PHY
  feeding the port is held in reset, which nothing on the driver side
  releases. AMD's own Linux driver has no PHY bringup for this generation; the
  display firmware owns it, and the one firmware command that targets that
  state returns no reply. Recorded in full in `docs/EXTERNAL-DISPLAY.md`. On
  laptops where HDMI is wired to the discrete GPU, HDMI and panel brightness
  are also mutually exclusive.
- **WebGL with antialiasing.** `getContext('webgl')` defaults to
  `antialias: true`, and multisampled *colour* surfaces fail to allocate, so
  some browser pages render white. Multisampled depth and stencil are fine,
  and everything single-sampled is fine.
- **Small monochrome UI glyphs can speckle** in some apps — Steam's status
  badges are the known case, while its text and artwork are correct. Traced as
  far as "not the OpenGL path"; the current candidate is a Metal surface class.
- **Occasional stalls.** Work is sometimes submitted that never retires,
  showing up as a few seconds of lag. Not reproducible on demand yet.
- **macOS versions.** Only Sequoia and Tahoe are claimed in
  `src/common/rb_osver.h`. Ventura and up will load, and are unmeasured.
- **Device IDs.** Everything has been measured on `1002:1681`. `1002:164d` is
  the same silicon and is matched, but no machine carrying it has booted this.

## What it needs

- A Ryzen 6000 / 7x35 laptop with a Rembrandt iGPU, booting macOS via OpenCore
- macOS Sequoia (15) or Tahoe (26)
- Lilu in the EFI
- **The firmware set to iGPU-only (Eco) mode.** On a switchable-graphics
  laptop the panel and its backlight can be wired to the discrete GPU, and
  then none of this reaches the screen. It is a firmware setting, not
  something software can work around.
- SIP relaxed enough to load unsigned kexts

## Installing with the app

`app/` is **Radeonize**, a macOS app that does the whole install. See
`app/README.md`.

It checks the Mac is a Rembrandt machine on a supported macOS, downloads the
latest release and verifies its checksum before unpacking, lists the EFI
partitions and marks the one holding OpenCore, then installs both kexts by
their two different mechanisms — the plugin into `EFI/OC/Kexts/` with
`config.plist` edited to load it after Lilu, and the framebuffer into
`/Library/Extensions/` with the auxiliary kernel collection rebuilt.

Every change is backed up first, into
`~/Library/Application Support/Radeonize/backups/`, and can be restored from
there. Each write is verified before the app reports success, and rolls back
if verification fails. Root is used only by one small helper script, through
the standard administrator prompt.

Reboot when it finishes.

## Building

You need Xcode (or its Command Line Tools) and two sources that are not in
this repository:

- **MacKernelSDK** headers and libraries, placed at `third_party/MacKernelSDK`.
- **Lilu**, placed at `third_party/Lilu`. Lilu is BSD-3-Clause.

```sh
make kext tools     # build the probe kext and the host tests
make fbkext         # build the framebuffer kext
make plugin         # build the Lilu plugin (needs Lilu)
```

`make kext` runs the host tests first. Never load a kext whose host tests fail.

## Installing manually

Installing changes the EFI and the system kernel collection. Read
`docs/RULES.md` first.

```sh
sudo tools/efi.sh install     # add the plugin to the OpenCore EFI
sudo tools/fbinstall.sh       # rebuild the auxiliary kernel collection with the framebuffer
```

Reboot after each step. Both print the UUID they installed — check it against
what was just built, because an install that silently did nothing looks
exactly like one that worked. `sudo tools/efi.sh status` shows what is in the
EFI right now, and `tools/efi.sh restore` and the app's Restore button put
back the configuration saved before a change.

## If a boot goes wrong

In rough order of bluntness, as boot-args or NVRAM keys:

| | what it does |
| --- | --- |
| *(nothing)* | a boot that dies inside 60 s makes the next one stock by itself |
| `sudo nvram rbgpu-arm=0` | acceleration off next boot |
| `-rbgpustock` | every feature off, plugin otherwise alive |
| `-rbgpuoff` | the plugin loads and does nothing — the documented rescue |

Recovery always boots stock, so it stays available as a way back.

## Layout

- `src/kext/` — `RembrandtProbe`, a read-only probe of the GPU.
- `src/fbkext/` — `RBAMDFramebuffer`, the framebuffer kext for the built-in panel.
- `src/plugin/` — `RembrandtGPU`, the Lilu plugin that boots from the OpenCore EFI.
- `src/common/` — shared C code used by the kexts, the plugin and the host tests.
- `tools/` — build helpers, install scripts (`efi.sh`, `fbinstall.sh`) and probes.
- `app/` — Radeonize, a macOS app that checks the Mac and installs the kexts.
- `docs/PLAN.md`, `docs/JOURNAL.md`, `docs/EXTERNAL-DISPLAY.md`, `docs/RULES.md` —
  the plan, the measured record, the open goal, and the engineering rules the
  code follows.

Every register offset cites the Linux header it came from; `docs/linux-ref/`
holds those files unmodified.

## License

Code in this repository is MIT licensed. See `LICENSE`.
Third-party code (Lilu, MacKernelSDK) keeps its own license.
