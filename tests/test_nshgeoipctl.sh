#!/bin/bash
# Unit-style tests for nshgeoipctl.sh's pure/offline-testable functions --
# edition-name mapping, GeoIP.conf parsing, database-path resolution
# (owned_db_path/external_db_path/nshgeoip_find_db), and download_db()'s
# skip logic -- all without root, network access, or real MaxMind
# credentials. stdin is /dev/null throughout as a safety net: even a bug
# that reached an unexpected credential/sudo prompt would fail fast on EOF
# instead of hanging.
#
# Complements tests/integration_test.sh (the nshgeoip binary itself) and
# tests/test_nshgeoip.cpp (nshgeoip's own pure C++ functions). Does NOT
# test anything that needs a real MaxMind account (download_one_db()'s
# actual network calls) -- that's only ever been verified manually,
# against real credentials, during development.

set -u

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL: $1"; }

WORKDIR="$(mktemp -d /tmp/nshgeoipctl_test.XXXXXX)"
trap 'rm -rf "$WORKDIR"' EXIT

# Sourcing runs main() unconditionally at the bottom of nshgeoipctl.sh with
# whatever args follow -- "help" is a harmless read-only path, output
# discarded.
# shellcheck source=../nshgeoipctl.sh
source "$SCRIPT_DIR/nshgeoipctl.sh" help >/dev/null 2>&1 < /dev/null

# ---------------------------------------------------------------------
# Edition name / MaxMind edition_id / config key mapping
# ---------------------------------------------------------------------

[ "$(edition_id_for country)" = "GeoLite2-Country" ] \
    && pass "edition_id_for: country -> GeoLite2-Country" \
    || fail "edition_id_for: country -> GeoLite2-Country"
[ "$(edition_id_for asn)" = "GeoLite2-ASN" ] \
    && pass "edition_id_for: asn -> GeoLite2-ASN" \
    || fail "edition_id_for: asn -> GeoLite2-ASN"
[ "$(edition_id_for city)" = "GeoLite2-City" ] \
    && pass "edition_id_for: city -> GeoLite2-City" \
    || fail "edition_id_for: city -> GeoLite2-City"
edition_id_for bogus >/dev/null 2>&1 < /dev/null
[ $? -ne 0 ] && pass "edition_id_for: unknown edition fails" || fail "edition_id_for: unknown edition fails"

[ "$(config_key_for country)" = "country_db" ] \
    && pass "config_key_for: country -> country_db" \
    || fail "config_key_for: country -> country_db"
[ "$(config_key_for asn)" = "asn_db" ] \
    && pass "config_key_for: asn -> asn_db" \
    || fail "config_key_for: asn -> asn_db"
[ "$(config_key_for city)" = "city_db" ] \
    && pass "config_key_for: city -> city_db" \
    || fail "config_key_for: city -> city_db"

[ "$(normalize_edition city)" = "city" ] \
    && pass "normalize_edition: short name passes through" \
    || fail "normalize_edition: short name passes through"
[ "$(normalize_edition GeoLite2-City)" = "city" ] \
    && pass "normalize_edition: full edition_id maps to short name" \
    || fail "normalize_edition: full edition_id maps to short name"
normalize_edition GeoIP2-ISP >/dev/null 2>&1 < /dev/null
[ $? -ne 0 ] && pass "normalize_edition: an unmapped (e.g. paid) edition fails cleanly" \
    || fail "normalize_edition: an unmapped (e.g. paid) edition fails cleanly"

[ "$(default_path_for city /var/lib/GeoIP)" = "/var/lib/GeoIP/GeoLite2-City.mmdb" ] \
    && pass "default_path_for: builds <dir>/GeoLite2-<Edition>.mmdb" \
    || fail "default_path_for: builds <dir>/GeoLite2-<Edition>.mmdb"

# ---------------------------------------------------------------------
# read_geoip_conf(): parses geoipupdate's own "Key Value" format
# ---------------------------------------------------------------------

GEOIP_CONF_FIXTURE="$WORKDIR/GeoIP.conf"
cat > "$GEOIP_CONF_FIXTURE" <<'EOF'
# GeoIP.conf file for GeoIP.conf
AccountID 123456

# License key
LicenseKey abcDEF123xyz

EditionIDs GeoLite2-ASN GeoLite2-City GeoLite2-Country
DatabaseDirectory /var/lib/GeoIP
EOF

GEOIP_CONF="$GEOIP_CONF_FIXTURE"
read_geoip_conf < /dev/null
rc=$?
[ $rc -eq 0 ] && pass "read_geoip_conf: returns 0 when the file exists" \
    || fail "read_geoip_conf: returns 0 when the file exists"
[ "$GEOIP_CONF_ACCOUNT_ID" = "123456" ] && pass "read_geoip_conf: parses AccountID" \
    || fail "read_geoip_conf: parses AccountID (got '$GEOIP_CONF_ACCOUNT_ID')"
