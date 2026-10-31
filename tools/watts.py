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

LOG = "logs/watts.log"

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


def power_w(dmah, mv, secs):
    """Average watts from charge drained over an interval.

    THE UNITS, SPELLED OUT, because getting them wrong is how this tool
    reported 17915 W for a real 17.93 W draw: mAh * mV is micro-watt-hours
    (1e-3 A.h * 1e-3 V), so the divisor is 1e6 and not 1e3. selftest() pins a
    measured case so the constant cannot drift again.
    """
    if secs <= 0:
        return 0.0
    return (dmah * mv / 1e6) / (secs / 3600.0)


def selftest():
    """Boot 260's own reading: 93 mAh at 16117 mV over 301 s was 17.93 W."""
    cases = [
        (93, 16117, 301, 17.93),
        (18, 16200, 45, 23.33),    # a plateau from the same run
        (0, 16000, 60, 0.0),
    ]
    bad = 0
    for dmah, mv, secs, want in cases:
        got = power_w(dmah, mv, secs)
        ok = abs(got - want) < 0.05
        print("%s %4d mAh %5d mV %4ds -> %8.2f W (want %.2f)"
              % ("ok  " if ok else "FAIL", dmah, mv, secs, got, want))
        bad += 0 if ok else 1
    # a sanity bound no laptop violates: this is what would have caught 1e3
    w = power_w(93, 16117, 301)
    if not (1.0 < w < 200.0):
        print("FAIL implausible magnitude %.2f W - check the unit divisor" % w)
        bad += 1
    print("selftest: %s" % ("all passed" if bad == 0 else "%d FAILED" % bad))
    return 1 if bad else 0


def signed64(v):
    """Fold an unsigned 64-bit field back to signed, exactly."""
    if v is None:
        return None
    return v - (1 << 64) if v >= (1 << 63) else v


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest()
    args = sys.argv[1:]
    label = ""
    if "--label" in args:
        i = args.index("--label")
        label = args[i + 1] if i + 1 < len(args) else ""
        del args[i:i + 2]
    secs = int(args[0]) if args else 300
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
                w = power_w(dmah, v, dt)
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
    w = power_w(dmah, v, dt)
    print("\n%.2f W mean over %.0fs   (%d mAh at %d mV, resolution +-%.0f%%)"
          % (w, dt, dmah, v, 100.0 / dmah))
    # Advisory only, and labelled: see the header on why this field is not trusted.
    a = signed64(s_end["InstantAmperage"])
    adv = (-a) * v / 1e6 if a is not None else float("nan")
    if a is not None:
        print("InstantAmperage says %.2f W - ADVISORY ONLY, this field is "
              "filtered and goes stale." % adv)

    # A run made with the the assistant app quit cannot be read back from its
    # terminal, so every result is appended here with the boot it came from.
    # Rule 33: a reading without the boot it belongs to is not evidence.
    try:
        boot = subprocess.run(["nvram", "rbgpu-boot"], capture_output=True,
                              text=True).stdout.split()[-1]
    except Exception:
        boot = "?"
    # 2z6: boot 260 read 17.93 W and boot 261 read 36.44 W on identical
    # binaries, and neither row recorded what the machine was DOING or which
    # timing it was running, so a 2x surprise could not be attributed. These
    # three turn that into a question with an answer. `armed` matters most:
    # 2z6's "stock" control was read off a boot that was never stock, and a
    # row that carries the arming cannot be mistaken for one that does not.
    env = {}
    try:
        env["load"] = "%.2f" % __import__("os").getloadavg()[0]
    except Exception:
        env["load"] = "?"
    env["mhz"] = "?"
    env["armed"] = "?"
    try:
        fb = subprocess.run(["ioreg", "-rl", "-c", "RBAMDFramebuffer"],
                            capture_output=True, text=True).stdout
        m = re.search(r'"RBFBModeCurMhz"\s*=\s*(\d+)', fb)
        if m:
            env["mhz"] = m.group(1)
    except Exception:
        pass
    try:
        pr = subprocess.run(["ioreg", "-rl", "-c", "RembrandtGPUPeer"],
                            capture_output=True, text=True).stdout
        m = re.search(r'"armed"=(\d+)', pr)
        if m:
            env["armed"] = m.group(1)
    except Exception:
        pass
    try:
        with open(LOG, "a") as f:
            f.write("%s boot=%s armed=%s mhz=%s load=%s label=%s  %.2f W  "
                    "(%d mAh @ %d mV over %.0fs, +-%.0f%%)  advisory=%.2f W\n"
                    % (time.strftime("%Y-%m-%dT%H:%M:%S"), boot,
                       env["armed"], env["mhz"], env["load"],
                       label or "-", w, dmah, v, dt, 100.0 / dmah, adv))
        print("appended to %s" % LOG)
    except OSError as e:
        print("could not write %s: %s" % (LOG, e))
    return 0


if __name__ == "__main__":
    sys.exit(main())
