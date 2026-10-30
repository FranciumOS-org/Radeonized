#!/usr/bin/env python3
"""Edit an OpenCore config.plist for the RembrandtGPU plugin. Used by
tools/efi.sh; works on any path, so it can be tried on a copy first.

    python3 tools/efi_config.py CONFIG status
    python3 tools/efi_config.py CONFIG add-kext       # Kernel -> Add, right after Lilu
    python3 tools/efi_config.py CONFIG remove-kext
    python3 tools/efi_config.py CONFIG bootarg add -rbgpuoff
    python3 tools/efi_config.py CONFIG bootarg remove -rbgpuoff
    python3 tools/efi_config.py CONFIG bootarg strip      # every rbgpu token
    python3 tools/efi_config.py CONFIG sysreport on|off   # Misc -> Debug -> SysReport

Only these keys are touched: the RembrandtGPU entry of Kernel -> Add,
boot-args under NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82, and
Misc -> Debug -> SysReport (OpenCore DEBUG builds dump ACPI tables, among
them the VBIOS table VFCT, to the EFI's SysReport folder at boot).
Everything else is written back as read (plistlib keeps key order, data and
types). The new file is written next to the old one, read back, and only
then moved over it, so an error never leaves a half-written config.
"""
import os
import plistlib
import re
import sys

BOOT_GUID = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
BUNDLE = "RembrandtGPU.kext"
ENTRY = {
    "Arch": "x86_64",
    "BundlePath": BUNDLE,
    "Comment": "RembrandtGPU (Radeon 680M)",
    "Enabled": True,
    "ExecutablePath": "Contents/MacOS/RembrandtGPU",
    "MaxKernel": "",
    "MinKernel": "",
    "PlistPath": "Contents/Info.plist",
}


class ConfigError(Exception):
    pass


def kext_list(cfg):
    try:
        lst = cfg["Kernel"]["Add"]
    except (KeyError, TypeError):
        raise ConfigError("no Kernel -> Add array; is this an OpenCore config.plist?")
    if not isinstance(lst, list):
        raise ConfigError("Kernel -> Add is not an array")
    return lst


def find(lst, bundle):
    return [i for i, e in enumerate(lst) if isinstance(e, dict) and e.get("BundlePath") == bundle]


def add_kext(cfg):
    lst = kext_list(cfg)
    lilu = [i for i in find(lst, "Lilu.kext") if lst[i].get("Enabled")]
    if not lilu:
        raise ConfigError("no enabled Lilu.kext in Kernel -> Add")
    for i in reversed(find(lst, BUNDLE)):       # re-add fresh, so position and fields are right
        del lst[i]
    at = find(lst, "Lilu.kext")[0] + 1
    lst.insert(at, dict(ENTRY))
    return f"Kernel -> Add: {BUNDLE} at index {at}, right after Lilu.kext"


def remove_kext(cfg):
    lst = kext_list(cfg)
    hits = find(lst, BUNDLE)
    for i in reversed(hits):
        del lst[i]
    return f"Kernel -> Add: removed {len(hits)} {BUNDLE} entr{'y' if len(hits) == 1 else 'ies'}"


def boot_vars(cfg):
    try:
        v = cfg["NVRAM"]["Add"][BOOT_GUID]
    except (KeyError, TypeError):
        raise ConfigError(f"no NVRAM -> Add -> {BOOT_GUID}")
    if not isinstance(v.get("boot-args"), str):
        raise ConfigError("no boot-args string in NVRAM -> Add")
    return v


def bootarg(cfg, op, arg):
    v = boot_vars(cfg)
    args = v["boot-args"].split()
    if op == "add":
        if arg in args:
            return f"boot-args already has {arg}: {v['boot-args']}"
        args.append(arg)
    else:
        if arg not in args:
            return f"boot-args has no {arg}: {v['boot-args']}"
        args = [a for a in args if a != arg]
    v["boot-args"] = " ".join(args)
    return f"boot-args now: {v['boot-args']}"


DEFAULTS_H = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "src", "common", "rb_defaults.h")


def default_tokens(path=DEFAULTS_H):
    """The default-on tokens, read out of src/common/rb_defaults.h.

    Read, not copied: the header is the list the kext compiles against and the
    hosttest checks, so a second copy here could only ever disagree with it.
    """
    with open(path) as f:
        src = f.read()
    try:
        body = src.split("rb_default_on_tokens[] = {", 1)[1].split("};", 1)[0]
    except IndexError:
        raise ConfigError(f"{path}: no rb_default_on_tokens[] table")
    toks = re.findall(r'"(-rbgpu[A-Za-z0-9]+)"', body)
    if len(toks) < 40:
        raise ConfigError(f"{path}: only {len(toks)} tokens parsed; refusing to "
                          f"strip boot-args from a table that looks wrong")
    return toks


