#!/bin/sh
# 3fw: fetch the one Linux header the build needs.
#
# tools/hosttest/smu13_test.c checks src/common/rb_smu13.c against upstream's
# own PPSMC struct definitions, so it includes the kernel header unmodified
# rather than a transcription. That header is GPL-2.0-only and this repository
# does not redistribute it: it is fetched here, into third_party/, pinned to
# the revision the tests were written against and verified by digest.
#
#   sh tools/fetch_linux_ref.sh
set -eu

TAG='v6.6'
REL='drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu13_driver_if_yellow_carp.h'
URL="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/${REL}?h=${TAG}"
SHA='29efcd87c38c67cac6fdbdc5ddefbedccc928e2514b3bce14b41ff4be04eb294'

DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/third_party/linux-ref"
DST="${DIR}/smu13_driver_if_yellow_carp.h"

if [ -f "$DST" ] && [ "$(shasum -a 256 "$DST" | cut -d' ' -f1)" = "$SHA" ]; then
    echo "  HAVE ${DST}"
    exit 0
fi

mkdir -p "$DIR"
TMP="${DST}.part"
trap 'rm -f "$TMP"' EXIT INT TERM
curl -fsSL "$URL" -o "$TMP"

GOT="$(shasum -a 256 "$TMP" | cut -d' ' -f1)"
if [ "$GOT" != "$SHA" ]; then
    echo "${REL}: sha256 ${GOT}, expected ${SHA} for ${TAG}" >&2
    exit 1
fi

mv "$TMP" "$DST"
trap - EXIT INT TERM
echo "  GET  ${DST} (linux ${TAG}, GPL-2.0-only)"
