#!/bin/bash
# Process/protocol-level integration tests for nshgeoip, run against a real
# running daemon over its actual UNIX socket. Complements the pure-function
# unit tests in test_nshgeoip.cpp.
#
# Configuration (all optional except NSHGEOIP_BIN):
#   NSHGEOIP_BIN        path to the nshgeoip binary (default: ./nshgeoip)
#   TEST_COUNTRY_DB   path to a GeoLite2-Country(-Test).mmdb
#   TEST_ASN_DB       path to a GeoLite2-ASN(-Test).mmdb
#   TEST_CITY_DB      path to a GeoLite2-City(-Test).mmdb
#   TEST_KNOWN_IP / TEST_KNOWN_COUNTRY
#                     an IP known to be in TEST_COUNTRY_DB and its expected
#                     ISO country code, to assert actual lookup content
#
# If TEST_COUNTRY_DB/TEST_ASN_DB/TEST_CITY_DB are not set, content-dependent
# tests are skipped but all protocol-level tests (status codes, malformed
# requests, concurrency, stale socket handling, clean shutdown) still run
# using an empty/dummy setup where possible.
#
# MaxMind publishes small, redistributable test fixture databases (used by
# libmaxminddb's own test suite) at:
#   https://github.com/maxmind/MaxMind-DB/tree/main/test-data
# GeoLite2-Country-Test.mmdb / GeoLite2-ASN-Test.mmdb / GeoLite2-City-Test.mmdb
# from that repo work well as TEST_COUNTRY_DB / TEST_ASN_DB / TEST_CITY_DB
# for this script.
#
# Separately, if a real .mmdb already exists and is readable at one of
# nshgeoip's own auto-detect locations (/var/lib/GeoIP, matching
# geoipupdate's default, or /var/lib/crowdsec/data, CrowdSec's bundled
# copy -- see apply_default_db_paths() in src/config.cpp), this script also
# runs an end-to-end auto-detect test against it: no staging, no root
# needed, just reads whatever is already there. Skipped entirely if none
# of those files are present/readable.

set -u

TESTS_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=../default_geoip_dirs.sh
. "$TESTS_DIR/../default_geoip_dirs.sh"

NSHGEOIP_BIN="${NSHGEOIP_BIN:-./nshgeoip}"
TEST_COUNTRY_DB="${TEST_COUNTRY_DB:-}"
TEST_ASN_DB="${TEST_ASN_DB:-}"
TEST_CITY_DB="${TEST_CITY_DB:-}"
TEST_KNOWN_IP="${TEST_KNOWN_IP:-}"
TEST_KNOWN_COUNTRY="${TEST_KNOWN_COUNTRY:-}"

PASS=0
FAIL=0
SKIP=0

