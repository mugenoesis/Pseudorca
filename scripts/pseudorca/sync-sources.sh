#!/usr/bin/env bash
# Bring Pseudorca up to date with its sources and rebuild the integration tree.
#
# Usage: scripts/pseudorca/sync-sources.sh [--push-topics] [--base <ref>]
#
# 1. fetch OrcaSlicer main and fast-forward our `main` mirror ref (local origin/main is updated by
#    `gh repo sync mugenoesis/Pseudorca --branch main`; this script only reports when it is behind)
# 2. merge origin/main into each topic branch, in a scratch worktree (git rerere replays known resolutions)
# 3. merge the topics into a scratch branch `integration/<orca-sha>` in the order below
# 4. rewrite PSEUDORCA_SOURCES for the new state
#
# It never touches headless-orca or pseudorca-next and pushes nothing unless --push-topics is given (then only the
# topic branches). The integration branch is then built and gated with build-candidate.sh / gate.sh, and merged
# into pseudorca-next by a PR whose tree is the integration's tree (see scripts/pseudorca/README.md).
# Stops at the first conflict and says which topic and files; fix them in the printed worktree, commit, rerun.
set -euo pipefail

TOPICS=(topic/build topic/cli topic/belt-fixes)   # merge order; topic/resin is appended once it exists
PUSH=0
BASE=origin/main
while [ $# -gt 0 ]; do
  case $1 in
    --push-topics) PUSH=1 ;;
    --base) BASE=$2; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

REPO=$(git rev-parse --show-toplevel)
WORK=${SYNC_WORK:-$REPO/../sync-work}
mkdir -p "$WORK"
cd "$REPO"
git config rerere.enabled true

git remote get-url orca >/dev/null 2>&1 || git remote add orca https://github.com/OrcaSlicer/OrcaSlicer.git
git fetch -q orca main
git fetch -q origin
ORCA=$(git rev-parse orca/main)
if [ "$(git rev-parse "$BASE")" != "$ORCA" ]; then
  echo "origin/main is behind OrcaSlicer main. Run: gh repo sync mugenoesis/Pseudorca --branch main   (then rerun)"
  exit 1
fi
if git ls-remote --exit-code origin refs/heads/topic/resin >/dev/null 2>&1; then TOPICS+=(topic/resin); fi
SHORT=$(git rev-parse --short=10 "$ORCA")

merge_in() {   # merge_in <worktree> <ref> <what>
  if ! git -C "$1" merge --no-edit "$2" >/dev/null 2>&1; then
    echo "CONFLICT merging $3 in $1:"; git -C "$1" diff --name-only --diff-filter=U | sed 's/^/  /'
    echo "Resolve them there, commit, and rerun (rerere remembers the resolution)."
    exit 1
  fi
}

for t in "${TOPICS[@]}"; do
  wt="$WORK/$(echo "$t" | tr / -)"
  [ -d "$wt" ] || git worktree add -q --detach "$wt" "origin/$t"
  git -C "$wt" checkout -q --detach "origin/$t"
  merge_in "$wt" "$BASE" "origin/main into $t"
  echo "$t: up to date with main ($(git -C "$wt" rev-parse --short HEAD))"
  git -C "$REPO" branch -f "sync/$t" "$(git -C "$wt" rev-parse HEAD)"
done

INTEG="$WORK/integ"
[ -d "$INTEG" ] || git worktree add -q --detach "$INTEG" "$BASE"
git -C "$INTEG" checkout -q --detach "$BASE"
for t in "${TOPICS[@]}"; do merge_in "$INTEG" "sync/$t" "$t into the integration"; done
git -C "$REPO" branch -f "integration/$SHORT" "$(git -C "$INTEG" rev-parse HEAD)"
echo "integration: integration/$SHORT = $(git -C "$INTEG" rev-parse --short HEAD) (worktree $INTEG)"

if [ "$PUSH" = 1 ]; then
  for t in "${TOPICS[@]}"; do git push origin "sync/$t:$t"; done
fi

# Record the state. prusa-sla is only changed by hand when an SLA port lands.
SRC="$INTEG/PSEUDORCA_SOURCES"
PRUSA=$(sed -n 's/^prusa-sla *\([^ ]*\).*/\1/p' "$REPO/PSEUDORCA_SOURCES" 2>/dev/null || true)
{
  echo "orca-main      $SHORT   $(git log -1 --format=%cs "$ORCA")"
  echo "prusa-sla      ${PRUSA:-none}"
  for t in "${TOPICS[@]}"; do printf '%-14s %s\n' "${t#topic/}" "$(git rev-parse "sync/$t")"; done
} > "$SRC"
git -C "$INTEG" add PSEUDORCA_SOURCES
git -C "$INTEG" commit -q -m "Record sources: OrcaSlicer main $SHORT" && git -C "$REPO" branch -f "integration/$SHORT" "$(git -C "$INTEG" rev-parse HEAD)"
cat <<MSG

Next: build and gate integration/$SHORT (sync-builder.sh, build-candidate.sh, golden.sh, golden_compare.sh against an
upstream-only build, unit-tests.sh), then open a PR into pseudorca-next whose tree is the integration's tree.
MSG