[ "$GEOIP_CONF_LICENSE_KEY" = "abcDEF123xyz" ] && pass "read_geoip_conf: parses LicenseKey" \
    || fail "read_geoip_conf: parses LicenseKey (got '$GEOIP_CONF_LICENSE_KEY')"
[ "$GEOIP_CONF_DATABASE_DIR" = "/var/lib/GeoIP" ] && pass "read_geoip_conf: parses DatabaseDirectory" \
    || fail "read_geoip_conf: parses DatabaseDirectory (got '$GEOIP_CONF_DATABASE_DIR')"
[ "${#GEOIP_CONF_EDITION_IDS[@]}" -eq 3 ] && pass "read_geoip_conf: parses all three EditionIDs entries" \
    || fail "read_geoip_conf: parses all three EditionIDs entries (got ${#GEOIP_CONF_EDITION_IDS[@]})"
[ "${GEOIP_CONF_EDITION_IDS[0]}" = "GeoLite2-ASN" ] && pass "read_geoip_conf: EditionIDs preserves order" \
    || fail "read_geoip_conf: EditionIDs preserves order (got '${GEOIP_CONF_EDITION_IDS[0]:-}')"

GEOIP_CONF="/nonexistent/GeoIP.conf"
read_geoip_conf < /dev/null
[ $? -ne 0 ] && pass "read_geoip_conf: returns 1 when the file doesn't exist" \
    || fail "read_geoip_conf: returns 1 when the file doesn't exist"
[ -z "$GEOIP_CONF_ACCOUNT_ID" ] && pass "read_geoip_conf: leaves fields empty when the file doesn't exist" \
    || fail "read_geoip_conf: leaves fields empty when the file doesn't exist"

# ---------------------------------------------------------------------
# nshgeoip_find_db() / nshgeoip_any_db_available() (default_geoip_dirs.sh)
# ---------------------------------------------------------------------

FIND_DIR1="$WORKDIR/find1"
FIND_DIR2="$WORKDIR/find2"
mkdir -p "$FIND_DIR1" "$FIND_DIR2"
touch "$FIND_DIR2/GeoLite2-City.mmdb"

found="$(nshgeoip_find_db GeoLite2-City.mmdb "$FIND_DIR1" "$FIND_DIR2" < /dev/null)"
[ "$found" = "$FIND_DIR2/GeoLite2-City.mmdb" ] \
    && pass "nshgeoip_find_db: falls through to the second directory when the first lacks the file" \
    || fail "nshgeoip_find_db: falls through to the second directory when the first lacks the file (got '$found')"

nshgeoip_find_db GeoLite2-ASN.mmdb "$FIND_DIR1" "$FIND_DIR2" >/dev/null 2>&1 < /dev/null
[ $? -ne 0 ] && pass "nshgeoip_find_db: returns 1 when no given directory has the file" \
    || fail "nshgeoip_find_db: returns 1 when no given directory has the file"

# ---------------------------------------------------------------------
# owned_db_path(): configured value wins; default_path_for() otherwise
# ---------------------------------------------------------------------

OWNED_CONF="$WORKDIR/nshgeoip_owned.conf"
echo "city_db=/custom/city.mmdb" > "$OWNED_CONF"
CONF_FILE="$OWNED_CONF"
owned="$(owned_db_path city /var/lib/GeoIP < /dev/null)"
[ "$owned" = "/custom/city.mmdb" ] \
    && pass "owned_db_path: an explicitly configured path wins over the default" \
    || fail "owned_db_path: an explicitly configured path wins over the default (got '$owned')"

owned="$(owned_db_path asn /var/lib/GeoIP < /dev/null)"
[ "$owned" = "/var/lib/GeoIP/GeoLite2-ASN.mmdb" ] \
    && pass "owned_db_path: falls back to default_path_for() when unconfigured" \
    || fail "owned_db_path: falls back to default_path_for() when unconfigured (got '$owned')"

CONF_FILE="/nonexistent/nshgeoip.conf"

# ---------------------------------------------------------------------
# external_db_path(): searches NSHGEOIP_DEFAULT_GEOIP_DIRS entries other
# than geoip_dir itself -- geoip_dir (the "owned" location) is deliberately
# excluded, since a file already there is an update candidate, not
# something to silently skip (see download_db()'s own comment).
# ---------------------------------------------------------------------

EXT_OWNED="$WORKDIR/ext_owned"
EXT_OTHER="$WORKDIR/ext_other"
mkdir -p "$EXT_OWNED" "$EXT_OTHER"
touch "$EXT_OTHER/GeoLite2-City.mmdb"
touch "$EXT_OWNED/GeoLite2-ASN.mmdb"

