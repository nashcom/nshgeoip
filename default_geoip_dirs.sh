#!/bin/bash
# Shared with kDefaultGeoipDirs in src/main.cpp -- keep the two lists in
# sync if either changes. Sourced (never executed directly) by
# nshgeoipctl.sh (download-db's "already available" pre-check) and
# tests/integration_test.sh (deciding whether a real database exists to
# test the auto-detect feature against), so neither one re-declares its
# own separate copy of where nshgeoip looks for a GeoLite2 database when
# country_db/asn_db/city_db aren't configured -- see
# apply_default_db_paths() in src/config.cpp for the actual logic this
# list feeds.

NSHGEOIP_DEFAULT_GEOIP_DIRS=(
    "/var/lib/GeoIP"         # geoipupdate's own default DatabaseDirectory
    "/var/lib/crowdsec/data" # CrowdSec's bundled copy (GeoLite2-ASN/-City only, no Country)
)

# Prints the first "<dir>/<filename>" that exists and is readable by this
# user, searching each of the given directories (or, if none are given,
# NSHGEOIP_DEFAULT_GEOIP_DIRS) in order -- mirrors resolve_db_slot() in
# src/config.cpp. Returns 1 with nothing printed if none exist. Read-only:
# never used to decide where to write a file, only whether one is already
# available somewhere nshgeoip would find it on its own.
nshgeoip_find_db()
{
    local filename="$1"
    shift
    local dirs=("$@")
    [ ${#dirs[@]} -eq 0 ] && dirs=("${NSHGEOIP_DEFAULT_GEOIP_DIRS[@]}")

    local dir candidate
    for dir in "${dirs[@]}"; do
        candidate="$dir/$filename"
        if [ -r "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

# Returns 0 if any of GeoLite2-{Country,ASN,City}.mmdb is auto-detectable
# (readable) across NSHGEOIP_DEFAULT_GEOIP_DIRS, 1 otherwise. Convenience
# wrapper around nshgeoip_find_db() for callers that just need a yes/no
# answer without caring which specific file or directory matched -- e.g.
# integration_test.sh deciding whether it can run at all with no
# TEST_COUNTRY_DB/TEST_ASN_DB/TEST_CITY_DB set.
nshgeoip_any_db_available()
{
    local filename
    for filename in GeoLite2-City.mmdb GeoLite2-ASN.mmdb GeoLite2-Country.mmdb; do
        nshgeoip_find_db "$filename" >/dev/null && return 0
    done
    return 1
}
