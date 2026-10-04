#!/usr/bin/env python3
"""Resolve every symbol the kext imports against its OSBundleLibraries on this
machine (kmutil libraries --undef-symbols) and fail if any is left over.

Usage: python3 tools/check_imports.py build/out/RembrandtProbe.kext
"""
import subprocess
import sys


def main():
    bundle = sys.argv[1]
    r = subprocess.run(["kmutil", "libraries", "-p", bundle, "--undef-symbols"],
                       capture_output=True, text=True)
    lines = [l.strip() for l in (r.stdout + r.stderr).splitlines()]
    syms = [l for l in lines if l.startswith("_")]
    unresolved = [l for l in syms if " in /" not in l]
    libs = sorted({l.rsplit(": ", 1)[1] for l in syms if ": " in l})
    if r.returncode != 0 or unresolved:
        print("  IMPORTS unresolved:\n    " + "\n    ".join(unresolved or lines))
        return 1
    print(f"  IMPORTS {len(syms)} symbols resolve via {', '.join(libs)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
