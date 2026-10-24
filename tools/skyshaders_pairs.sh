#!/bin/bash
# Each named vertex/fragment pair from SkyLightShaders, one process each,
# because Metal aborts the process on a descriptor its validator rejects
# (2qb). An abort is reported as ABORT and is a statement about the
# DESCRIPTOR this tool built, not about the GPU.
cd "$(dirname "$0")/.."
B=build/out/skyshaders
[ -x "$B" ] || { echo "build it: make skyshaders"; exit 3; }
pairs=(
  "UberCompositeVertex UberCompositeFragment"
  "UberCompositeVertex UberResampleLanczosFragmentBGRA"
  "UberCompositeVertex UberResampleLanczosFragmentYCbCr"
  "BlurCompositeVertex BlurComposite"
  "SimpleColorVertex SimpleColorFragment"
  "SimpleVertex SimpleTextureFragment"
  "SimpleVertex AlphaTextureFragment"
  "SimpleVertex SimpleGrayscale"
  "SimpleVertex SimpleTextureTintFragment"
  "SimpleTextureLightingVertex SimpleTextureLightingFragment"
  "SimpleVertexShadow ShadowCompositeFragment"
  "SimpleVertexShadow ShadowVerticalBlurFragment"
  "vertexShaderWarp fragmentShaderWarp"
  "SimpleMeshVertex RippleFragment"
)
for fmt in bgra rgba16f; do
  echo "== $fmt"
  b=0; r=0; a=0
  for p in "${pairs[@]}"; do
    set -- $p
    out=$(env -u DYLD_INSERT_LIBRARIES "$B" --pair "$1" "$2" "$fmt" 2>&1)
    rc=$?
    case $rc in
      0) b=$((b+1)); printf "  built  %-28s + %s\n" "$1" "$2" ;;
      1) r=$((r+1)); printf "  REFUSE %-28s + %s\n         %s\n" "$1" "$2" "$out" ;;
      3) printf "  absent %-28s + %s\n" "$1" "$2" ;;
      *) a=$((a+1)); printf "  ABORT  %-28s + %s  (rc=%d, Metal validator)\n" "$1" "$2" "$rc" ;;
    esac
  done
  echo "  -> $b built, $r refused with an error, $a aborted the validator"
done
