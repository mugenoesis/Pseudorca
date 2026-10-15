#!/usr/bin/env bash
# Build and run the engine's Catch2 suites in an incremental-build container, and write one line per test.
#
# Usage: scripts/pseudorca/unit-tests.sh <container> <results-file>
#
# <results-file> gets "<test name> <Passed|Failed|...>" per line, sorted, so a candidate can be compared with a
# baseline using unit-tests-compare (see gate.sh): every test the baseline passes must still pass.
# The container's source must already match the commit under test (sync-builder.sh does that).
set -euo pipefail

CONTAINER=${1:?usage: $0 <container> <results-file>}
RESULTS=${2:?results file}
JOBS=${BUILD_JOBS:-6}
TARGETS="libnest2d_tests libslic3r_tests fff_print_tests sla_print_tests filament_group_tests"

docker exec "$CONTAINER" bash -c "cd /src/orcaslicer && cmake -S . -B build -DBUILD_TESTS=ON >/dev/null && nice -n 10 cmake --build build --config Release --target $TARGETS -j$JOBS"
docker exec "$CONTAINER" bash -c "cd /src/orcaslicer/build && ctest -C Release -j4 --timeout 900 --exclude-regex 'RequiresApp' 2>&1" > "$RESULTS.log" || true
# "  1/12 Test  #1: name ..........   Passed    0.12 sec"
sed -nE 's/^ *[0-9]+\/[0-9]+ Test +#[0-9]+: (.*[^ .]) \.+ *(\*\*\*)?([A-Za-z ]+[A-Za-z]) +[0-9.]+ sec.*$/\1 \3/p' "$RESULTS.log" | sort > "$RESULTS"
echo "unit-tests: $(grep -c ' Passed$' "$RESULTS" || true) passed, $(grep -vc ' Passed$' "$RESULTS" || true) not passed ($RESULTS)"
