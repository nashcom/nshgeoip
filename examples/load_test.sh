#!/bin/bash
# Load-tests a throwaway nshgeoip instance's /lookup endpoint using ab
# (Apache Bench) and/or wrk, whichever is installed.
#
# ab and wrk are both TCP-only tools -- neither can talk to a UNIX domain
# socket -- so this starts nshgeoip with its optional TCP listener enabled
# and benchmarks that instead. NGINX's real auth_request path normally goes
# over the UNIX socket (see README.md's Concurrency model section), and
# nshgeoip closes every connection after one response (no HTTP keep-alive),
# so this measures accept()+dispatch+lookup overhead under fresh connections
# each time -- a synthetic throughput/latency benchmark, not a substitute
# for testing the real deployment path.
#
# Usage:
#   ./examples/load_test.sh
#   LOAD_TEST_IP=1.1.1.1 LOAD_TEST_CONCURRENCY=100 ./examples/load_test.sh
#
# Needs at least one real or fixture .mmdb database, same as
# tests/integration_test.sh -- see that script's own comment for details
# (auto-detected at /var/lib/GeoIP etc., or point TEST_COUNTRY_DB/
# TEST_ASN_DB/TEST_CITY_DB at MaxMind's redistributable test fixtures).
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=../default_geoip_dirs.sh
. "$SCRIPT_DIR/../default_geoip_dirs.sh"

NSHGEOIP_BIN="${NSHGEOIP_BIN:-./nshgeoip}"
TEST_COUNTRY_DB="${TEST_COUNTRY_DB:-}"
TEST_ASN_DB="${TEST_ASN_DB:-}"
TEST_CITY_DB="${TEST_CITY_DB:-}"

LOAD_TEST_IP="${LOAD_TEST_IP:-8.8.8.8}"
LOAD_TEST_PORT="${LOAD_TEST_PORT:-18767}"
LOAD_TEST_REQUESTS="${LOAD_TEST_REQUESTS:-20000}"      # ab only
LOAD_TEST_CONCURRENCY="${LOAD_TEST_CONCURRENCY:-50}"   # ab and wrk
LOAD_TEST_DURATION="${LOAD_TEST_DURATION:-10}"         # seconds, wrk only
LOAD_TEST_THREADS="${LOAD_TEST_THREADS:-4}"            # wrk only

if [ ! -x "$NSHGEOIP_BIN" ]; then
    echo "nshgeoip binary not found or not executable: $NSHGEOIP_BIN" >&2
    echo "build it first (make) or set NSHGEOIP_BIN" >&2
    exit 2
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "curl is required (used to wait for startup)" >&2
    exit 2
fi

HAVE_AB=""
command -v ab >/dev/null 2>&1 && HAVE_AB="yes"
HAVE_WRK=""
command -v wrk >/dev/null 2>&1 && HAVE_WRK="yes"
if [ -z "$HAVE_AB" ] && [ -z "$HAVE_WRK" ]; then
    echo "Neither ab (apache2-utils / httpd-tools) nor wrk is installed --" >&2
    echo "install at least one to run this." >&2
    exit 2
fi

if [ -z "$TEST_COUNTRY_DB" ] && [ -z "$TEST_ASN_DB" ] && [ -z "$TEST_CITY_DB" ] && ! nshgeoip_any_db_available; then
    echo "No database configured or auto-detectable -- see tests/integration_test.sh's" >&2
    echo "own comment for how to point TEST_COUNTRY_DB/TEST_ASN_DB/TEST_CITY_DB at" >&2
    echo "MaxMind's small redistributable test fixtures." >&2
    exit 2
fi

WORKDIR="$(mktemp -d /tmp/nshgeoip_load.XXXXXX)"
CONF="$WORKDIR/nshgeoip.conf"
SOCK="$WORKDIR/nshgeoip.sock"
NSHGEOIP_PID=""

cleanup() {
    if [ -n "$NSHGEOIP_PID" ] && kill -0 "$NSHGEOIP_PID" 2>/dev/null; then
        kill -TERM "$NSHGEOIP_PID" 2>/dev/null
        wait "$NSHGEOIP_PID" 2>/dev/null
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

{
    [ -n "$TEST_COUNTRY_DB" ] && echo "country_db=$TEST_COUNTRY_DB"
    [ -n "$TEST_ASN_DB" ] && echo "asn_db=$TEST_ASN_DB"
    [ -n "$TEST_CITY_DB" ] && echo "city_db=$TEST_CITY_DB"
    echo "socket=$SOCK"
    echo "tcp_port=$LOAD_TEST_PORT"
    echo "tcp_address=127.0.0.1"
} > "$CONF"

"$NSHGEOIP_BIN" --config "$CONF" >"$WORKDIR/stdout.log" 2>"$WORKDIR/stderr.log" &
NSHGEOIP_PID=$!

ready=""
for _ in $(seq 1 50); do
    if curl -sS -o /dev/null "http://127.0.0.1:$LOAD_TEST_PORT/health" 2>/dev/null; then
        ready="yes"
        break
    fi
    kill -0 "$NSHGEOIP_PID" 2>/dev/null || break
    sleep 0.1
done
if [ -z "$ready" ]; then
    echo "nshgeoip did not come up on 127.0.0.1:$LOAD_TEST_PORT" >&2
    cat "$WORKDIR/stderr.log" >&2
    exit 1
fi

URL="http://127.0.0.1:$LOAD_TEST_PORT/lookup?ip=$LOAD_TEST_IP"
echo "Load-testing $URL"
echo "(TCP only, no keep-alive -- see this script's header comment for why)"
echo

if [ -n "$HAVE_AB" ]; then
    echo "--- ab: $LOAD_TEST_REQUESTS requests, concurrency $LOAD_TEST_CONCURRENCY ---"
    ab -n "$LOAD_TEST_REQUESTS" -c "$LOAD_TEST_CONCURRENCY" "$URL"
    echo
fi

if [ -n "$HAVE_WRK" ]; then
    echo "--- wrk: $LOAD_TEST_THREADS threads, concurrency $LOAD_TEST_CONCURRENCY, ${LOAD_TEST_DURATION}s ---"
    wrk -t"$LOAD_TEST_THREADS" -c"$LOAD_TEST_CONCURRENCY" -d"${LOAD_TEST_DURATION}s" "$URL"
    echo
fi
