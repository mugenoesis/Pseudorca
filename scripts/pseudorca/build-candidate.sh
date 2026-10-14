#!/usr/bin/env bash
# Build the engine incrementally in a builder container and layer it onto a Trident image as a "candidate".
#
# Usage: scripts/pseudorca/build-candidate.sh <container> <base-image> <candidate-tag> [source-dir=.]
#
# 1. sync-builder.sh makes the container's source match this checkout (verified by checksum).
# 2. ninja rebuilds what changed (the OrcaSlicer target).
# 3. build/src/Release/ and resources/ are layered over <base-image> (the runtime stage of docker/Dockerfile
#    copies exactly these two things), tagged <candidate-tag>.
# Final images that ship still come from docker/Dockerfile; this is the fast per-PR loop.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
CONTAINER=${1:?container}
BASE=${2:?base image}
TAG=${3:?candidate tag}
SRC=$(cd "${4:-.}" && pwd)
JOBS=${BUILD_JOBS:-6}

"$HERE/sync-builder.sh" "$CONTAINER" "$SRC"
docker exec "$CONTAINER" sh -c "cd /src/orcaslicer && nice -n 10 cmake --build build --config Release --target OrcaSlicer -j$JOBS"

CTX=$(mktemp -d)
trap 'rm -rf "$CTX"' EXIT
mkdir "$CTX/release"
docker cp "$CONTAINER:/src/orcaslicer/build/src/Release/." "$CTX/release"
cp -a "$SRC/resources" "$CTX/resources"
cat > "$CTX/Dockerfile" <<DOCKERFILE
FROM $BASE
COPY release/ /opt/orcaslicer/
COPY resources /opt/orcaslicer/resources
ENV ORCA_API_ORCASLICER_COMMIT_SHA=$(git -C "$SRC" rev-parse HEAD)
DOCKERFILE
docker build -q -t "$TAG" "$CTX"
echo "build-candidate: $TAG built from $(git -C "$SRC" rev-parse --short HEAD)"
