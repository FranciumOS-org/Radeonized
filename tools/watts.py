#!/usr/bin/env python3
"""watts.py [secs] - whole-system power draw from the battery gauge.

No kext, no sudo, no SMU. AppleSmartBattery publishes InstantAmperage (mA) and
Voltage (mV), and their product is what the machine is actually pulling. It is
the only wattmeter this project has that costs nothing, and it works at 100 %
charge.

IT ONLY READS ON BATTERY. With the adapter connected and the pack full,
amperage is 0 because nothing flows through the gauge - that is not a
measurement of 0 W (rule 14), so this refuses rather than printing a
comforting zero.

THE ARITHMETIC IS IN EXACT INTEGERS, DELIBERATELY. A discharge arrives as a
signed value carried in an unsigned 64-bit field: -242 mA reads as
18446744073709551374. The first version of this tool folded that in awk, whose
doubles round the value to exactly 2^64, so `a - 2^64` came out exactly 0 and
it printed "0.00 W" over a real 3.9 W draw - a broken instrument reporting the
very absence it was written to refuse. Python ints are arbitrary precision;
awk must never touch this field.
"""
import re
import subprocess
import sys
import time

FIELDS = ("InstantAmperage", "Amperage", "Voltage", "AppleRawCurrentCapacity")


def sample():
    out = subprocess.run(["ioreg", "-rl", "-c", "AppleSmartBattery"],
                         capture_output=True, text=True).stdout
    vals = {}
    for key in FIELDS:
        m = re.search(r'"%s"\s*=\s*(-?\d+)' % key, out)
        vals[key] = int(m.group(1)) if m else None
    m = re.search(r'"ExternalConnected"\s*=\s*(\w+)', out)
    vals["ExternalConnected"] = m.group(1) if m else None
    return vals


def signed64(v):
    """Fold an unsigned 64-bit field back to signed, exactly."""
    if v is None:
        return None
    return v - (1 << 64) if v >= (1 << 63) else v


def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    first = sample()
    if first["ExternalConnected"] == "Yes":
        sys.stderr.write(
            "watts.py: adapter connected - the gauge reads 0 and that is not 0 W.\n"
            "          Unplug, then re-run.\n")
        return 1

    print("sampling %ds on battery..." % secs)
    watts = []
    t0 = time.time()
    while time.time() - t0 < secs:
        s = sample()
        a = signed64(s["InstantAmperage"])
        v = s["Voltage"]
        if a is None or v is None or v <= 0:
            time.sleep(2)
            continue
        w = (-a) * v / 1e6
        watts.append(w)
        print("  t+%3ds  %6.2f W   (%d mA @ %d mV)"
              % (int(time.time() - t0), w, a, v))
        time.sleep(2)

    if not watts:
        sys.stderr.write("watts.py: no usable samples - gauge fields missing.\n")
        return 1
    o = sorted(watts)
    print("\nmean %.2f W   median %.2f W   min %.2f   max %.2f   (%d samples)"
          % (sum(o) / len(o), o[len(o) // 2], o[0], o[-1], len(o)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
