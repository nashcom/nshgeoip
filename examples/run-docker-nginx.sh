#!/bin/bash
# Runs NGINX in Docker with nginx-geoip.conf, for a quick end-to-end smoke
# test against an already-running nshgeoip: does the auth_request subrequest
# to nshgeoip actually populate X-GeoIP-* headers on a real NGINX response?
#
# nshgeoip itself is NOT started by this script -- it must already be
# running on the host (or wherever Docker's bind mounts resolve against;
# with Docker Desktop's WSL2 integration, that's the integrated WSL distro's
# filesystem), with its UNIX socket somewhere on disk. Point
# NSHGEOIP_SOCKET_DIR at that socket's directory (not the socket file itself
# -- the whole directory gets bind-mounted read-only into the container).
#
# Usage:
#   NSHGEOIP_SOCKET_DIR=/tmp/nshgeoip ./examples/docker-run-nginx.sh
#   NSHGEOIP_SOCKET_DIR=/run/nshgeoip ./examples/docker-run-nginx.sh   # systemd-managed nshgeoip

docker kill nshgeoip-nginx
docker rm nshgeoip-nginx

NSHGEOIP_SOCKET_DIR="${NSHGEOIP_SOCKET_DIR:-/run/nshgeoip}"
HOST_PORT="${HOST_PORT:-8888}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

if [ ! -S "$NSHGEOIP_SOCKET_DIR/nshgeoip.sock" ]; then
  echo "warning: $NSHGEOIP_SOCKET_DIR/nshgeoip.sock not found -- is nshgeoip running, and is NSHGEOIP_SOCKET_DIR set to its actual socket directory?" >&2
fi

docker run -d --name nshgeoip-nginx --network host -v "${NSHGEOIP_SOCKET_DIR}:/run/nshgeoip:ro" -v "${SCRIPT_DIR}/nginx-geoip.conf:/etc/nginx/nginx.conf:ro" nginx:alpine

echo
sleep 3
docker logs nshgeoip-nginx
echo
