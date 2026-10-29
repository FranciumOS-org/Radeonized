#!/usr/bin/env python3
"""2x3: phase-trace the transaction counter, prompting the USER in real time.

The first attempt at this failed for a reason worth writing down: the trace
was started and THEN the instructions were sent, so the user read them after
the window had already closed and the capture contained none of the actions it
was built to measure. Rule 28's shape, applied to an experiment instead of a
log - the window and the event must be tied together at the source.

So this script owns the clock AND the prompting. It prints one short line when
each phase begins, deliberately NOT a live countdown: printing is itself screen
damage, which is the very thing being measured, so the instrument keeps its own
footprint to five lines in seventy-five seconds.

Reads only: ioreg, nothing else. No kext, no arming, no sudo.
"""
import subprocess, sys, time

PHASES = [
    ("do NOTHING - leave the keyboard alone", 15),
    ("TYPE characters continuously, as fast as you like", 15),
    ("do NOTHING again", 15),
    ("hold DELETE / backspace (make sure there IS text to delete)", 15),
    ("press ENTER a few times", 15),
]
KEYS = ("RBFBAmtrSets", "RBFBHubpWrites", "RBFBAmtrNoFlip", "RBFBAmtrPeak")

def read():
    out = subprocess.run(["ioreg", "-rl", "-c", "RBAMDFramebuffer"],
                         capture_output=True, text=True).stdout
    d = {}
    for ln in out.splitlines():
        for k in KEYS:
            if '"%s"' % k in ln:
                try: d[k] = int(ln.split("=")[1].strip())
                except ValueError: pass
    return d

def main():
    if not read():
        print("RBAMDFramebuffer not found - this must run in the ARMED boot.")
        return 2
    print("tracing 75 s. Watch the SCREEN and note when it updates.\n")
    t0 = time.time()
    samples, bounds, elapsed = [], [], 0.0
    for text, dur in PHASES:
        print(">>> %s" % text, flush=True)
        bounds.append((text, elapsed, elapsed + dur))
        end = elapsed + dur
        while time.time() - t0 < end:
            d = read()
            samples.append((time.time() - t0, d.get("RBFBAmtrSets", -1),
                            d.get("RBFBAmtrNoFlip", -1)))
            time.sleep(0.25)
        elapsed = end
    print("\n=== transactions per phase ===")
    print("%-62s %6s %8s" % ("phase", "txns", "per_s"))
    for text, a, b in bounds:
        sel = [s for s in samples if a <= s[0] < b]
        n = (sel[-1][1] - sel[0][1]) if len(sel) > 1 else 0
        print("%-62s %6d %8.2f" % (text[:62], n, n / (b - a)))
    tot = samples[-1][1] - samples[0][1]
    nf = samples[-1][2] - samples[0][2]
    print("\ntotal %d transactions in %.0f s; NoFlip (dropped by our gate) = %d"
          % (tot, samples[-1][0], nf))
    print("\nNow say, for each phase, whether the SCREEN updated.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
