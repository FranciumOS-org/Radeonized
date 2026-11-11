# Radeonized

Work towards graphics acceleration for the AMD Radeon 680M (Rembrandt APU, PCI IDs
`1002:1681` and `1002:164d`) on macOS, as an OpenCore-booted AMD-based hackintosh.

## Status

- **Built-in panel:** Laptop (Ryzen 7 7735HS, macOS Tahoe 26.7) reaches an
  accelerated desktop with the framebuffer kext and the Lilu plugin. This needs the
  firmware set to iGPU-only (Eco) mode. Results are recorded in `docs/JOURNAL.md`.
- **External displays over USB-C DisplayPort:** not working yet. The blocker is recorded
  in `docs/JOURNAL.md` and `docs/RULES.md`. 75% there.
- **Other macOS versions:** only Sequoia and Tahoe are claimed in `src/common/rb_osver.h`.
  Anything else is unmeasured, Ventura and up are added, untested.

Nothing here is supported by Apple. Installing the kexts changes your boot configuration.
Keep a known-good OpenCore boot entry before you install anything.

## Layout

- `src/kext/` — `RembrandtProbe`, a read-only probe of the GPU.
- `src/fbkext/` — `RBAMDFramebuffer`, the framebuffer kext for the built-in panel.
- `src/plugin/` — `RembrandtGPU`, the Lilu plugin that boots from the OpenCore EFI.
- `src/common/` — shared C code used by the kexts, the plugin and the host tests.
- `tools/` — build helpers, install scripts (`efi.sh`, `fbinstall.sh`) and probes.
- `app/` — Radeonize, a macOS app that checks the Mac and installs the kexts. See `app/README.md`.
- `docs/PLAN.md`, `docs/JOURNAL.md`, `docs/RULES.md` — the plan, the measured record, and the
  engineering rules the code follows.

## Building

You need Xcode (or its Command Line Tools) and two sources that are not in this repository:

- **MacKernelSDK** headers and libraries, placed at `third_party/MacKernelSDK`.
- **Lilu**, placed at `third_party/Lilu`. Lilu is BSD-3-Clause.

```sh
make kext tools     # build the probe kext and the host tests
make fbkext         # build the framebuffer kext
make plugin         # build the Lilu plugin (needs Lilu)
```

`make kext` runs the host tests first. Never load a kext whose host tests fail.

## Installing manually

Installing changes the EFI and the system kernel collection. Read `docs/RULES.md` first.

```sh
sudo tools/efi.sh install     # add the plugin to the OpenCore EFI
sudo tools/fbinstall.sh       # rebuild the auxiliary kernel collection with the framebuffer
```

Reboot after each step. `tools/efi.sh restore` and the app's Restore button put back the
configuration saved before a change.

## License

Code in this repository is MIT licensed. See `LICENSE`.
Third-party code (Lilu, MacKernelSDK) keeps its own license.
