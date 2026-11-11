#!/bin/sh
# 3fxe: fetch the AMD firmware containers the plugin embeds.
#
# These are AMD microcode, redistributed under AMD's own licence
# (third_party/LICENSE.amdgpu, reproduced verbatim as that licence requires).
# This repository does NOT redistribute them: they are fetched here, from
# linux-firmware, and verified by digest. The plugin embeds each file WHOLE and
# byte-exact and slices it at runtime with src/common/rb_fwhdr.h, so every byte
# the built binary carries is a verbatim copy of an upstream file.
#
#   sh tools/fetch_amd_fw.sh
#
# A build with NOFW=1 needs none of this - but that build is not expected to
# accelerate: boot 542 measured the images consumed twelve times.
set -eu

PROJ='48890189'          # gitlab.com/kernel-firmware/linux-firmware
REF='main'

# name                                                        sha256
FILES='
ce:bd1fd7a357d7c9aa489469166435aa2d3e2ee1b324d4be35e9a648c794281674
pfp:b817f15a37ae080d5037a159732d60be24dda0bed74b6a3ba609d7c38f4d29a7
me:e2eeb8958a08231813943dabffd0e18ab65e7c0338a36155b9e6584ce55df31f
mec:9020014a34a9357b28058f4ecdf1f0d3ffbcac7265df04c544722ed5f7447e9b
rlc:bf41c78ecf4f3444ee14c917358485e55bfc5c496779821f1cad643b25a3da7f
sdma:be8cdc90616c6aa1e92cf1307e18515958a5c6b80552e20fa4452e2e61c18fd4
toc:9fc8da2f6106bf657222ca4bcca687e66e657e5710278c694d9d0e862221758d
'

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
DIR="${ROOT}/third_party/amd-fw"
mkdir -p "$DIR"

for ent in $FILES; do
    name="${ent%%:*}"
    sha="${ent#*:}"
    dst="${DIR}/yellow_carp_${name}.bin"

    if [ -f "$dst" ] && [ "$(shasum -a 256 "$dst" | cut -d' ' -f1)" = "$sha" ]; then
        echo "  HAVE yellow_carp_${name}.bin"
        continue
    fi

    url="https://gitlab.com/api/v4/projects/${PROJ}/repository/files/amdgpu%2Fyellow_carp_${name}.bin/raw?ref=${REF}"
    tmp="${dst}.part"
    curl -fsSL "$url" -o "$tmp"

    got="$(shasum -a 256 "$tmp" | cut -d' ' -f1)"
    if [ "$got" != "$sha" ]; then
        rm -f "$tmp"
        echo "yellow_carp_${name}.bin: sha256 ${got}, expected ${sha}" >&2
        echo "Upstream has changed this file. Do NOT re-pin without checking" >&2
        echo "that the new payload still matches tools/hosttest/fwhdr_test.c," >&2
        echo "which pins the bytes this driver was validated against." >&2
        exit 1
    fi

    mv "$tmp" "$dst"
    echo "  GET  yellow_carp_${name}.bin (AMD microcode, see third_party/LICENSE.amdgpu)"
done

# The licence must travel with anything built from these (its own condition).
cp "${ROOT}/third_party/LICENSE.amdgpu" "${DIR}/LICENSE.amdgpu"
echo "  OK   ${DIR}"
