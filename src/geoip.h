#pragma once

// Thin wrapper around libmaxminddb: opens the Country, ASN, and City
// databases once at startup, keeps them memory-mapped for the lifetime of
// the daemon, and answers lookups against the already-open handles.
//
// A shared_mutex guards the MMDB_s handles so SIGHUP can swap in
// freshly-opened databases (see reload_country/reload_asn/reload_city)
// while lookups are concurrently in flight from worker threads: lookups
// take a shared (read) lock, reload takes a unique (write) lock.
//
// City fields are sourced only from city_db, never inferred from
// country_db. Country/continent fields work the other way: country_db is
// authoritative whenever it's configured (even though a real
// GeoLite2-City.mmdb also carries country/continent data, so two
// independently-updated databases can't silently disagree) -- but if
// country_db isn't configured at all, country/continent are read from
// city_db instead, since it's the only source available and MaxMind's
// City schema already includes those fields. This lets a setup that only
// needs city_db + asn_db (e.g. CrowdSec) skip country_db entirely.

#include <maxminddb.h>

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>

#include "ip_addr.h"

namespace nshgeoip
{

// A database's own self-reported metadata, read from libmaxminddb fresh
// every time one of GeoIpDatabases's *_metadata() accessors is called (not
// cached from open/reload time). build_time is derived from
// metadata.build_epoch, formatted to match the daemon's own log timestamps
// ("%Y-%m-%dT%H:%M:%SZ", UTC) -- see Logger::timestamp() in log.h.
// age_days/age_ms/age_ns are all (now - build_epoch) in different units,
// as of the moment they were read, so they stay accurate however long
// after open/reload they're queried -- not a one-time value frozen at
// startup. build_epoch (and so "now") only has whole-second resolution to
// begin with, so age_ms/age_ns are exact unit conversions of the same
// whole-second difference, not independently more precise measurements.
struct GeoIpDbMetadata
{
    std::string database_type; // e.g. "GeoLite2-City"
    std::uint64_t build_epoch = 0;
    std::string build_time;
    double age_days = 0.0;
    std::uint64_t age_ms = 0;
    std::uint64_t age_ns = 0;
};

struct GeoIpResult
{
    bool found = false; // true if any configured database had an entry

    std::optional<std::string> country_code;
    std::optional<std::string> country_name;
    std::optional<std::string> continent_code;

    std::optional<uint32_t> asn;
    std::optional<std::string> as_org;

    std::optional<std::string> city_name;
    std::optional<std::string> postal_code;
    std::optional<double> latitude;
    std::optional<double> longitude;
    std::optional<uint16_t> accuracy_radius;
};

class GeoIpDatabases
{
public:
    GeoIpDatabases() = default;
    ~GeoIpDatabases();

    GeoIpDatabases(const GeoIpDatabases &) = delete;
    GeoIpDatabases &operator=(const GeoIpDatabases &) = delete;

    // Opens the database at `path` as the country/ASN/city database.
    // Returns false and fills `err` on failure. Calling this again after a
    // database is already open in that slot replaces it (used at startup
    // only; use reload_* for a running daemon).
    bool open_country(const std::string &path, std::string &err);
    bool open_asn(const std::string &path, std::string &err);
    bool open_city(const std::string &path, std::string &err);

    bool has_country() const
    {
        return country_open_;
    }
    bool has_asn() const
    {
        return asn_open_;
    }
    bool has_city() const
    {
        return city_open_;
    }

    // Each database's own self-reported type/build date (see
    // GeoIpDbMetadata), read fresh from the currently-open handle --
    // reflects the last successful open/reload, not necessarily the file
    // currently on disk. std::nullopt if that database isn't open.
    std::optional<GeoIpDbMetadata> country_metadata() const;
    std::optional<GeoIpDbMetadata> asn_metadata() const;
    std::optional<GeoIpDbMetadata> city_metadata() const;

    // Opens a fresh handle at `path` and atomically swaps it in for the
    // corresponding database, closing the old handle afterwards. Safe to
    // call while lookups are concurrently running on other threads.
    // Returns false and fills `err` on failure; the previous handle keeps
    // serving lookups in that case.
    bool reload_country(const std::string &path, std::string &err);
    bool reload_asn(const std::string &path, std::string &err);
    bool reload_city(const std::string &path, std::string &err);

    // Looks up `addr` in whichever databases are open. Sets `db_error` to
    // true if libmaxminddb reported an internal lookup error (maps to
    // HTTP 500 in the caller); a syntactically valid address that simply
    // is not covered by any database is reflected as `found == false` on
    // the returned result, not as an error.
    GeoIpResult lookup(const ParsedAddr &addr, bool &db_error) const;

private:
    mutable std::shared_mutex mutex_;

    MMDB_s country_mmdb_{};
    bool country_open_ = false;

    MMDB_s asn_mmdb_{};
    bool asn_open_ = false;

    MMDB_s city_mmdb_{};
    bool city_open_ = false;
};

enum class DbMetadataFormat
{
    Table, // aligned "Label : value" lines, for a human at a terminal
    Json,  // one JSON object, keys matching libmaxminddb's own MMDB_metadata_s field names
    Ini,   // "key=value" lines (same field names as Json), for shell/grep consumption
};

// Opens the .mmdb file at `path` directly -- independent of any
// GeoIpDatabases instance, and of anything nshgeoip is actually
// configured to use -- and prints its full self-reported metadata to
// stdout in the given format: every field libmaxminddb exposes (type, IP
// version, binary format, node count, record size, build date/age,
// languages, descriptions), not just the subset GeoIpDbMetadata carries
// for startup logging. The Json/Ini formats use the original
// MMDB_metadata_s field names (database_type, ip_version,
// binary_format_major_version, node_count, ...) plus build_date/age_days,
// which libmaxminddb doesn't itself provide (derived from build_epoch). A
// standalone diagnostic entry point (see "nshgeoip --check-db PATH
// [--format json|ini]" in main.cpp), never called by the running daemon
// itself. Returns false and fills `err` if the file can't be opened as an
// MMDB.
bool print_db_metadata(const std::string &path, DbMetadataFormat format, std::string &err);

} // namespace nshgeoip
