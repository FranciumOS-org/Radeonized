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
echo "== did the experiment run? dal_latch 0 or 2 = ran, 1 or 3 = DECLINED =="
ioreg -rl -c RembrandtGPUPeer 2>/dev/null \
  | tr ',' '\n' \
  | grep -oE '"(dal_latch|dal_on|dal_pass|dal_remap|dal_remap_refused|dal_pass_refused)"=[0-9]+' \
  | sort -u | sed 's/^/  /'

echo
echo "== the units question: dal_untr MUST be 0, or the table is misapplied =="
echo "== and the three-outcome counters =="
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
echo "== who owns the panel =="
ioreg -rl -c IOFramebuffer 2>/dev/null | grep -oE '^[ |+-]*\+-o [A-Za-z0-9_]+' | sed 's/^/  /'
