#!/usr/bin/env bash
# The no-regression gate: does an engine change leave everything Trident depends on unchanged?
#
# Usage: TRIDENT_API=<trident>/api BASELINE=<baseline-dir> \
#          scripts/pseudorca/gate.sh <container> <candidate-tag> [source-dir=.]
#
#   <container>      incremental-build container made from the headless-orca builder image
#   <candidate-tag>  tag for the candidate image (a Trident image with the new engine layered on)
#   BASELINE         directory holding golden/ (golden.sh output of the production engine) and unit-tests.txt
#   BASE_IMAGE       Trident image the candidate is layered on (default mugenoesis/trident:latest)
#   SKIP_UNIT=1      skip the C++ unit tests (they add several minutes)
#   SKIP_TRIDENT=1   skip Trident's own real-binary suite (api/tests_slicer, about four minutes)
#
# Steps: 1 build the candidate, 2 golden FDM outputs byte-compared with the baseline, 3 unit tests, 4 Trident's
# tests_slicer against the candidate, 5 `orca-slicer --help`. Everything runs in throwaway containers with scratch
# state; the live Trident container is never involved. Writes a markdown report to $REPORT_DIR (default ./gate-report)
# and exits non-zero if any step fails.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
CONTAINER=${1:?usage: $0 <container> <candidate-tag> [source-dir]}
TAG=${2:?candidate tag}
SRC=$(cd "${3:-.}" && pwd)
: "${TRIDENT_API:?set TRIDENT_API to the api/ directory of a Trident checkout}"
: "${BASELINE:?set BASELINE to the baseline directory}"
BASE_IMAGE=${BASE_IMAGE:-mugenoesis/trident:latest}
SHA=$(git -C "$SRC" rev-parse --short HEAD)
REPORT_DIR=${REPORT_DIR:-$PWD/gate-report}
mkdir -p "$REPORT_DIR"
REPORT="$REPORT_DIR/gate-$SHA.md"
FAILED=0

step() { echo; echo "=== $*"; }
record() { # <title> <status> <log-file>
  # shellcheck disable=SC2016 # the backticks are literal markdown fences
  printf '### %s: %s\n\n```\n%s\n```\n\n' "$1" "$2" "$(tail -n 40 "$3")" >> "$REPORT"
  [ "$2" = PASS ] || FAILED=1
}
run_step() { # <title> <log> <command...>
  local title=$1 log=$2
  shift 2
  step "$title"
  if "$@" > "$log" 2>&1; then record "$title" PASS "$log"; else record "$title" FAIL "$log"; fi
  tail -n 6 "$log"
}

printf '# Gate report for %s\n\nengine commit: %s\nbaseline: %s\ncandidate image: %s\n\n' \
  "$SHA" "$(git -C "$SRC" rev-parse HEAD)" "$BASELINE" "$TAG" > "$REPORT"

run_step "1 build candidate" "$REPORT_DIR/build.log" "$HERE/build-candidate.sh" "$CONTAINER" "$BASE_IMAGE" "$TAG" "$SRC"
if [ "$FAILED" != 0 ]; then echo "build failed; stopping"; echo "report: $REPORT"; exit 1; fi

step "2 golden FDM outputs"
rm -rf "$REPORT_DIR/golden-$SHA"
"$HERE/golden.sh" "$TAG" "$REPORT_DIR/golden-$SHA" > "$REPORT_DIR/golden.log" 2>&1
if "$HERE/golden_compare.sh" "$BASELINE/golden" "$REPORT_DIR/golden-$SHA" --allow-new-cases > "$REPORT_DIR/golden-compare.log" 2>&1; then
  record "2 golden FDM outputs" PASS "$REPORT_DIR/golden-compare.log"
else
  record "2 golden FDM outputs" FAIL "$REPORT_DIR/golden-compare.log"
fi
tail -n 4 "$REPORT_DIR/golden-compare.log"

if [ -z "${SKIP_UNIT:-}" ]; then
  run_step "3 unit tests" "$REPORT_DIR/unit.log" bash -c \
    "'$HERE/unit-tests.sh' '$CONTAINER' '$REPORT_DIR/unit-tests-$SHA.txt' && '$HERE/unit-tests-compare.sh' '$BASELINE/unit-tests.txt' '$REPORT_DIR/unit-tests-$SHA.txt'"
fi

if [ -z "${SKIP_TRIDENT:-}" ]; then
  run_step "4 Trident tests_slicer" "$REPORT_DIR/tests_slicer.log" docker run --rm --entrypoint sh \
    -v "$TRIDENT_API/app":/opt/api/.venv/lib/python3.12/site-packages/app:ro -v "$TRIDENT_API":/src:ro "$TAG" -c \
    'cp -r /src /work && cd /work && /opt/api/.venv/bin/python -m pip install -q pytest && /opt/api/.venv/bin/python -m pytest tests_slicer -q -p no:cacheprovider'
fi

run_step "5 orca-slicer --help" "$REPORT_DIR/help.log" docker run --rm "$TAG" orca-slicer --help

echo
if [ "$FAILED" = 0 ]; then echo "GATE PASSED ($SHA). Report: $REPORT"; else echo "GATE FAILED ($SHA). Report: $REPORT"; fi
exit "$FAILED"
