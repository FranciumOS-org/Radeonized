#!/usr/bin/env python3
"""Edit an OpenCore config.plist for the RembrandtGPU plugin. Used by
tools/efi.sh; works on any path, so it can be tried on a copy first.

    python3 tools/efi_config.py CONFIG status
    python3 tools/efi_config.py CONFIG add-kext       # Kernel -> Add, right after Lilu
    python3 tools/efi_config.py CONFIG remove-kext
    python3 tools/efi_config.py CONFIG bootarg add -rbgpuoff
    python3 tools/efi_config.py CONFIG bootarg remove -rbgpuoff

Only these keys are touched: the RembrandtGPU entry of Kernel -> Add, and
boot-args under NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82.
Everything else is written back as read (plistlib keeps key order, data and
types). The new file is written next to the old one, read back, and only
then moved over it, so an error never leaves a half-written config.
"""
import os
import plistlib
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
        elif cmd == "bootarg" and len(argv) == 5 and argv[3] in ("add", "remove"):
            msg = bootarg(cfg, argv[3], argv[4])
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
