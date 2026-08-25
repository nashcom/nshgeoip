#!/bin/sh
# Builds a fully static nshgeoip binary against Alpine's packaged
# libmaxminddb-static. Meant to run inside an Alpine container with
# g++, make, libmaxminddb-dev, and libmaxminddb-static already installed
# (see ../Dockerfile) -- this script only does the actual compile/link,
# not environment setup, so it works the same whether it's invoked from
# a Dockerfile RUN or interactively inside your own build container.
#
# Usage: docker/compile_alpine_static.sh [output-path]
# (default output path: ./nshgeoip, i.e. the project root)
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
OUT="${1:-$PROJECT_ROOT/nshgeoip}"

cd "$PROJECT_ROOT"

echo "==> compiling nshgeoip object files"
make src/config.o src/geoip.o src/http.o src/ip_addr.o src/main.o src/server.o src/text_util.o src/thread_pool.o \
    CXXFLAGS="-std=c++17 -Wall -Wextra -O2 -pthread"

echo "==> compiling fortify shim"
# On Alpine, g++/libstdc++ still emits calls to glibc-style
# _FORTIFY_SOURCE wrapper symbols (__printf_chk, __snprintf_chk,
# __isoc23_strtol, etc.) at -O2, but musl's *static* libc doesn't provide
# them (only glibc does) -- reproduced across multiple Alpine/gcc
# versions, not a one-off quirk. fortify_shim.cpp provides thin
# pass-throughs to the real functions; see its own comment for why
# that's safe. It's compiled and linked here rather than folded into the
# regular src/ build, since linking it into a normal glibc build would
# fail with "multiple definition" -- glibc already provides all of these
# natively.
g++ -std=c++17 -Wall -Wextra -O2 -pthread -c "$SCRIPT_DIR/fortify_shim.cpp" -o "$SCRIPT_DIR/fortify_shim.o"

echo "==> linking static binary -> $OUT"
g++ -static -pthread -o "$OUT" \
    src/config.o src/geoip.o src/http.o src/ip_addr.o src/main.o src/server.o src/text_util.o src/thread_pool.o \
    "$SCRIPT_DIR/fortify_shim.o" \
    -lmaxminddb -lssp_nonshared

echo "==> done: $OUT"
file "$OUT" 2>/dev/null || true
