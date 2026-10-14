#!/usr/bin/env bash
# Slice/convert the golden FDM cases with the engine inside a Trident image and record the results.
#
# Usage: TRIDENT_API=<trident>/api scripts/pseudorca/golden.sh <image> <outdir> [case-substring ...]
#
# TRIDENT_API is the api/ directory of a Trident checkout; its app/ and tests_slicer/ are mounted read-only over
# the image's own copy, so the baseline and the candidate are driven by the SAME Trident code and only the engine
# differs. Everything runs in a throwaway container with scratch state: the live Trident container and its /data are
# never involved. Compare two outputs with golden_compare.sh.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
IMAGE=${1:?usage: $0 <image> <outdir> [case ...]}
OUT=${2:?outdir}
shift 2
: "${TRIDENT_API:?set TRIDENT_API to the api/ directory of a Trident checkout}"
mkdir -p "$OUT"

docker run --rm \
  -v "$HERE:/golden:ro" \
  -v "$TRIDENT_API/app:/opt/api/.venv/lib/python3.12/site-packages/app:ro" \
  -v "$TRIDENT_API/tests_slicer:/tests_slicer:ro" \
  -v "$(cd "$OUT" && pwd):/out" \
  "$IMAGE" /opt/api/.venv/bin/python /golden/golden_cases.py /out "$@" 2>&1 \
  | grep -v -E "^> Warning|xkbcomp|XKEYBOARD"
exit "${PIPESTATUS[0]}"
