# wsread.py (2qg) - read, from the LIVE WindowServer, the four values the
# compositor decision actually uses.  Run via tools/wsread.sh; this file is
# exec'd inside lldb's Python.
#
# READ-ONLY BY CONSTRUCTION: every value below comes from process.ReadMemory.
# No expression is evaluated and no function is called in the target - calling
# a function in a stopped WindowServer can deadlock on a lock its own stopped
# thread holds, and that would freeze the desktop rather than pause it.
#
# Unslid addresses, each established earlier and cited:
#   0x7ff843c9ecdc  compositor MODE        (2qa; 0 metal, 1 sw, 2 noop)
#   0x7ff843ca08b0  Metal capability flag  (2pz/2qd)
#   0x7ff843c9ecd8  "acceleration disabled" byte, inverted (2qa)
#   0x7ff843bb1c08  CGXDisplayDevice list head (2qc, from CGXNextDisplayDevice)
#   0x7ff843bb2280  the indirect predicate CGXNextDisplayDevice consults first
#   0x7ff843bb20d0  the alternative (virtual-framebuffer) head accessor
#   0x7ff8404fc700  the MP ops table base (2qe, confirmed by three entries)
# The slide is MEASURED here from a symbol, never assumed (2ps).
import struct

UNSLID_NEXTDEV = 0x7ff8041229ef
G = {
    "mode  (0=metal 1=sw 2=noop)": (0x7ff843c9ecdc, 4),
    "metal capability flag":       (0x7ff843ca08b0, 1),
    "accel-DISABLED byte":         (0x7ff843c9ecd8, 1),
    "device list head ptr":        (0x7ff843bb1c08, 8),
    "list predicate fnptr":        (0x7ff843bb2280, 8),
    "alt(VFB) head accessor":      (0x7ff843bb20d0, 8),
}
MP_OPS_BASE = 0x7ff8404fc700

t = lldb.debugger.GetSelectedTarget()
p = t.GetProcess()
err = lldb.SBError()

def rd(addr, n):
    e = lldb.SBError()
    b = p.ReadMemory(addr, n, e)
    if not e.Success():
        return None
    return int.from_bytes(b, "little")

syms = t.FindSymbols("CGXNextDisplayDevice")
if not syms.GetSize():
    print("FAIL: CGXNextDisplayDevice not found - cannot measure the slide.")
else:
    live = syms.GetContextAtIndex(0).GetSymbol().GetStartAddress().GetLoadAddress(t)
    slide = live - UNSLID_NEXTDEV
    print("CGXNextDisplayDevice live   : 0x%x" % live)
    print("cache slide (measured)      : 0x%x" % slide)
    print()
    for name, (ua, n) in G.items():
        v = rd(ua + slide, n)
        print("  %-28s = %s" % (name, "UNREADABLE" if v is None else hex(v)))
    print()

    head = rd(0x7ff843bb1c08 + slide, 8)
    print("== the device the decision would walk ==")
    if not head:
        print("  list head is NULL - nothing to walk.")
    else:
        dev = head
        for i in range(4):
            if not dev:
                break
            ops  = rd(dev + 0x8, 8)
            priv = rd(dev + 0x160, 8)
            f64  = rd(dev + 0x64, 4)
            print("  device %d @ 0x%x" % (i, dev))
            if ops:
                off = ops - (MP_OPS_BASE + slide)
                print("    ops table      = 0x%x   %s" % (ops,
                      "== MP ops table" if off == 0 else
                      "NOT the MP table (delta %+d) - a VFB/other ops table" % off))
                a148 = rd(ops + 0x148, 8)
                print("    ops->0x148     = 0x%x  (the accelerator op)" % (a148 or 0))
            print("    dev->0x160 priv= 0x%x" % (priv or 0))
            if priv:
                print("      priv->0x58 service     = 0x%x" % (rd(priv+0x58,4) or 0))
                acc = rd(priv + 0x5c, 4)
                print("      priv->0x5c accelerator = 0x%x   %s" % (acc or 0,
                      "<<< ZERO: this IS why the compositor is CompositorSW"
                      if not acc else "<<< NON-ZERO"))
                print("      priv->0x60 unit        = 0x%x" % (rd(priv+0x60,4) or 0))
                print("      priv->0xb0             = 0x%08x (bit22=%d)" % (
                      (rd(priv+0xb0,4) or 0), ((rd(priv+0xb0,4) or 0) >> 22) & 1))
            if f64 is not None:
                print("    dev->0x64      = 0x%08x  isAcceleratorBacked(b22)=%d"
                      % (f64, (f64 >> 22) & 1))
            print("    dev->0x48 numDisplayBuffers      = %s" % (rd(dev+0x48,4),))
            print("    dev->0x14c displayPipeIsAvailable = %s" % (rd(dev+0x14c,1),))
            dev = rd(dev, 8)      # dev->0x00 is `next` (2qc)
    print()
    print("== how to read this ==")
    print("mode must be 0, the capability flag 1, the accel-DISABLED byte 0,")
    print("and priv->0x5c NON-ZERO for the desktop to composite on the GPU.")
    print("Whichever of those four is wrong is the defect; if all four look")
    print("right, then the device above is not the one CDDisplay holds and the")
    print("list predicate / ops table lines say so.")
