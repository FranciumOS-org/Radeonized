#!/bin/sh
# RECOVERY ONLY. The `on` path of this script has been REMOVED.
#
# What it used to do, and what that cost (2026-10-21, logged as 2mq):
#   `launchctl setenv DYLD_INSERT_LIBRARIES <dylib>` is NOT a per-app switch. It
#   applies to every GUI process started afterwards. dyld KILLS a process that
#   cannot load an inserted dylib, and a sandboxed app (Mail, most App Store
#   apps) cannot read one out of a home directory - so it dies before any of our
#   code runs. the assistant would not open, then Terminal would not open, then Mail
#   would not open, and the machine had to be rebooted to get a usable GUI back.
#
# And the escape hatch did not work: `launchctl unsetenv` over SSH acts on a
# DIFFERENT launchd domain than the Aqua session, so it unsets nothing. To reach
# the GUI session from SSH or a non-GUI shell you must say so explicitly:
#
#   sudo launchctl asuser $(id -u) launchctl unsetenv DYLD_INSERT_LIBRARIES
#
# Rule: never put DYLD_INSERT_LIBRARIES in a launchd environment. Inject per
# process, where the blast radius is one process you started on purpose.
set -e
case "${1:-}" in
off)
    launchctl unsetenv DYLD_INSERT_LIBRARIES   || true
    launchctl unsetenv RBGPU_IOSURFTEX_LOG     || true
    launchctl unsetenv RBGPU_IOSURFTEX_LOGFILE || true
    launchctl unsetenv RBGPU_IOSURFTEX_ANYUSAGE || true
    echo "unset in THIS session's launchd domain."
    echo "If you are over SSH this did nothing to the GUI session. Use:"
    echo "  sudo launchctl asuser \$(id -u) launchctl unsetenv DYLD_INSERT_LIBRARIES"
    ;;
status)
    v=$(launchctl getenv DYLD_INSERT_LIBRARIES || true)
    [ -n "$v" ] && echo "DYLD_INSERT_LIBRARIES=$v" || echo "not set in this domain"
    ;;
on)
    echo "REFUSED. This broke every newly launched app and cost a reboot." >&2
    echo "See the comment at the top of this file, and docs/JOURNAL.md 2mq." >&2
    exit 1 ;;
*)
    sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
esac
