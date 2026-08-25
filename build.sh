#!/bin/sh
# Builds the static Alpine nshgeoip image and extracts the compiled
# binary to disk -- see Dockerfile and docker/compile_alpine_static.sh
# for what the build itself does.
#
# Usage:
#   ./build.sh                    # build image, extract binary to ./nshgeoip
#   ./build.sh ./out/nshgeoip     # ...to a specific path instead
#
# IMAGE_TAG=myregistry/nshgeoip:1.0 ./build.sh   # override the image tag
set -eu

IMAGE_TAG="${IMAGE_TAG:-nshgeoip:static}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OUT="${1:-$SCRIPT_DIR/nshgeoip}"

docker build --progress=plain -t "$IMAGE_TAG" "$SCRIPT_DIR"
echo "built image: $IMAGE_TAG"

CID="$(docker create "$IMAGE_TAG")"
docker cp "$CID:/usr/local/sbin/nshgeoip" "$OUT"
docker rm "$CID" >/dev/null
echo "extracted binary to $OUT"
