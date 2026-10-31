#!/usr/bin/env python3
"""watts.py [secs] - whole-system power draw, measured as ENERGY over time.

No kext, no sudo, no SMU. Works at 100 % charge. Reads on battery only.

WHY NOT InstantAmperage. The first two versions of this tool multiplied
AppleSmartBattery's InstantAmperage by Voltage. That field is not a power
sensor: across three runs on one machine it reported 24.75 W, ~27 W and then
2.33 W, the last frozen at the identical value for 40 consecutive seconds with
a leading sample that showed CHARGING while on battery. 2.33 W cannot run this
laptop. The field is filtered and goes stale, so a tight sampling loop reports
the same number many times and calls it many samples.

WHAT THIS DOES INSTEAD. AppleRawCurrentCapacity is in mAh and falls as the
pack drains. Energy drained over a measured wall interval is average power,
and a counter that must decrease cannot be faked by a stale read - if it has
not moved, this says so instead of dividing by it (rule 31: a counter that has
not moved YET is not a counter that did not move).

    mean W = dmAh * V / 1000 / (dt_hours)

THE COST IS TIME. Capacity has 1 mAh resolution, so a short window has poor
resolution: at ~15 W and 16.3 V the pack drains ~920 mA, i.e. ~15 mAh per
minute. One minute is +-7 %, five minutes is +-1.5 %. It REFUSES to report a
figure until capacity has actually moved, and prints the resolution it
achieved so the number is never better than it reads.

AND THE ARITHMETIC IS IN EXACT INTEGERS. A discharge arrives as a signed value
in an unsigned 64-bit field: -242 mA reads as 18446744073709551374. The first
version folded that in awk, whose doubles round it to exactly 2^64, so
`a - 2^64` was exactly 0 and it printed "0.00 W" over a real draw - a broken
instrument reporting the very absence it was written to refuse. Python ints
are arbitrary precision; awk must never touch this field.
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
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    first = sample()
    if first["ExternalConnected"] == "Yes":
        sys.stderr.write(
            "watts.py: adapter connected - nothing flows through the gauge and\n"
            "          a full pack reads 0, which is not 0 W. Unplug, re-run.\n")
        return 1
    cap0 = first["AppleRawCurrentCapacity"]
    if cap0 is None:
        sys.stderr.write("watts.py: no AppleRawCurrentCapacity - cannot measure.\n")
        return 1

    print("measuring %ds by capacity drain (1 mAh resolution; longer is better)..."
          % secs)
    t0 = time.time()
    last_report = t0
    while True:
        dt = time.time() - t0
        if dt >= secs:
            break
        time.sleep(5)
        s_now = sample()
        cap = s_now["AppleRawCurrentCapacity"]
        v = s_now["Voltage"]
        if s_now["ExternalConnected"] == "Yes":
            sys.stderr.write("\nwatts.py: adapter reconnected mid-run - result discarded.\n")
            return 1
        if cap is None or v is None:
            continue
        dmah = cap0 - cap
        dt = time.time() - t0
        if time.time() - last_report >= 15:
            last_report = time.time()
            if dmah > 0:
                w = dmah * v / 1000.0 / (dt / 3600.0)
                print("  t+%4ds  %5d mAh drained  -> %6.2f W  (+-%.0f%%)"
                      % (int(dt), dmah, w, 100.0 / dmah))
            else:
                print("  t+%4ds  capacity has not moved yet (%d mAh)" % (int(dt), cap))

    s_end = sample()
    cap1 = s_end["AppleRawCurrentCapacity"]
    v = s_end["Voltage"]
    dt = time.time() - t0
    dmah = cap0 - cap1 if cap1 is not None else 0
    if dmah <= 0:
        sys.stderr.write(
            "\nwatts.py: capacity did not drop over %ds (%d -> %s mAh).\n"
            "          That is not a measurement of 0 W - run longer.\n"
            % (int(dt), cap0, cap1))
        return 1
    w = dmah * v / 1000.0 / (dt / 3600.0)
    print("\n%.2f W mean over %.0fs   (%d mAh at %d mV, resolution +-%.0f%%)"
          % (w, dt, dmah, v, 100.0 / dmah))
    # Advisory only, and labelled: see the header on why this field is not trusted.
    a = signed64(s_end["InstantAmperage"])
    if a is not None:
        print("InstantAmperage says %.2f W - ADVISORY ONLY, this field is "
              "filtered and goes stale." % ((-a) * v / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
