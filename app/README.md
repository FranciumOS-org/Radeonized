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

## Before you ship

- Replace the placeholder `endpoint` in `Sources/Radeonize/Core/DiagnosticsReport.swift`
  with your deployed Worker URL.
- Deploy `worker/` (`npx wrangler r2 bucket create radeonize-reports`, then `npx wrangler deploy`).
  Put `/admin*` behind Cloudflare Access.
- Publish a release on the Radeonized repo with a `.zip` (containing `RembrandtGPU.kext` and
  `RBAMDFramebuffer.kext`) and a `.sha256` file.

## Not tested on hardware yet

This build compiles and the helper's EFI config edit was checked on a sample plist. The
install and restore paths have **not** been run on a real EFI or kernel collection. Test them
on a machine you can recover, with a known-good OpenCore boot entry available.
