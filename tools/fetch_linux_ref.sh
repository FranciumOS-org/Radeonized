#!/bin/sh
# 3fw: fetch the Linux headers the build needs.
#
# 3dcn added two more: `dcn_3_0_2_offset.h` and `dcn_3_1_2_offset.h`, which
# `tools/re/dcn_remap_gen.py` turns into src/common/rb_dcn_remap.h - the
# DCN 3.0.2 -> 3.1.2 address table Apple's Navi 23 DAL is driven through. They
# are PINNED to a tag like the first one, because an unpinned fetch would make
# the generated table depend on whatever upstream happened to say that day,
# and a register address is exactly the kind of thing that must be
# reproducible from the tree alone.
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
DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/third_party/linux-ref"

# relative-path<TAB>sha256, one per line.
FILES="drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu13_driver_if_yellow_carp.h 29efcd87c38c67cac6fdbdc5ddefbedccc928e2514b3bce14b41ff4be04eb294
drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_0_2_offset.h 183f24be2a7888c83464e379ce0087761c0ee299dec4c29a9a365bcb494e9449
drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_1_2_offset.h f6417efca7a1cbedcb1e65bb739b9afa1dbbbc54f91868184fbb06863624b145"

mkdir -p "$DIR"
echo "$FILES" | while read -r REL SHA; do
    [ -n "$REL" ] || continue
    DST="${DIR}/$(basename "$REL")"
    if [ -f "$DST" ] && [ "$(shasum -a 256 "$DST" | cut -d' ' -f1)" = "$SHA" ]; then
        echo "  HAVE ${DST}"
        continue
    fi
    URL="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/${REL}?h=${TAG}"
    TMP="${DST}.part"
    curl -fsSL "$URL" -o "$TMP" || { rm -f "$TMP"; echo "${REL}: fetch failed" >&2; exit 1; }
    GOT="$(shasum -a 256 "$TMP" | cut -d' ' -f1)"
    if [ "$GOT" != "$SHA" ]; then
        rm -f "$TMP"
        echo "${REL}: sha256 ${GOT}, expected ${SHA} for ${TAG}" >&2
        exit 1
    fi
    mv "$TMP" "$DST"
    echo "  GET  ${DST} (linux ${TAG}, GPL-2.0-only)"
done