pass() { PASS=$((PASS + 1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL: $1"; }
skip() { SKIP=$((SKIP + 1)); echo "SKIP: $1"; }

WORKDIR="$(mktemp -d /tmp/nshgeoip_test.XXXXXX)"
SOCK="$WORKDIR/nshgeoip.sock"
CONF="$WORKDIR/nshgeoip.conf"
NSHGEOIP_PID=""

cleanup() {
    if [ -n "$NSHGEOIP_PID" ] && kill -0 "$NSHGEOIP_PID" 2>/dev/null; then
        kill -TERM "$NSHGEOIP_PID" 2>/dev/null
        wait "$NSHGEOIP_PID" 2>/dev/null
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

write_conf() {
    {
        [ -n "$TEST_COUNTRY_DB" ] && echo "country_db=$TEST_COUNTRY_DB"
        [ -n "$TEST_ASN_DB" ] && echo "asn_db=$TEST_ASN_DB"
        [ -n "$TEST_CITY_DB" ] && echo "city_db=$TEST_CITY_DB"
        echo "socket=$SOCK"
        echo "socket_mode=0660"
        echo "threads=4"
    } > "$CONF"
}

start_daemon() {
    "$NSHGEOIP_BIN" --config "$CONF" >"$WORKDIR/stdout.log" 2>"$WORKDIR/stderr.log" &
    NSHGEOIP_PID=$!
    for _ in $(seq 1 50); do
        [ -S "$SOCK" ] && return 0
        kill -0 "$NSHGEOIP_PID" 2>/dev/null || return 1
        sleep 0.1
    done
    [ -S "$SOCK" ]
}

stop_daemon() {
    if [ -n "$NSHGEOIP_PID" ] && kill -0 "$NSHGEOIP_PID" 2>/dev/null; then
        kill -TERM "$NSHGEOIP_PID" 2>/dev/null
        for _ in $(seq 1 50); do
            kill -0 "$NSHGEOIP_PID" 2>/dev/null || break
            sleep 0.1
        done
    fi
}

http_status() {
    curl -sS -o /dev/null -w '%{http_code}' --unix-socket "$SOCK" "$1" \
        ${2:+-X "$2"}
}

if [ ! -x "$NSHGEOIP_BIN" ]; then
    echo "nshgeoip binary not found or not executable: $NSHGEOIP_BIN" >&2
    echo "build it first (make) or set NSHGEOIP_BIN" >&2
    exit 2
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "curl is required to run this test suite" >&2
    exit 2
fi
# Whether ANY database is in play at all -- explicit fixtures or a real
# auto-detected one. Gates tests that only need *some* working database and
# whose assertions don't depend on fixture-specific content (e.g. an
# RFC 5737 documentation-range address that's absent from every real GeoIP
# database, fixture or production, by definition -- see the IP-not-found
# test below). Tests asserting specific fixture values (a known
# country_code, city name, etc.) must stay gated on the more specific
# TEST_COUNTRY_DB/TEST_ASN_DB/TEST_CITY_DB instead, since real production
# data won't contain MaxMind's synthetic test addresses.
HAVE_ANY_DB=""
if [ -n "$TEST_COUNTRY_DB" ] || [ -n "$TEST_ASN_DB" ] || [ -n "$TEST_CITY_DB" ] || nshgeoip_any_db_available; then
    HAVE_ANY_DB="yes"
fi

if [ -z "$TEST_COUNTRY_DB" ] && [ -z "$TEST_ASN_DB" ] && [ -z "$TEST_CITY_DB" ]; then
    # nshgeoip refuses to start at all with no database configured and none
    # auto-detectable (by design -- see Server::init() and
    # apply_default_db_paths()), so at least one of those two is a hard
    # requirement for exercising the running daemon, not just for the
    # content-assertion tests below. write_conf() below writes no explicit
    # country_db=/asn_db=/city_db= either way, so nshgeoip's own
    # auto-detection covers the "real database, no TEST_*_DB set" case
    # identically to a config file that omits them.
    if [ -n "$HAVE_ANY_DB" ]; then
        echo "No TEST_COUNTRY_DB/TEST_ASN_DB/TEST_CITY_DB set, but a real database was" >&2
        echo "auto-detected -- running with content-specific assertions skipped." >&2
    else
        echo "TEST_COUNTRY_DB, TEST_ASN_DB, or TEST_CITY_DB must be set to a real .mmdb file" >&2
        echo "(nshgeoip will not start without at least one database configured or" >&2
        echo "auto-detectable at a standard location -- see default_geoip_dirs.sh)." >&2
        echo "MaxMind's small test fixtures work well here, e.g. GeoLite2-Country-Test.mmdb" >&2
        echo "from https://github.com/maxmind/MaxMind-DB/tree/main/test-data" >&2
        exit 2
    fi
fi

# --- stale socket: leftover regular (non-socket) file must be refused ---
write_conf
: > "$SOCK"  # plain regular file, not a socket
if start_daemon; then
    fail "startup refuses a non-socket file at the socket path (daemon started anyway)"
    stop_daemon
else
    if grep -qi "refusing to remove non-socket" "$WORKDIR/stderr.log" 2>/dev/null; then
        pass "startup refuses a non-socket file at the socket path"
    else
        pass "startup fails when a non-socket file occupies the socket path"
    fi
fi
rm -f "$SOCK"

# --- stale socket: leftover socket file from a crashed/killed run ---
if command -v python3 >/dev/null 2>&1; then
    python3 - "$SOCK" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(sys.argv[1])
# deliberately leaked: no close(), no unlink() -- simulates a crash leaving
# the socket file behind while nothing is listening on it.
PY
    if [ -S "$SOCK" ]; then
        if start_daemon; then
            pass "startup removes a stale (abandoned) socket file and binds successfully"
            stop_daemon
        else
            fail "startup did not recover from a stale socket file"
        fi
    else
        skip "stale socket file test (python3 could not create test socket)"
    fi
else
    skip "stale socket file test (python3 not available)"
fi
rm -f "$SOCK"

# --- second instance against a socket a first instance is still live on ---
# A socket file existing does not mean it's stale -- something could still
# be listening on it. Starting a second nshgeoip against the same path must
# be refused, and must NOT delete the first instance's socket out from
# under it (see path_has_live_listener() in src/server.cpp).
write_conf
if start_daemon; then
    second_out="$("$NSHGEOIP_BIN" --config "$CONF" 2>&1)"
    second_status=$?
    if [ "$second_status" -eq 0 ]; then
        fail "second instance against a live socket is refused (it started successfully instead)"
    elif ! echo "$second_out" | grep -qi "already listening"; then
        fail "second instance against a live socket is refused (wrong error: $second_out)"
    else
        pass "second instance against a live socket is refused"
    fi

    if [ -S "$SOCK" ] && [ "$(http_status 'http://localhost/lookup?ip=8.8.8.8')" != "000" ]; then
        pass "first instance's socket survives a second instance's refused startup"
    else
        fail "first instance's socket survives a second instance's refused startup"
    fi

    stop_daemon
else
    fail "could not start first instance for the live-socket-refusal test"
fi
rm -f "$SOCK"

# --- normal startup for the remaining tests ---
write_conf
if ! start_daemon; then
    echo "nshgeoip failed to start; see $WORKDIR/stderr.log" >&2
    cat "$WORKDIR/stderr.log" >&2
    exit 1
fi

# --- basic status codes ---
[ "$(http_status 'http://localhost/lookup?ip=8.8.8.8')" != "000" ] \
    && pass "valid IPv4 request gets an HTTP response" \
    || fail "valid IPv4 request gets an HTTP response"

[ "$(http_status 'http://localhost/lookup?ip=2001:4860:4860::8888')" != "000" ] \
    && pass "valid IPv6 request gets an HTTP response" \
    || fail "valid IPv6 request gets an HTTP response"

[ "$(http_status 'http://localhost/lookup?ip=not-an-ip')" = "400" ] \
    && pass "invalid IPv4 syntax returns 400" \
    || fail "invalid IPv4 syntax returns 400"

[ "$(http_status 'http://localhost/lookup?ip=gggg::1')" = "400" ] \
    && pass "invalid IPv6 syntax returns 400" \
    || fail "invalid IPv6 syntax returns 400"

[ "$(http_status 'http://localhost/lookup')" = "400" ] \
    && pass "missing ip parameter returns 400" \
    || fail "missing ip parameter returns 400"

[ "$(http_status 'http://localhost/lookup?ip=8.8.8.8' PUT)" = "405" ] \
    && pass "unsupported method returns 405" \
    || fail "unsupported method returns 405"

# HEAD must behave like GET (same status/headers, including a correct
# Content-Length) but with the body withheld. Using curl's dedicated -I
# rather than -X HEAD: curl -I is the one that actually knows not to read a
# body, so a wrong Content-Length or a body nshgeoip sent anyway would make
# curl itself error out here. 8.8.8.8 isn't guaranteed to resolve against
# MaxMind's tiny test fixtures, so this only checks HEAD's protocol
# behavior (curl accepted the response cleanly), not lookup content --
# that's already covered by the GET-based content assertions elsewhere.
head_status="$(curl -sS -o /dev/null -w '%{http_code}' -I --unix-socket "$SOCK" 'http://localhost/lookup?ip=8.8.8.8')"
head_rc=$?
if [ "$head_rc" -eq 0 ] && { [ "$head_status" = "200" ] || [ "$head_status" = "404" ]; }; then
    pass "HEAD request gets a well-formed response with no body"
else
    fail "HEAD request gets a well-formed response with no body (curl exit=$head_rc, status=$head_status)"
fi

# HEAD has no body, so unlike GET it should not send Content-Type either
# (nothing to describe); Content-Length is still expected.
head_headers="$(curl -sS -I --unix-socket "$SOCK" 'http://localhost/lookup?ip=8.8.8.8')"
if ! echo "$head_headers" | grep -qi '^Content-Type:' \
    && echo "$head_headers" | grep -qi '^Content-Length:'; then
    pass "HEAD response omits Content-Type but keeps Content-Length"
else
    fail "HEAD response omits Content-Type but keeps Content-Length (headers: $head_headers)"
fi

# Content negotiation: default is JSON, Accept: text/plain switches to the
# INI-style "key=value" body. Content-Type alone doesn't depend on lookup
# content (even an error body is negotiated), but confirming the INI body
# itself needs an address that actually resolves -- 8.8.8.8 isn't
# guaranteed to against MaxMind's tiny test fixtures, so this reuses
# 67.43.156.1 like the country-lookup content assertion above.
default_ctype="$(curl -sS -o /dev/null -D - --unix-socket "$SOCK" 'http://localhost/lookup?ip=8.8.8.8' | grep -i '^Content-Type:')"
if echo "$default_ctype" | grep -qi 'application/json'; then
    pass "no Accept header defaults to application/json"
else
    fail "no Accept header defaults to application/json (got: $default_ctype)"
fi

# --- /health and /metrics: always available, no config flag. /metrics is
# never negotiated (always Prometheus text). /health defaults to a minimal
# text body -- unlike /lookup, where no Accept header means JSON -- since
# most container health checks never send an Accept header and only look
# at the status code; the full per-database JSON is opt-in. ---
health_body_default="$(curl -sS --unix-socket "$SOCK" 'http://localhost/health')"
health_ctype_default="$(curl -sS -o /dev/null -D - --unix-socket "$SOCK" 'http://localhost/health' | grep -i '^Content-Type:')"
if echo "$health_ctype_default" | grep -qi 'text/plain' && [ "$health_body_default" = "status=ok" ]; then
    pass "/health with no Accept header returns a minimal text status"
else
    fail "/health with no Accept header returns a minimal text status (ctype: $health_ctype_default, body: $health_body_default)"
fi

health_body_json="$(curl -sS -H 'Accept: application/json' --unix-socket "$SOCK" 'http://localhost/health')"
health_ctype_json="$(curl -sS -o /dev/null -D - -H 'Accept: application/json' --unix-socket "$SOCK" 'http://localhost/health' | grep -i '^Content-Type:')"
if echo "$health_ctype_json" | grep -qi 'application/json' \
    && echo "$health_body_json" | grep -q '"status":"ok"' \
    && echo "$health_body_json" | grep -q '"databases"'; then
    pass "/health with Accept: application/json returns full per-database JSON"
else
    fail "/health with Accept: application/json returns full per-database JSON (ctype: $health_ctype_json, body: $health_body_json)"
fi

metrics_body="$(curl -sS --unix-socket "$SOCK" 'http://localhost/metrics')"
metrics_ctype="$(curl -sS -o /dev/null -D - --unix-socket "$SOCK" 'http://localhost/metrics' | grep -i '^Content-Type:')"
if echo "$metrics_ctype" | grep -qi 'text/plain' \
    && echo "$metrics_body" | grep -q '^# HELP nshgeoip_requests_total' \
    && echo "$metrics_body" | grep -q '^nshgeoip_build_info{version='; then
    pass "/metrics returns Prometheus text format with expected metric names"
else
    fail "/metrics returns Prometheus text format with expected metric names (ctype: $metrics_ctype)"
fi

# Two /lookup requests, then confirm the requests_total{path="lookup"}
# counter specifically moved by exactly that amount -- not just that the
# endpoint renders something plausible, and not conflated with /metrics'
# own self-counted scrape requests (see requests_total{path="metrics"}).
before="$(curl -sS --unix-socket "$SOCK" 'http://localhost/metrics' | grep 'requests_total{path="lookup"}' | awk '{print $2}')"
curl -sS --unix-socket "$SOCK" 'http://localhost/lookup?ip=8.8.8.8' >/dev/null
curl -sS --unix-socket "$SOCK" 'http://localhost/lookup?ip=192.0.2.1' >/dev/null
after="$(curl -sS --unix-socket "$SOCK" 'http://localhost/metrics' | grep 'requests_total{path="lookup"}' | awk '{print $2}')"
if [ -n "$before" ] && [ -n "$after" ] && [ "$after" -eq "$((before + 2))" ]; then
    pass "nshgeoip_requests_total{path=\"lookup\"} increases with real /lookup traffic"
else
    fail "nshgeoip_requests_total{path=\"lookup\"} increases with real /lookup traffic (before=$before, after=$after)"
fi

head_status_health="$(curl -sS -o /dev/null -w '%{http_code}' -I --unix-socket "$SOCK" 'http://localhost/health')"
head_status_metrics="$(curl -sS -o /dev/null -w '%{http_code}' -I --unix-socket "$SOCK" 'http://localhost/metrics')"
if [ "$head_status_health" = "200" ] && [ "$head_status_metrics" = "200" ]; then
    pass "HEAD /health and HEAD /metrics both return 200"
else
    fail "HEAD /health and HEAD /metrics both return 200 (health=$head_status_health, metrics=$head_status_metrics)"
fi

if [ -n "$TEST_COUNTRY_DB" ] || [ -n "$TEST_ASN_DB" ]; then
    text_body="$(curl -sS -H 'Accept: text/plain' --unix-socket "$SOCK" 'http://localhost/lookup?ip=67.43.156.1')"
    text_ctype="$(curl -sS -o /dev/null -D - -H 'Accept: text/plain' --unix-socket "$SOCK" 'http://localhost/lookup?ip=67.43.156.1' | grep -i '^Content-Type:')"
    if echo "$text_ctype" | grep -qi 'text/plain' && echo "$text_body" | grep -q '^ip=67\.43\.156\.1$'; then
        pass "Accept: text/plain returns an INI-style body"
    else
        fail "Accept: text/plain returns an INI-style body (ctype: $text_ctype, body: $text_body)"
    fi
else
    skip "Accept: text/plain content check (no country/ASN database configured)"
fi

# TEST-NET-1 (RFC 5737 documentation range): syntactically valid, and not
# expected to be present in any GeoIP database, fixture or real -- safe to
# run against either, unlike the fixture-specific content assertions below.
status="$(http_status 'http://localhost/lookup?ip=192.0.2.1')"
if [ -n "$HAVE_ANY_DB" ]; then
    [ "$status" = "404" ] \
        && pass "IP with no GeoIP data returns 404" \
        || fail "IP with no GeoIP data returns 404 (got $status)"
else
    skip "IP-not-found test (no database configured)"
fi

# --- content assertions ---
#
# 8.8.8.8 is NOT a useful test address here: MaxMind's small redistributable
# test fixtures (the ones this script's own documentation recommends) only
# cover a handful of curated test networks, not real public IP space. When
# TEST_COUNTRY_DB/TEST_ASN_DB/TEST_CITY_DB point at those fixtures (source:
# https://github.com/maxmind/MaxMind-DB/tree/main/test-data), a few of their
# addresses are used instead:
#   67.43.156.1  -- present in both fixtures (country BT / continent AS)
#   1.0.0.1      -- present only in the ASN fixture (AS15169 / Google),
#                    absent from the country fixture -- this doubles as the
#                    "missing MMDB field" case: found overall (asn present)
#                    but country_code/continent_code come back null.
#   2.125.160.220 -- present in both the city and country fixtures
#                    (city Boxford, postal OX1, country GB).
# If real production databases are configured instead, these addresses may
# legitimately resolve to something else (or nothing), so the checks are
# skipped unless TEST_KNOWN_IP overrides them -- see below.

if [ -n "$TEST_COUNTRY_DB" ] && [ -z "$TEST_KNOWN_IP" ]; then
    body="$(curl -sS --unix-socket "$SOCK" 'http://localhost/lookup?ip=67.43.156.1')"
    if echo "$body" | grep -q '"country_code":"BT"'; then
        pass "country lookup: known fixture address resolves to expected country_code"
    else
        fail "country lookup: known fixture address resolves to expected country_code (body: $body)"
    fi
fi

if [ -n "$TEST_ASN_DB" ] && [ -z "$TEST_KNOWN_IP" ]; then
    body="$(curl -sS --unix-socket "$SOCK" 'http://localhost/lookup?ip=1.0.0.1')"
    if echo "$body" | grep -q '"asn":15169'; then
        pass "ASN lookup: known fixture address resolves to expected asn"
    else
        fail "ASN lookup: known fixture address resolves to expected asn (body: $body)"
    fi

    if [ -n "$TEST_COUNTRY_DB" ]; then
        if echo "$body" | grep -q '"country_code":null'; then
            pass "missing MMDB field: address absent from country db comes back as null, not omitted or invented"
        else
            fail "missing MMDB field: address absent from country db comes back as null (body: $body)"
        fi
    fi
fi

if [ -n "$TEST_CITY_DB" ] && [ -z "$TEST_KNOWN_IP" ]; then
    # 2.125.160.220 -- present in both the city and country fixtures
    # (city Boxford, postal OX1, country GB), giving a combined-lookup
    # check for free when TEST_COUNTRY_DB is also set.
    body="$(curl -sS --unix-socket "$SOCK" 'http://localhost/lookup?ip=2.125.160.220')"
    if echo "$body" | grep -q '"city_name":"Boxford"' \
        && echo "$body" | grep -q '"postal_code":"OX1"' \
        && echo "$body" | grep -q '"latitude":51.75' \
        && echo "$body" | grep -q '"longitude":-1.25'; then
        pass "city lookup: known fixture address resolves to expected city/postal/lat/long"
    else
        fail "city lookup: known fixture address resolves to expected city/postal/lat/long (body: $body)"
    fi
fi

if [ -n "$TEST_KNOWN_IP" ] && [ -n "$TEST_KNOWN_COUNTRY" ]; then
    body="$(curl -sS --unix-socket "$SOCK" "http://localhost/lookup?ip=$TEST_KNOWN_IP")"
    if echo "$body" | grep -q "\"country_code\":\"$TEST_KNOWN_COUNTRY\""; then
        pass "known IP resolves to expected country code"
    else
        fail "known IP resolves to expected country code (body: $body)"
    fi
else
    skip "known-IP content assertion (TEST_KNOWN_IP/TEST_KNOWN_COUNTRY not set)"
fi

# --- malformed HTTP request (raw bytes on the wire) ---
if command -v python3 >/dev/null 2>&1; then
    status="$(python3 - "$SOCK" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(3)
s.connect(sys.argv[1])
s.sendall(b"NOT AN HTTP REQUEST AT ALL\r\n\r\n")
try:
    data = s.recv(4096)
except socket.timeout:
    data = b""
print(data.split(b" ")[1].decode() if data.startswith(b"HTTP/") else "000")
PY
)"
    [ "$status" = "400" ] \
        && pass "malformed HTTP request returns 400" \
        || fail "malformed HTTP request returns 400 (got '$status')"
else
    skip "malformed HTTP request test (python3 not available)"
fi

# --- concurrent requests ---
CONCURRENCY=20
rm -f "$WORKDIR"/conc_*.status
conc_pids=()
for i in $(seq 1 "$CONCURRENCY"); do
    ( http_status 'http://localhost/lookup?ip=1.1.1.1' > "$WORKDIR/conc_$i.status" ) &
    conc_pids+=("$!")
done
# Wait only for the requests just launched -- a bare `wait` would also
# block on $NSHGEOIP_PID, which is a long-running daemon backgrounded
# earlier in this same shell and never exits on its own.
for pid in "${conc_pids[@]}"; do
    wait "$pid"
done

ok_count=0
for i in $(seq 1 "$CONCURRENCY"); do
    code="$(cat "$WORKDIR/conc_$i.status" 2>/dev/null || echo 000)"
    [ "$code" != "000" ] && ok_count=$((ok_count + 1))
done
[ "$ok_count" -eq "$CONCURRENCY" ] \
    && pass "all $CONCURRENCY concurrent requests got an HTTP response" \
    || fail "all $CONCURRENCY concurrent requests got an HTTP response (only $ok_count/$CONCURRENCY did)"

# --- clean shutdown ---
stop_daemon
if kill -0 "$NSHGEOIP_PID" 2>/dev/null; then
    fail "daemon exits promptly on SIGTERM"
else
    pass "daemon exits promptly on SIGTERM"
fi
if [ -e "$SOCK" ]; then
    fail "socket file removed after clean shutdown"
else
    pass "socket file removed after clean shutdown"
fi
NSHGEOIP_PID=""

# --- city_db + asn_db only (no country_db): country/continent should still
# come from city_db's own fields, since a real GeoLite2-City.mmdb already
# carries them -- this is the setup CrowdSec-style consumers use, wanting
# city + ASN without also needing to maintain a separate Country database.
if [ -n "$TEST_ASN_DB" ] && [ -n "$TEST_CITY_DB" ]; then
    CONF="$WORKDIR/city_asn_only.conf"
    SOCK="$WORKDIR/city_asn_only.sock"
    {
        echo "asn_db=$TEST_ASN_DB"
        echo "city_db=$TEST_CITY_DB"
        echo "socket=$SOCK"
    } > "$CONF"

    if start_daemon; then
        body="$(curl -sS --unix-socket "$SOCK" 'http://localhost/lookup?ip=2.125.160.220')"
        if echo "$body" | grep -q '"country_code":"GB"' && echo "$body" | grep -q '"continent_code":"EU"'; then
            pass "city_db-only setup (no country_db) still resolves country/continent"
        else
            fail "city_db-only setup (no country_db) still resolves country/continent (body: $body)"
        fi
        stop_daemon
    else
        fail "could not start nshgeoip with only city_db + asn_db configured"
    fi
else
    skip "city_db + asn_db only test (TEST_ASN_DB/TEST_CITY_DB not both set)"
fi

# --- auto-detected database at a real standard location, if one exists ---
# Uses whatever is already on this machine (e.g. from ./nshgeoipctl.sh
# download-db, an existing geoipupdate install, or CrowdSec) instead of a
# staged fixture -- confirms the real apply_default_db_paths() code path,
# not just its unit tests. No root: only reads files already readable by
# this user, never writes into /var/lib/GeoIP or /var/lib/crowdsec/data.
# Filename order mirrors resolve_db_slot()'s own city-before-asn-before
# country priority (src/config.cpp); nshgeoip_find_db() (sourced from
# default_geoip_dirs.sh above) does the per-directory search, in the same
# NSHGEOIP_DEFAULT_GEOIP_DIRS order the application itself uses.
DEFAULT_DB_CANDIDATE=""
for filename in GeoLite2-City.mmdb GeoLite2-ASN.mmdb GeoLite2-Country.mmdb; do
    if candidate="$(nshgeoip_find_db "$filename")"; then
        DEFAULT_DB_CANDIDATE="$candidate"
        break
    fi
done

if [ -n "$DEFAULT_DB_CANDIDATE" ]; then
    CONF="$WORKDIR/default_db.conf"
    SOCK="$WORKDIR/default_db.sock"
    {
        echo "socket=$SOCK"
        # Deliberately no country_db=/asn_db=/city_db= at all -- this is
        # exactly the case apply_default_db_paths() exists for.
    } > "$CONF"

    if start_daemon; then
        if grep -qi 'auto-detected' "$WORKDIR/stdout.log" "$WORKDIR/stderr.log" 2>/dev/null; then
            pass "startup with no db config auto-detects a real database ($DEFAULT_DB_CANDIDATE)"
        else
            fail "startup with no db config auto-detects a real database (no auto-detect log line found)"
        fi

        status="$(http_status 'http://localhost/lookup?ip=8.8.8.8')"
        [ "$status" != "000" ] \
            && pass "lookup against the auto-detected database gets an HTTP response" \
            || fail "lookup against the auto-detected database gets an HTTP response (got $status)"

        stop_daemon
    else
        fail "nshgeoip failed to start using an auto-detected real database ($DEFAULT_DB_CANDIDATE); see $WORKDIR/stderr.log"
        cat "$WORKDIR/stderr.log" >&2
    fi
else
    skip "auto-detected real database test (no readable .mmdb found at /var/lib/GeoIP or /var/lib/crowdsec/data)"
fi

# --- metrics_file: periodic Prometheus-text export to disk, independent of
# the always-on /metrics HTTP endpoint ---
CONF="$WORKDIR/metrics_file.conf"
SOCK="$WORKDIR/metrics_file.sock"
METRICS_FILE="$WORKDIR/nshgeoip.prom"
write_conf
{
    echo "metrics_file=$METRICS_FILE"
    echo "metrics_interval_seconds=1"
} >> "$CONF"

if start_daemon; then
    # Written once immediately at startup (before the first interval
    # elapses), so this shouldn't need to wait a full second -- a short
    # poll loop keeps this robust against slow CI/VM scheduling either way.
    found_file=""
    for _ in $(seq 1 20); do
        [ -s "$METRICS_FILE" ] && { found_file="yes"; break; }
        sleep 0.1
    done

    if [ -n "$found_file" ] && grep -q '^# HELP nshgeoip_requests_total' "$METRICS_FILE" \
        && grep -q '^nshgeoip_build_info{version=' "$METRICS_FILE"; then
        pass "metrics_file is written at startup with expected content"
    else
        fail "metrics_file is written at startup with expected content (found_file=$found_file)"
    fi

    if [ ! -e "$METRICS_FILE.tmp" ]; then
        pass "metrics_file leaves no .tmp file behind (atomic rename completed)"
    else
        fail "metrics_file leaves no .tmp file behind (found $METRICS_FILE.tmp still present)"
    fi

    # Drive some real traffic, then confirm a later periodic write picked
    # it up (metrics_interval_seconds=1 above).
    curl -sS --unix-socket "$SOCK" 'http://localhost/lookup?ip=8.8.8.8' >/dev/null
    sleep 1.5
    if grep -q 'nshgeoip_requests_total{path="lookup"} [1-9]' "$METRICS_FILE"; then
        pass "metrics_file refreshes periodically and reflects real traffic"
    else
        fail "metrics_file refreshes periodically and reflects real traffic (contents: $(grep requests_total "$METRICS_FILE"))"
    fi

    stop_daemon
else
    fail "could not start nshgeoip with metrics_file configured"
fi

# --- SIGHUP: reload the configured databases without dropping the socket
# or interrupting service ---
CONF="$WORKDIR/sighup.conf"
SOCK="$WORKDIR/sighup.sock"
write_conf
if start_daemon; then
    kill -HUP "$NSHGEOIP_PID"
    sleep 0.3

    if grep -qi 'reloaded\|database reload complete' "$WORKDIR/stdout.log" "$WORKDIR/stderr.log" 2>/dev/null; then
        pass "SIGHUP triggers a database reload"
    else
        fail "SIGHUP triggers a database reload (no 'reloaded' log line found)"
    fi

    after_status="$(http_status 'http://localhost/lookup?ip=8.8.8.8')"
    if [ "$after_status" != "000" ] && [ -S "$SOCK" ]; then
        pass "daemon keeps serving requests after a SIGHUP reload"
    else
        fail "daemon keeps serving requests after a SIGHUP reload (status=$after_status, socket present=$([ -S "$SOCK" ] && echo yes || echo no))"
    fi

    stop_daemon
else
    fail "could not start nshgeoip for the SIGHUP reload test"
fi

# --- --check-db: standalone diagnostic mode, independent of any running
# daemon or configured database ---
CHECK_DB_TARGET=""
for candidate in "$TEST_CITY_DB" "$TEST_ASN_DB" "$TEST_COUNTRY_DB" "$DEFAULT_DB_CANDIDATE"; do
    if [ -n "$candidate" ] && [ -r "$candidate" ]; then
        CHECK_DB_TARGET="$candidate"
        break
    fi
done

if [ -n "$CHECK_DB_TARGET" ]; then
    check_db_out="$("$NSHGEOIP_BIN" --check-db "$CHECK_DB_TARGET" --format json 2>&1)"
    check_db_rc=$?
    if [ "$check_db_rc" -eq 0 ] && echo "$check_db_out" | grep -q '"database_type"'; then
        pass "--check-db prints metadata for a real .mmdb file and exits 0"
    else
        fail "--check-db prints metadata for a real .mmdb file and exits 0 (rc=$check_db_rc, output: $check_db_out)"
    fi
else
    skip "--check-db content test (no real or fixture .mmdb available)"
fi

check_db_bad_out="$("$NSHGEOIP_BIN" --check-db /nonexistent/path.mmdb 2>&1)"
check_db_bad_rc=$?
if [ "$check_db_bad_rc" -ne 0 ] && echo "$check_db_bad_out" | grep -qi 'error'; then
    pass "--check-db on a nonexistent file fails cleanly (nonzero exit, error message)"
else
    fail "--check-db on a nonexistent file fails cleanly (rc=$check_db_bad_rc, output: $check_db_bad_out)"
fi

# --- TCP listener: off by default (every test above stays on the UNIX
# socket); when tcp_port is set, both listeners are live simultaneously and
# serve identical routes, including /health and /metrics ---
CONF="$WORKDIR/tcp.conf"
SOCK="$WORKDIR/tcp.sock"
TCP_PORT=18765
write_conf
echo "tcp_port=$TCP_PORT" >> "$CONF"

if start_daemon; then
    ipv4_status="$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$TCP_PORT/lookup?ip=8.8.8.8")"
    [ "$ipv4_status" != "000" ] \
        && pass "TCP listener (default, dual-stack) accepts IPv4 requests" \
        || fail "TCP listener (default, dual-stack) accepts IPv4 requests (got $ipv4_status)"

    ipv6_status="$(curl -sS -o /dev/null -w '%{http_code}' "http://[::1]:$TCP_PORT/lookup?ip=8.8.8.8")"
    [ "$ipv6_status" != "000" ] \
        && pass "TCP listener (default, dual-stack) accepts IPv6 requests" \
        || fail "TCP listener (default, dual-stack) accepts IPv6 requests (got $ipv6_status)"

    unix_status="$(http_status 'http://localhost/lookup?ip=8.8.8.8')"
    [ "$unix_status" != "000" ] \
        && pass "UNIX socket still works with the TCP listener also enabled" \
        || fail "UNIX socket still works with the TCP listener also enabled (got $unix_status)"

    tcp_health="$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$TCP_PORT/health")"
    tcp_metrics="$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$TCP_PORT/metrics")"
    if [ "$tcp_health" = "200" ] && [ "$tcp_metrics" = "200" ]; then
        pass "/health and /metrics are both reachable over the TCP listener too"
    else
        fail "/health and /metrics are both reachable over the TCP listener too (health=$tcp_health, metrics=$tcp_metrics)"
    fi

    stop_daemon
else
    fail "could not start nshgeoip with tcp_port configured"
fi

# --- TCP listener: explicit tcp_address binds only that one address,
# instead of the default IPv4+IPv6 loopback pair ---
CONF="$WORKDIR/tcp_explicit.conf"
SOCK="$WORKDIR/tcp_explicit.sock"
TCP_PORT2=18766
write_conf
{
    echo "tcp_port=$TCP_PORT2"
    echo "tcp_address=127.0.0.1"
} >> "$CONF"

if start_daemon; then
    ipv4_status="$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$TCP_PORT2/lookup?ip=8.8.8.8")"
    ipv6_status="$(curl -sS -o /dev/null -w '%{http_code}' --max-time 1 "http://[::1]:$TCP_PORT2/lookup?ip=8.8.8.8" 2>/dev/null)"
    if [ "$ipv4_status" != "000" ] && [ "$ipv6_status" = "000" ]; then
        pass "explicit tcp_address restricts the TCP listener to that one address family"
    else
        fail "explicit tcp_address restricts the TCP listener to that one address family (ipv4=$ipv4_status, ipv6=$ipv6_status)"
    fi
    stop_daemon
else
    fail "could not start nshgeoip with explicit tcp_address configured"
fi

echo
echo "$PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
