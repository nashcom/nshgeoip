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

# Defined once, reused at every compile AND the final link -- not just
# tidiness. GCC's own LTO docs say optimization flags need to be present at
# the final link step too, not only at each individual compile, since the
# LTO backend's actual whole-program codegen happens at link time; typing
# these three times independently is exactly the kind of drift that let
# src/metrics.cpp silently go missing from this script's object list
# earlier (see git history) -- one definition removes that risk here too.
#
# -Os: size, not speed -- nshgeoip's workload is short synchronous lookups,
# not CPU-bound, so this is a real size win with no meaningful cost.
# -fno-exceptions -fno-rtti: nshgeoip's own code has no throw/catch/
# dynamic_cast/typeid anywhere (grepped to confirm), and this has been
# functionally verified end-to-end (lookup, /health, --health-check, and
# the 400-on-invalid-IP validation path) -- an uncaught exception would
# already crash the process either way (nothing catches anything), so
# std::terminate() via -fno-exceptions changes nothing observable, just
# removes the unwind-table machinery.
# -flto: gives --gc-sections whole-program visibility instead of per-file,
# catching more dead code (verified: an extra ~8% smaller on top of
# everything else).
# -ffunction-sections -fdata-sections: lets --gc-sections (at the final
# link, below) drop functions/data nothing actually references, including
# unused pieces of the statically-linked libmaxminddb.a/libstdc++.a.
#
# Local debugging still uses the plain Makefile build (-O2, exceptions/RTTI
# enabled, no LTO) -- only this static/release build trades those for size.
CXXFLAGS="-std=c++17 -Wall -Wextra -Os -flto -pthread -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti"

echo "==> compiling nshgeoip object files"
# Derived from src/*.cpp (same source list the Makefile's own wildcard uses)
# rather than hardcoded, so a new source file can't silently go missing from
# the static build the way src/metrics.cpp once did here.
OBJS=""
for f in src/*.cpp; do
    OBJS="$OBJS ${f%.cpp}.o"
done

make $OBJS CXXFLAGS="$CXXFLAGS"

echo "==> compiling fortify shim"
# On Alpine, g++/libstdc++ still emits calls to glibc-style
# _FORTIFY_SOURCE wrapper symbols (__printf_chk, __snprintf_chk,
# __isoc23_strtol, etc.) at any optimization level above -O0, but musl's
# *static* libc doesn't provide them (only glibc does) -- reproduced across
# multiple Alpine/gcc versions, not a one-off quirk. fortify_shim.cpp
# provides thin pass-throughs to the real functions; see its own comment for
# why that's safe. It's compiled and linked here rather than folded into the
# regular src/ build, since linking it into a normal glibc build would fail
# with "multiple definition" -- glibc already provides all of these
# natively. Pure C-style code, no exceptions/RTTI/STL to lose from matching
# the main build's flags.
g++ $CXXFLAGS -c "$SCRIPT_DIR/fortify_shim.cpp" -o "$SCRIPT_DIR/fortify_shim.o"

echo "==> linking static binary -> $OUT"
# -static: fully static binary, no shared libmaxminddb/libc at all.
# -s: strips the symbol table and relocation info at link time (same effect
# as a separate `strip` pass, just done by the linker itself) -- matters
# more here than for a normal dynamic build, since static linking pulls in
# libmaxminddb.a's and libstdc++.a's own symbol tables too, not just
# nshgeoip's. No debug info is generated in the first place (no -g), so
# this isn't discarding something a debugger would otherwise use.
# -Wl,--gc-sections: the actual dead-code removal -ffunction-sections/
# -fdata-sections (in $CXXFLAGS) made possible.
g++ $CXXFLAGS -static -s -Wl,--gc-sections -o "$OUT" \
    $OBJS \
    "$SCRIPT_DIR/fortify_shim.o" \
    -lmaxminddb -lssp_nonshared

echo "==> done: $OUT"
file "$OUT" 2>/dev/null || true
