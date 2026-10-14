#!/usr/bin/env bash
# Fail if any test that passed in the baseline results does not pass in the candidate results.
# Usage: scripts/pseudorca/unit-tests-compare.sh <baseline-results> <candidate-results>
set -euo pipefail
BASE=${1:?baseline results}
CAND=${2:?candidate results}
BAD=$(comm -23 <(grep ' Passed$' "$BASE" | sort) <(grep ' Passed$' "$CAND" | sort) || true)
if [ -n "$BAD" ]; then
  echo "unit tests that passed in the baseline but not in the candidate:"
  echo "  ${BAD//$'\n'/$'\n'  }"
  exit 1
fi
echo "unit tests: every baseline-passing test still passes ($(grep -c ' Passed$' "$CAND" || true) passing in the candidate)"
