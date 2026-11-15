#!/bin/sh
# 3dmc-b: fetch the DMCUB (display microcontroller) firmware image for this chip.
#
# WHY THIS IS A SEPARATE SCRIPT FROM fetch_linux_ref.sh. Three reasons, all of
# which would otherwise be silently wrong:
#
#   1. DIFFERENT LICENCE. fetch_linux_ref.sh takes GPL-2.0-only kernel headers
#      from kernel.org's linux.git. This takes a BINARY FIRMWARE BLOB from
#      linux-firmware.git, which is NOT GPL: amdgpu firmware is redistributable
#      only under LICENSE.amdgpu, which this repository does not carry and does
#      not grant. The blob is fetched to the gitignored firmware/ tree and is
#      NEVER committed and NEVER handed out, exactly as this board's own VBIOS
#      is not (3fw).
#   2. DIFFERENT NECESSITY. fetch_linux_ref.sh is required for every build - the
#      hosttest will not compile without it. This is required by nothing: it
#      exists only for the DMCUB image-replacement work (3dmc), so a clone that
#      never touches that line never needs to run it.
#   3. IT IS NOT REPRODUCIBLE FROM A BRANCH. 3dmc-b measured TWO DIFFERENT
#      BUILDS live: linux-firmware's current main is 236048 bytes with
#      ucode_version 0x04000050, and tag 20250917 is 234768 bytes with
#      0x04000045. An unpinned fetch would silently change which firmware this
#      project is reasoning about, which for a microcontroller image is the
#      whole experiment.
#
# Pinned by TAG and verified by DIGEST. The digest is the real guarantee; the
# tag only decides which build the digest is allowed to be.
#
#   sh tools/fetch_dmcub.sh
set -eu

TAG='20250917'
REL='amdgpu/yellow_carp_dmcub.bin'
SHA='e758135298cc2652403f3ddd7f5b99463a34036aac406fc93e21bdaa3d54be3f'

# For the record, NOT fetched by default: linux-firmware main as of 2026-11-15
# was sha256 8d546bbbdcec355625dfb4bb4baef78841793c9c0209be42df5305855094f4eb,
# 236048 bytes, ucode_version 0x04000050 (revision 80) against this tag's
# revision 69. Both are ip_version 3.1 and dal_fw 1. If the newer build is ever
# wanted, change all three variables together, never just the tag.

DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/firmware/linux"
DST="${DIR}/yellow_carp_dmcub.bin"

mkdir -p "$DIR"
if [ -f "$DST" ] && [ "$(shasum -a 256 "$DST" | cut -d' ' -f1)" = "$SHA" ]; then
    echo "  HAVE ${DST}"
    exit 0
fi

URL="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/${REL}?h=${TAG}"
TMP="${DST}.part"
curl -fsSL "$URL" -o "$TMP" || { rm -f "$TMP"; echo "${REL}: fetch failed" >&2; exit 1; }

# A 404 from this host returns an HTML error page with a 200-ish body on some
# paths - 3dmc-b hit exactly that, a 3006-byte "image" - so check the digest
# BEFORE believing the file, and say what was got.
GOT="$(shasum -a 256 "$TMP" | cut -d' ' -f1)"
if [ "$GOT" != "$SHA" ]; then
    SZ="$(wc -c < "$TMP" | tr -d ' ')"
    rm -f "$TMP"
    echo "${REL}: sha256 ${GOT} (${SZ} bytes), expected ${SHA} for tag ${TAG}" >&2
    exit 1
fi
mv "$TMP" "$DST"
echo "  GET  ${DST} (linux-firmware ${TAG}, LICENSE.amdgpu - DO NOT REDISTRIBUTE)"
