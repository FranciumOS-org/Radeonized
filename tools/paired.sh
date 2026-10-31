#!/bin/sh
# 2zb: pair the latch histogram with the frame rate over the SAME window on the
# SAME boot - the three ad-hoc mode deltas could not, since each spanned two
# modes (rule 39 in miniature).
#
# THE DIVISOR IS MEASURED, NOT ASSUMED. The first version of this script
# divided the counter deltas by animtest's --secs argument, and animtest runs
# ~20.3 s whatever you ask for, so every rate came out 1.353x high and the
# panel appeared to scan at 194 Hz. That is 2y9's error with the terms
# swapped. And time.monotonic() is PROCESS-RELATIVE in this python build -
# it returns 0.0088 in a fresh process - so the two samples must come from
# time.time(), which is absolute. So: time the run, and CHECK the result against the mode's own rate,
# because vbl/s is the one quantity whose true value we already know (rule 26
# - the instrument must say when its own premise has failed).
snap() {
  ioreg -rl -c RBAMDFramebuffer | awk -F'= ' '
    /RBFBHubpLatchHist/   { gsub(/"/,"",$2); split($2,a," ");
                            for (i in a) { split(a[i],b,":"); printf "h%d %s\n", i, b[2] } }
    /RBFBHubpLatchSamples/{ print "samples", $2 }
    /RBFBHubpLatched"/    { print "latched", $2 }
    /RBFBHubpLatchPreempted/ { print "preempt", $2 }
    /RBFBAmtrSets/        { print "sets", $2 }
    /RBFBVBLFires/        { print "vbl", $2 }
    /RBFBAmtrHeld/        { print "held", $2 }
    /RBFBAmtrGets/        { print "gets", $2 }
    /RBFBModeCurMhz/      { print "mhz", $2 }'
}
SECS=${1:-20}
snap > /tmp/.p0
T0=$(python3 -c 'import time;print(time.time())')
i=0; while [ $i -lt ${JOBS:-1} ]; do
  env -u DYLD_INSERT_LIBRARIES ./build/out/animtest --secs "$SECS" >/dev/null 2>&1 &
  i=$((i+1))
done; wait
ELAPSED=$(python3 -c "import time;print(time.time()-$T0)")
snap > /tmp/.p1
join /tmp/.p0 /tmp/.p1 | awk -v s="$ELAPSED" -v asked="$SECS" '
  { d[$1] = $3 - $2; v[$1] = $3 }
  END {
    hz = v["mhz"]/1000.0; vbls = d["vbl"]/s; n = d["h1"]+d["h2"]+d["h3"]+d["h4"]+d["h5"];
    printf "mode %.3f Hz   window %.2fs (asked %ss)\n", hz, s, asked;
    err = 100.0*(vbls - hz)/hz;
    if (err < -5.0 || err > 5.0) {
      printf "  REFUSED: vbl/s %.2f is %+.1f %% off the mode rate - the\n", vbls, err;
      printf "           divisor or the counter is wrong, so no rate here is usable.\n";
      exit 1;
    }
    printf "  latch hist   1:%d 2:%d 3:%d 4:%d 5+:%d   (n=%d, preempt +%d)\n",
           d["h1"], d["h2"], d["h3"], d["h4"], d["h5"], n, d["preempt"];
    if (n > 0) printf "  bin2+ share  %.1f %%\n", 100.0*(n - d["h1"])/n;
    if (d["latched"] > 0) printf "  mean vblanks %.3f\n", d["samples"]/d["latched"];
    printf "  sets/s       %.2f   vbl/s %.2f (%+.1f %%)   fed %.1f %%\n",
           d["sets"]/s, vbls, err, 100.0*(d["sets"]/s)/vbls;
    if (d["gets"] > 0) printf "  held/gets    %.1f %%  (%d of %d)\n",
           100.0*d["held"]/d["gets"], d["held"], d["gets"];
    if (d["latched"] > 0) printf "  latch ceiling %.1f /s   (actual is %.0f %% of it)\n",
           vbls/(d["samples"]/d["latched"]),
           100.0*(d["sets"]/s)/(vbls/(d["samples"]/d["latched"]));
  }'