NSHGEOIP_DEFAULT_GEOIP_DIRS=("$EXT_OWNED" "$EXT_OTHER")
found="$(external_db_path city "$EXT_OWNED" < /dev/null)"
[ "$found" = "$EXT_OTHER/GeoLite2-City.mmdb" ] \
    && pass "external_db_path: finds a file in a genuinely external directory" \
    || fail "external_db_path: finds a file in a genuinely external directory (got '$found')"

external_db_path asn "$EXT_OWNED" >/dev/null 2>&1 < /dev/null
[ $? -ne 0 ] && pass "external_db_path: does NOT count a file at geoip_dir itself as external" \
    || fail "external_db_path: does NOT count a file at geoip_dir itself as external"

# ---------------------------------------------------------------------
# download_db()'s skip logic -- no network, no credentials, no root: every
# scenario below either resolves fully via external_db_path()/the
# city-covers-country optimization (returning before any credential
# prompt), or deliberately proceeds far enough to prove it did NOT skip,
# without actually reaching the network (empty credentials + /dev/null
# stdin fail fast with a clear error instead of hanging).
# ---------------------------------------------------------------------

CONF_FILE="/nonexistent/nshgeoip.conf"
GEOIP_CONF="/nonexistent/GeoIP.conf"
NSHGEOIP_MAXMIND_ACCOUNT_ID=""
NSHGEOIP_MAXMIND_LICENSE_KEY=""

# All three externally available (including a real Country file this time,
# unlike the city-covers-country scenario below) -- proves the external
# skip applies to country too, not just asn/city.
SKIP_ALL_OWNED="$WORKDIR/skip_all_owned"
SKIP_ALL_EXT="$WORKDIR/skip_all_ext"
mkdir -p "$SKIP_ALL_OWNED" "$SKIP_ALL_EXT"
touch "$SKIP_ALL_EXT/GeoLite2-Country.mmdb" "$SKIP_ALL_EXT/GeoLite2-ASN.mmdb" "$SKIP_ALL_EXT/GeoLite2-City.mmdb"
NSHGEOIP_DEFAULT_GEOIP_DIRS=("$SKIP_ALL_OWNED" "$SKIP_ALL_EXT")
GEOIP_DIR_DEFAULT="$SKIP_ALL_OWNED"
out="$(download_db < /dev/null 2>&1)"
rc=$?
if [ $rc -eq 0 ] && echo "$out" | grep -q "Nothing to do"; then
    pass "download_db: all editions externally available -> nothing to do, no prompt, exit 0"
else
    fail "download_db: all editions externally available -> nothing to do, no prompt, exit 0 (rc=$rc, out: $out)"
fi

# city+asn externally available, country nowhere -- auto-defaulted (no
# explicit editions given), so country gets skipped via the
# city-covers-country optimization, not via external-availability.
SKIP_CITY_OWNED="$WORKDIR/skip_city_owned"
SKIP_CITY_EXT="$WORKDIR/skip_city_ext"
mkdir -p "$SKIP_CITY_OWNED" "$SKIP_CITY_EXT"
touch "$SKIP_CITY_EXT/GeoLite2-ASN.mmdb" "$SKIP_CITY_EXT/GeoLite2-City.mmdb"
NSHGEOIP_DEFAULT_GEOIP_DIRS=("$SKIP_CITY_OWNED" "$SKIP_CITY_EXT")
GEOIP_DIR_DEFAULT="$SKIP_CITY_OWNED"
out="$(download_db < /dev/null 2>&1)"
rc=$?
if [ $rc -eq 0 ] && echo "$out" | grep -q "city already supplies country/continent data" && echo "$out" | grep -q "Nothing to do"; then
    pass "download_db: auto-defaulted run skips country via city-covers-country, not just external-availability"
else
    fail "download_db: auto-defaulted run skips country via city-covers-country, not just external-availability (rc=$rc, out: $out)"
fi

# Same city/asn availability, but country requested EXPLICITLY -- must NOT
# be skipped this time (auto_defaulted=0), so it proceeds toward asking
# for credentials instead of silently doing nothing.
out="$(download_db country < /dev/null 2>&1)"
rc=$?
if [ $rc -ne 0 ] && ! echo "$out" | grep -q "Nothing to do" && echo "$out" | grep -qi "account ID and license key are both required"; then
    pass "download_db: an explicitly requested edition is never skipped, even if city is available"
else
    fail "download_db: an explicitly requested edition is never skipped, even if city is available (rc=$rc, out: $out)"
fi

# Unknown edition name is rejected before anything else happens (no
# credential prompt, no network).
out="$(download_db bogus < /dev/null 2>&1)"
rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -qi "unknown database edition" \
    && pass "download_db: an unknown edition name is rejected cleanly" \
    || fail "download_db: an unknown edition name is rejected cleanly (rc=$rc, out: $out)"

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
