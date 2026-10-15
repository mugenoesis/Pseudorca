#!/usr/bin/env bash
# Compare two golden.sh outputs (the host needs no Python: this runs golden_compare.py in a throwaway container).
#
# Usage: scripts/pseudorca/golden_compare.sh <baseline-dir> <candidate-dir> [--allow-new-cases] [--image <image>]
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BASE=$(cd "${1:?baseline dir}" && pwd)
CAND=$(cd "${2:?candidate dir}" && pwd)
shift 2
IMAGE=${GOLDEN_COMPARE_IMAGE:-mugenoesis/trident:latest}
exec docker run --rm --entrypoint /opt/api/.venv/bin/python \
  -v "$HERE:/golden:ro" -v "$BASE:/a:ro" -v "$CAND:/b:ro" \
  "$IMAGE" /golden/golden_compare.py /a /b "$@"
