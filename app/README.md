# Radeonize

A small macOS app that installs [Radeonized](https://github.com/FranciumOS-org/Radeonized)
(Radeon 680M / Rembrandt, `1002:1681`) on Intel Macs running OpenCore.

## What it does

1. **Checks the Mac:** Intel only, a Rembrandt GPU on the PCI bus, macOS Sequoia (15) or Tahoe (26). Warns when an NVIDIA GPU is active, which means hybrid mode.
2. **Downloads the latest GitHub release** and checks its `.sha256` before unpacking anything.
3. **Lists EFI partitions** (mounted or not, internal or USB, SSD or HDD) and marks the one with OpenCore.
4. **Installs two kexts, by two mechanisms:**
   - `RembrandtGPU.kext` → `EFI/OC/Kexts/`, after Lilu, with `config.plist` edited (same as `tools/efi.sh`).
   - `RBAMDFramebuffer.kext` → `/Library/Extensions/`, then the auxiliary kernel collection rebuilt with `kmutil` (same as `tools/fbinstall.sh`).
5. **Backs up before every change** into `~/Library/Application Support/Radeonize/backups/`, and restores from there.
6. **Builds a diagnostics report** you can read and edit before sending. Nothing is sent without a click.

## Build

```sh
./scripts/make-app.sh      # → build/Radeonize.app (ad-hoc signed, Intel)
```

Requires the Command Line Tools (`swift build`). No Xcode project is needed.

## Privileged steps

Every change to the EFI or the kernel collection runs `radeonize-helper.sh` as root through
the standard macOS administrator prompt. The helper is the only root code path. It:

- saves the state it is about to replace,
- writes,
- verifies (the EFI config lints; the UUID of the framebuffer is inside the live collection),
- rolls back if verification fails.

## The VBIOS step is gone

A released `RembrandtGPU.kext` carries no video BIOS: the image belongs to the board
vendor, so the driver uses the one already on the machine it runs on. Since 3vb-e it
**reads that image itself, in the kernel, at boot** - out of the machine's own ACPI
`VFCT` table, via `AppleACPIPlatformExpert`'s table cache. The installer does nothing
for it: no SysReport, no dumped `.aml`, no `Info.plist` injection and no extra reboot.
`Core/VFCT.swift` and the `sysreport` / `vfct-dump` helper verbs implemented that old
route and have been deleted.

Confirmed on boots 552, 553 and 554 of the reference machine, the last of those with a
release build that had no baked image to fall back on. After a first boot, the
diagnostics report carries the verdict:

- `vbios_src` - 3 means the image came from ACPI, 1 means a manually injected override,
  absent means none was published at all.
- `acpivbios` - 0 means the ACPI route succeeded; any other value is its refusal code.
- `acpivb_how` - 0 means this macOS version is not one the route is measured on, so it
  declined on purpose rather than dereferencing a guess.

If a board ever turns up whose firmware publishes no usable `VFCT`, the manual override
still exists outside the app: `tools/vbios_plist.py <kext> VFCT.aml` in the source tree,
documented in the release's `README-INSTALL.md`.

## Before you ship

- Reports go to `https://reports.jetpixel.space/report`, set in
  `Sources/Radeonize/Core/DiagnosticsReport.swift` and matched by the custom-domain route in
  `worker/wrangler.toml`. The private viewer is `https://reports.jetpixel.space/admin`.
- Deploy `worker/` (`npx wrangler r2 bucket create radeonize-reports`, then `npx wrangler deploy`).
  **`/admin*` must be behind Cloudflare Access before the domain resolves** - the worker serves
  that path with no authentication of its own, so until Access is in front of it anyone who
  guesses the URL can read every submitted report. Only `/report` is meant to be public.
- Until the worker is deployed at that hostname, uploading a report fails; the report itself is
  still shown in the app and can be pasted into an issue by hand.
- The kexts are released from the repository root, not from here: `Radeonized-<tag>.zip`
  containing `RembrandtGPU.kext`, `RBAMDFramebuffer.kext`, `README-INSTALL.md`,
  `LICENSE.amdgpu` and `tools/`.
- **A release needs `Radeonized-<tag>.zip.sha256` beside the zip**, or `ReleaseFetcher`
  stops with "has no .zip with a matching .sha256" before downloading anything - v0.1.0
  shipped without it and the app refused the release it was built for. `shasum -a 256
  Radeonized-<tag>.zip > Radeonized-<tag>.zip.sha256` is the whole step. The fetcher pins
  the sidecar to the zip's own name, so shipping a `.dmg.sha256` as well is safe.

## Not tested on hardware yet

This build compiles and the helper's EFI config edit was checked on a sample plist. The
install and restore paths have **not** been run on a real EFI or kernel collection. Test them
on a machine you can recover, with a known-good OpenCore boot entry available.

## Requirements on a tester's Mac

The installed app needs only what ships with macOS. It does not need Xcode or the
Command Line Tools: the UUID is read in Swift (`MachOUUID.swift`), and the config edit
uses `plutil`. Building the app from source still needs the Command Line Tools.

## Security notes

- The helper runs as root from inside the app bundle, through the standard
  administrator prompt. Anyone who can write to the bundle can run code as root.
  That is acceptable for a beta; ship it signed before wider distribution.
- Release downloads are checked against the `.sha256` published with them. That
  protects against corruption and tampering in transit, not against a compromised
  release.
- Install and restore change the EFI and the system kernel collection. The UUID of the
  framebuffer is checked inside the collection after the write, and the copied kexts
  are compared byte-for-byte with what was staged.
