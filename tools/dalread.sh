#!/bin/sh
# 3ret-c/3dcn-c: read the DAL bring-up boot. Run after the reboot.
#
# Order matters and is rule 8 and rule 33: establish WHICH boot this is and
# whether the experiment ran at all, BEFORE reading anything it produced. A
# declined experiment and an experiment that ran and saw nothing publish very
# different things and must not be confused (rule 14).
set -u
echo "== boot =="
sysctl -n kern.boottime
sw_vers -productVersion; sw_vers -buildVersion

echo
echo "== artifacts loaded (rule 15: check EVERY one) =="
kextstat 2>/dev/null | grep -iE 'rembrandt|lilu' | awk '{print "  " $6, $7, $8}'

echo
echo "== RESCUE TOKENS FIRST: either of these makes everything below empty =="
# 3dcn-d: this script did not check for these, and I then read a framebuffer
# log line naming them and asserted they were absent. -rbgpuoff was there.
# Rule 26: a search is an instrument, and an absence is only a reading once
# something has actually looked.
for t in -rbgpuoff -rbgpustock; do
    if sysctl -n kern.bootargs | tr ' \t' '\n\n' | grep -qx -- "$t"; then
        echo "  PRESENT  $t  <-- the plugin is inert; nothing below is evidence"
    else
        echo "  absent   $t"
    fi
done

echo
echo "== arms, as the KERNEL parsed them (rule 43: not nvram's stale copy) =="
for t in -rbgpuretirevbl -rbgpudal -rbgpudalpass rbgpudalenc=dword \
         -rbgpudalremap -rbgpudalfbreal -rbgpunosentinel; do
    if sysctl -n kern.bootargs | tr ' \t' '\n\n' | grep -qx -- "$t"; then
        echo "  PRESENT  $t"
    else
        echo "  absent   $t"
    fi
done

echo
# 3ret-d: the gate moved to the point of use, so the keys changed.
#   dal_stock    1 = this boot is stock, so the experiment was DECLINED
#   dal_declined 1 = the tokens were in boot-args AND the boot was stock
#   dal_asked    1 = the tokens were in boot-args at all
#   dal_latch    0 no latch, 1 LATCHED, 2 the peer never looked
# dal_latch 3 does not exist any more: it was 3ret-c reading /options from
# RGPU::init(), where it is not reachable, and declining every healthy boot.
echo "== did the experiment run? dal_stock 1 = DECLINED, dal_on 1 = routed =="
ioreg -rl -c RembrandtGPUPeer 2>/dev/null \
  | tr ',' '\n' \
  | grep -oE '"(dal_latch|dal_stock|dal_asked|dal_declined|dal_on|dal_pass|dal_remap|dal_remap_refused|dal_pass_refused|dal_wroutes|dal_srouted|dal_start|dal_start_ret|dalfb_srouted|dalfb_grouted|dalfb_start|dalfb_start_ret)"=[0-9]+' \
  | sort -u | sed 's/^/  /'

echo
echo "== the units question =="
echo "  dal_untr 0 means the table was applied cleanly - BUT ONLY IF"
echo "  dal_writes > 0. With dal_writes 0 nothing was ever classified and the"
echo "  0 carries NO INFORMATION (rule 48: a conjunction can be correct while"
echo "  one term says nothing). Boot 581 read exactly that: untr 0, writes 0."
echo "  The controller's start() is construction only and makes no register"
echo "  writes (boot 140, reproduced in 581), so the only configuration that"
echo "  offers writes to classify is the Wrangler promotion -rbgpudalfbreal,"
echo "  which PANICKED in 2pk and HUNG in 2pl and 2po."
echo "== the three-outcome counters =="
ioreg -rl -c RembrandtGPUPeer 2>/dev/null \
  | tr ',' '\n' \
  | grep -oE '"(dal_untr|dal_wrr|dal_rdr|dal_blk|dal_passed|dal_denied|dal_writes|dal_r_hw|dal_r_atom|dal_r_bios)"=[0-9]+' \
  | sort -u | sed 's/^/  /'

echo
echo "== frame delivery still healthy? (the 3ret-b fix is default-on now) =="
ioreg -rl -c RembrandtGPUPeer 2>/dev/null \
  | tr ',' '\n' \
  | grep -oE '"(vblret_armed|vblret_src|vblret_sub|vblret_vbl|vblret_timer|vblret_autodrv|vbl_mhz)"=[0-9]+' \
  | sort -u | sed 's/^/  /'

echo
echo "== display =="
system_profiler SPDisplaysDataType 2>/dev/null \
  | grep -iE 'Chipset|VRAM|Metal|Resolution|Display Type|Connection' | sed 's/^ */  /'

echo
echo "== who owns the panel, and which classes actually STARTED =="
# 3dcn-d: `ioreg -c <class>` does NOT filter on a miss - it prints the whole
# tree, identically for an absent class and a present one, which is rule 26's
# own listed trap. Grep the instance lines out of ioreg -l instead, and keep a
# nonsense class as the control so a row of zeroes can be told from a broken
# search.
_tree=$(ioreg -l 2>/dev/null)
# 3ret-d: THE POSITIVE CONTROL WAS WRONG. It asserted AppleDisplay >= 1, and
# AppleDisplay only attaches in the UNACCELERATED configuration - so on the
# accelerated boot this script exists to read, the control read 0 and declared
# its own search broken. Rule 39 applied to an instrument: a control has to
# hold in the configuration being measured. IOResources is in every boot that
# reaches userspace at all, which is a precondition of running this script.
for c in ThisClassDoesNotExistXYZ IOResources RembrandtGPUPeer \
         RBAMDFramebuffer \
         AMDRadeonX6000_AMDNavi21GraphicsAccelerator \
         AMDRadeonX6000_AmdGpuWrangler \
         AMDRadeonX6000_AmdRadeonControllerNavi21 \
         AMDRadeonX6000_AmdRadeonControllerNavi2 AppleDisplay; do
    printf '  %-46s %s\n' "$c" "$(printf '%s' "$_tree" | grep -c "+-o $c")"
done
echo "  (ThisClassDoesNotExistXYZ MUST be 0 and IOResources MUST be >=1, or"
echo "   this search is broken and the rows above say nothing. AppleDisplay is"
echo "   NOT a control: it attaches only when the panel is UNaccelerated.)"
echo
echo "  3ret-d: there is NO class called AmdRadeonControllerNavi25. The symbol"
echo "  __ZN39...AmdRadeonControllerNavi25startEP9IOService demangles to"
echo "  AmdRadeonControllerNavi2::start - 39 is the length of ...Navi2 and the"
echo "  5 belongs to \"start\". An earlier version of this script listed Navi25"
echo "  as a class, so it could only ever print 0, and that 0 was reported as"
echo "  \"the controller never started\". It is a FABRICATED CONTROL. Navi21 is"
echo "  the instance on this chip and it has no own start, so it inherits"
echo "  Navi2::start - which is what we route. The authoritative witness that"
echo "  the controller ran is dal_start / dal_start_ret, NOT a class row."
