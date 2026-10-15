#!/usr/bin/env bash
# Sync this checkout's tracked files into an incremental-build container and verify them by checksum.
#
# Usage: scripts/pseudorca/sync-builder.sh <container> [source-dir=.]
#
# <container> is a running container made from the headless-orca builder image, with the source at
# /src/orcaslicer (e.g. `docker run -d --name pseudorca-incr --entrypoint sleep headless-orca:builder infinity`).
# The builder image can be STALE against the commit you want to test, so this compares every tracked file by
# sha256, copies only the ones that differ (stamped with the current time, so ninja rebuilds them even when the
# file is older than the object), and re-checks everything afterwards. Files that were deleted in the checkout
# are not removed from the container (CMake lists decide what gets compiled).
set -euo pipefail

CONTAINER=${1:?usage: $0 <container> [source-dir]}
SRC=$(cd "${2:-.}" && pwd)
DEST=/src/orcaslicer
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cd "$SRC"
# Regular files only (mode 100644/100755): symlinks and gitlinks cannot be checksummed.
git ls-files -s -z | awk -v RS="\0" -v ORS="\0" '$1 ~ /^100/ { sub(/^[^\t]*\t/, ""); print }' | xargs -0 sha256sum > "$TMP/host.sha"
docker cp "$TMP/host.sha" "$CONTAINER:/tmp/host.sha"
# `sha256sum -c` reports "<path>: FAILED" for changed files and "<path>: FAILED open or read" for missing ones.
docker exec -w "$DEST" "$CONTAINER" sh -c 'sha256sum -c /tmp/host.sha 2>&1 | grep -v ": OK$" || true' \
  | sed -n 's/: FAILED.*$//p' | sort -u > "$TMP/changed.txt"

COUNT=$(wc -l < "$TMP/changed.txt")
echo "sync-builder: $COUNT file(s) differ from the builder"
if [ "$COUNT" -gt 0 ]; then
  tr '\n' '\0' < "$TMP/changed.txt" | tar --null -T - -c | docker exec -i "$CONTAINER" tar -x -m -C "$DEST"
fi

BAD=$(docker exec -w "$DEST" "$CONTAINER" sh -c 'sha256sum -c /tmp/host.sha 2>&1 | grep -v ": OK$" | wc -l')
if [ "$BAD" != "0" ]; then
  echo "sync-builder: $BAD file(s) still differ after the copy" >&2
  exit 1
fi
echo "sync-builder: builder source matches $(git rev-parse --short HEAD) ($(wc -l < "$TMP/host.sha") tracked files verified)"