def bootarg_strip(cfg):
    """Take the now-redundant tokens of ours out of boot-args (2xw).

    Since 2xw every token in `rb_defaults.h` is the default, so one in
    boot-args changes nothing - and a redundant token is still a token that can
    be mistyped, half-removed, or read as evidence that a feature needs it.

    What it removes is EXACTLY that table, matched by name, which is why the
    table is read from the header rather than restated here. Everything else
    stays, and the three classes that matters for are worth naming:

      * `wdt=-1` and the rest of the boot-args that are not ours.
        `-rbgpuforceenable` is default-on now and that watchdog arg is Apple's,
        so we cannot set it from a kext, and without it a frozen boot panics at
        120 s with no SSH window (2hh).
      * every DISARM: `-rbgpunofbhubp`, `-rbgpustock`, `-rbgpuoff`, and - the
        case a name-shaped rule gets wrong - `-rbgpugates` and `-rbgpucgcg`,
        which are the positive forms that turn the two already-negative
        defaults off. Silently undoing a disarm would be the opposite of a
        cleanup.
      * tunables with a value (`rbgpuschedtickus=2000`). Those are deliberate,
        so they are reported and left alone.
    """
    v = boot_vars(cfg)
    table = set(default_tokens())
    kept, gone, left = [], [], []
    for a in v["boot-args"].split():
        if a in table:
            gone.append(a)
            continue
        kept.append(a)
        name = a.split("=", 1)[0]
        if name.startswith("-rbgpu") or name.startswith("rbgpu"):
            left.append(a)
    v["boot-args"] = " ".join(kept)
    out = []
    if gone:
        out.append(f"boot-args: removed {len(gone)} redundant token(s): {' '.join(gone)}")
    else:
        out.append("boot-args: nothing of ours left to remove")
    if left:
        out.append("KEPT, because each one still means something - check them: "
                   + " ".join(left))
    out.append(f"boot-args now: {v['boot-args']}")
    return "\n".join(out)


def sysreport(cfg, on):
    try:
        dbg = cfg["Misc"]["Debug"]
    except (KeyError, TypeError):
        raise ConfigError("no Misc -> Debug")
    if not isinstance(dbg, dict) or not isinstance(dbg.get("SysReport"), bool):
        raise ConfigError("no Misc -> Debug -> SysReport boolean")
    dbg["SysReport"] = on
    return f"Misc -> Debug -> SysReport = {on}"


def status(cfg):
    out = []
    lst = kext_list(cfg)
    hits = find(lst, BUNDLE)
    lilu = find(lst, "Lilu.kext")
    if not hits:
        out.append(f"{BUNDLE}: not in Kernel -> Add")
    for i in hits:
        e = lst[i]
        where = "after Lilu" if lilu and i > lilu[0] else "BEFORE Lilu (wrong order)"
        out.append(f"{BUNDLE}: index {i}, Enabled={e.get('Enabled')}, {where}")
    v = boot_vars(cfg)
    out.append(f"boot-args: {v['boot-args']}")
    sr = cfg.get("Misc", {}).get("Debug", {}).get("SysReport")
    out.append(f"SysReport: {sr}")
    deletes = cfg.get("NVRAM", {}).get("Delete", {}).get(BOOT_GUID, [])
    if "boot-args" not in deletes:
        out.append("WARNING: NVRAM -> Delete does not list boot-args, so boot-args edits here "
                   "may not reach the Mac until NVRAM is reset")
    return "\n".join(out)


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    path, cmd = argv[1], argv[2]
    with open(path, "rb") as f:
        cfg = plistlib.load(f)
    try:
        if cmd == "status":
            print(status(cfg))
            return 0
        if cmd == "add-kext":
            msg = add_kext(cfg)
        elif cmd == "remove-kext":
            msg = remove_kext(cfg)
        elif cmd == "sysreport" and len(argv) == 4 and argv[3] in ("on", "off"):
            msg = sysreport(cfg, argv[3] == "on")
        elif cmd == "bootarg" and len(argv) == 5 and argv[3] in ("add", "remove"):
            msg = bootarg(cfg, argv[3], argv[4])
        elif cmd == "bootarg" and len(argv) == 4 and argv[3] == "strip":
            msg = bootarg_strip(cfg)
        else:
            print(__doc__, file=sys.stderr)
            return 2
    except ConfigError as e:
        print(f"config.plist: {e}; nothing changed", file=sys.stderr)
        return 1
    tmp = path + ".rbgpu-new"
    with open(tmp, "wb") as f:
        plistlib.dump(cfg, f, fmt=plistlib.FMT_XML, sort_keys=False)
        f.flush()
        os.fsync(f.fileno())
    with open(tmp, "rb") as f:
        if plistlib.load(f) != cfg:
            os.remove(tmp)
            print("config.plist: read-back mismatch; nothing changed", file=sys.stderr)
            return 1
    os.replace(tmp, path)
    print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
