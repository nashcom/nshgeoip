// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "geoip.h"

#include <cstdio>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>

#include "text_util.h"

namespace nshgeoip
{

namespace
{

bool open_mmdb(const std::string &path, MMDB_s &out, std::string &err)
{
    int status = MMDB_open(path.c_str(), MMDB_MODE_MMAP, &out);
    if (status != MMDB_SUCCESS)
    {
        err = "failed to open database '" + path + "': " + MMDB_strerror(status);
        return false;
    }
    return true;
}

// MMDB_get_value()'s path is a varargs list terminated by a NULL pointer,
// read back with va_arg(..., const char *). A bare `NULL`/0 literal risks
// being passed as a plain int on some ABIs, which va_arg would then
// mis-read as a pointer; an explicitly-typed pointer sentinel avoids that.
constexpr const char *kPathEnd = nullptr;

std::optional<std::string> get_string(MMDB_entry_s &entry, const char *k1, const char *k2 = kPathEnd,
                                      const char *k3 = kPathEnd)
{
    MMDB_entry_data_s data;
    int status = MMDB_get_value(&entry, &data, k1, k2, k3, kPathEnd);
    if (status == MMDB_SUCCESS && data.has_data && data.type == MMDB_DATA_TYPE_UTF8_STRING)
    {
        return std::string(data.utf8_string, data.data_size);
    }
    return std::nullopt;
}

std::optional<uint32_t> get_uint32(MMDB_entry_s &entry, const char *k1)
{
    MMDB_entry_data_s data;
    int status = MMDB_get_value(&entry, &data, k1, kPathEnd);
    if (status == MMDB_SUCCESS && data.has_data && data.type == MMDB_DATA_TYPE_UINT32)
    {
        return data.uint32;
    }
    return std::nullopt;
}

std::optional<uint16_t> get_uint16(MMDB_entry_s &entry, const char *k1, const char *k2)
{
    MMDB_entry_data_s data;
    int status = MMDB_get_value(&entry, &data, k1, k2, kPathEnd);
    if (status == MMDB_SUCCESS && data.has_data && data.type == MMDB_DATA_TYPE_UINT16)
    {
        return data.uint16;
    }
    return std::nullopt;
}

std::optional<double> get_double(MMDB_entry_s &entry, const char *k1, const char *k2)
{
    MMDB_entry_data_s data;
    int status = MMDB_get_value(&entry, &data, k1, k2, kPathEnd);
    if (status == MMDB_SUCCESS && data.has_data && data.type == MMDB_DATA_TYPE_DOUBLE)
    {
        return data.double_value;
    }
    return std::nullopt;
}

// Matches Logger::timestamp()'s format in log.h, so a build date reads
// consistently with the rest of the daemon's log output.
std::string format_build_time(std::uint64_t build_epoch)
{
    std::time_t t = static_cast<std::time_t>(build_epoch);
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

GeoIpDbMetadata make_metadata(const MMDB_s &mmdb)
{
    GeoIpDbMetadata info;
    info.database_type = mmdb.metadata.database_type != nullptr ? mmdb.metadata.database_type : "";
    info.build_epoch = mmdb.metadata.build_epoch;
    info.build_time = format_build_time(mmdb.metadata.build_epoch);

    std::time_t build_time = static_cast<std::time_t>(mmdb.metadata.build_epoch);
    std::time_t now = std::time(nullptr);
    double age_seconds = std::difftime(now, build_time);

    info.age_days = age_seconds / 86400.0;
    info.age_ms = static_cast<std::uint64_t>(age_seconds * 1e3);
    info.age_ns = static_cast<std::uint64_t>(age_seconds * 1e9);

    return info;
}

std::string join_languages(const MMDB_metadata_s &meta, const char *sep)
{
    std::string joined;
    for (std::size_t i = 0; i < meta.languages.count; ++i)
    {
        if (i > 0)
        {
            joined += sep;
        }
        joined += meta.languages.names[i];
    }
    return joined;
}

// The "en" entry out of meta.description -- the one actually wanted most
// of the time, so callers don't have to search the full array themselves
// for the common case. Empty if there's no "en" entry (real GeoLite2/GeoIP2
// databases always have one, but nothing here assumes that).
std::string english_description(const MMDB_metadata_s &meta)
{
    for (std::size_t i = 0; i < meta.description.count; ++i)
    {
        const MMDB_description_s *d = meta.description.descriptions[i];
        if (d->language != nullptr && std::string(d->language) == "en")
        {
            return d->description != nullptr ? d->description : "";
        }
    }
    return "";
}

// Aligned "Label : value" lines for a human at a terminal. Every row uses
// the same label width, including the per-language Description rows, so
// the colons line up -- a longer label (e.g. a multi-part language code)
// just pushes its own colon right rather than throwing off the others.
void print_metadata_table(const std::string &path, const MMDB_s &mmdb, const GeoIpDbMetadata &info)
{
    const MMDB_metadata_s &meta = mmdb.metadata;

    auto row = [](const std::string &label, const std::string &value) { std::printf("%-16s: %s\n", label.c_str(), value.c_str()); };

    row("Path", path);
    row("libmaxminddb", MMDB_lib_version());
    row("Database type", info.database_type);
    row("IP version", std::to_string(meta.ip_version));
    row("Binary format", std::to_string(meta.binary_format_major_version) + "." +
                              std::to_string(meta.binary_format_minor_version));
    row("Node count", std::to_string(meta.node_count));
    row("Record size", std::to_string(meta.record_size) + " bits");
    row("Build epoch", std::to_string(meta.build_epoch));
    row("Build date", info.build_time);

    std::ostringstream age;
    age << std::fixed << std::setprecision(1) << info.age_days << " days";
    row("Age", age.str());
    row("Age (ms)", std::to_string(info.age_ms));
    row("Age (ns)", std::to_string(info.age_ns));

    if (meta.languages.count > 0)
    {
        row("Languages", join_languages(meta, ", "));
    }

    for (std::size_t i = 0; i < meta.description.count; ++i)
    {
        const MMDB_description_s *d = meta.description.descriptions[i];
        row("Description [" + std::string(d->language) + "]", d->description);
    }
}

// One JSON object, keys matching libmaxminddb's own MMDB_metadata_s field
// names, plus build_date/age_days (derived from build_epoch, not
// themselves part of the struct).
void print_metadata_json(const std::string &path, const MMDB_s &mmdb, const GeoIpDbMetadata &info)
{
    const MMDB_metadata_s &meta = mmdb.metadata;

    std::ostringstream out;
    out << "{";
    out << "\"path\":\"" << json_escape(path) << "\"";
    out << ",\"libmaxminddb_version\":\"" << json_escape(MMDB_lib_version()) << "\"";
    out << ",\"database_type\":\"" << json_escape(info.database_type) << "\"";
    out << ",\"ip_version\":" << meta.ip_version;
    out << ",\"binary_format_major_version\":" << meta.binary_format_major_version;
    out << ",\"binary_format_minor_version\":" << meta.binary_format_minor_version;
    out << ",\"node_count\":" << meta.node_count;
    out << ",\"record_size\":" << meta.record_size;
    out << ",\"build_epoch\":" << meta.build_epoch;
    out << ",\"build_date\":\"" << json_escape(info.build_time) << "\"";
    out << ",\"age_days\":" << std::fixed << std::setprecision(1) << info.age_days;
    out << ",\"age_ms\":" << info.age_ms;
    out << ",\"age_ns\":" << info.age_ns;

    out << ",\"languages\":[";
    for (std::size_t i = 0; i < meta.languages.count; ++i)
    {
        if (i > 0)
        {
            out << ",";
        }
        out << "\"" << json_escape(meta.languages.names[i]) << "\"";
    }
    out << "]";

    // "description": the English text alone -- what's wanted most of the
    // time, so callers don't have to search "description_list" themselves
    // for the common case. Empty string if there's no "en" entry.
    out << ",\"description\":\"" << json_escape(english_description(meta)) << "\"";

    // "description_list": every language, as an array of {language,
    // description} objects rather than a {lang: text} map --
    // MMDB_description_s itself has exactly those two fields per entry, so
    // this mirrors the underlying struct shape directly rather than
    // collapsing it into a keyed object.
    out << ",\"description_list\":[";
    for (std::size_t i = 0; i < meta.description.count; ++i)
    {
        const MMDB_description_s *d = meta.description.descriptions[i];
        if (i > 0)
        {
            out << ",";
        }
        out << "{\"language\":\"" << json_escape(d->language) << "\",\"description\":\"" << json_escape(d->description)
            << "\"}";
    }
    out << "]";
    out << "}";

    std::printf("%s\n", out.str().c_str());
}

// "key=value" lines, same field names as the Json format, for
// shell/grep consumption -- multi-language descriptions become
// "description_<lang>=..." lines since INI has no native nested object,
// alongside a plain "description=" line for the English text alone (what's
// wanted most of the time). String values go through
// sanitize_header_value() since an MMDB file is untrusted input, same as
// the HTTP API's own INI body (see text_util.h).
void print_metadata_ini(const std::string &path, const MMDB_s &mmdb, const GeoIpDbMetadata &info)
{
    const MMDB_metadata_s &meta = mmdb.metadata;

    std::printf("path=%s\n", sanitize_header_value(path).c_str());
    std::printf("libmaxminddb_version=%s\n", MMDB_lib_version());
    std::printf("database_type=%s\n", sanitize_header_value(info.database_type).c_str());
    std::printf("ip_version=%u\n", meta.ip_version);
    std::printf("binary_format_major_version=%u\n", meta.binary_format_major_version);
    std::printf("binary_format_minor_version=%u\n", meta.binary_format_minor_version);
    std::printf("node_count=%u\n", meta.node_count);
    std::printf("record_size=%u\n", meta.record_size);
    std::printf("build_epoch=%llu\n", static_cast<unsigned long long>(meta.build_epoch));
    std::printf("build_date=%s\n", info.build_time.c_str());
    std::printf("age_days=%.1f\n", info.age_days);
    std::printf("age_ms=%llu\n", static_cast<unsigned long long>(info.age_ms));
    std::printf("age_ns=%llu\n", static_cast<unsigned long long>(info.age_ns));

    if (meta.languages.count > 0)
    {
        std::printf("languages=%s\n", sanitize_header_value(join_languages(meta, ",")).c_str());
    }

    std::printf("description=%s\n", sanitize_header_value(english_description(meta)).c_str());

    for (std::size_t i = 0; i < meta.description.count; ++i)
    {
        const MMDB_description_s *d = meta.description.descriptions[i];
        std::printf("description_%s=%s\n", d->language, sanitize_header_value(d->description).c_str());
    }
}

} // namespace

GeoIpDatabases::~GeoIpDatabases()
{
    if (country_open_)
    {
        MMDB_close(&country_mmdb_);
    }
    if (asn_open_)
    {
        MMDB_close(&asn_mmdb_);
    }
    if (city_open_)
    {
        MMDB_close(&city_mmdb_);
    }
}

bool GeoIpDatabases::open_country(const std::string &path, std::string &err)
{
    MMDB_s tmp{};
    if (!open_mmdb(path, tmp, err))
    {
        return false;
    }

    std::unique_lock lock(mutex_);
    if (country_open_)
    {
        MMDB_close(&country_mmdb_);
    }
    country_mmdb_ = tmp;
    country_open_ = true;
    return true;
}

bool GeoIpDatabases::open_asn(const std::string &path, std::string &err)
{
    MMDB_s tmp{};
    if (!open_mmdb(path, tmp, err))
    {
        return false;
    }

    std::unique_lock lock(mutex_);
    if (asn_open_)
    {
        MMDB_close(&asn_mmdb_);
    }
    asn_mmdb_ = tmp;
    asn_open_ = true;
    return true;
}

bool GeoIpDatabases::open_city(const std::string &path, std::string &err)
{
    MMDB_s tmp{};
    if (!open_mmdb(path, tmp, err))
    {
        return false;
    }

    std::unique_lock lock(mutex_);
    if (city_open_)
    {
        MMDB_close(&city_mmdb_);
    }
    city_mmdb_ = tmp;
    city_open_ = true;
    return true;
}

bool GeoIpDatabases::reload_country(const std::string &path, std::string &err)
{
    return open_country(path, err);
}

bool GeoIpDatabases::reload_asn(const std::string &path, std::string &err)
{
    return open_asn(path, err);
}

bool GeoIpDatabases::reload_city(const std::string &path, std::string &err)
{
    return open_city(path, err);
}

GeoIpResult GeoIpDatabases::lookup(const ParsedAddr &addr, bool &db_error) const
{
    GeoIpResult result;
    db_error = false;

    std::shared_lock lock(mutex_);

    if (country_open_)
    {
        int mmdb_err = MMDB_SUCCESS;
        MMDB_lookup_result_s res = MMDB_lookup_sockaddr(&country_mmdb_, addr.sockaddr_ptr(), &mmdb_err);
        if (mmdb_err != MMDB_SUCCESS)
        {
            db_error = true;
        }
        else if (res.found_entry)
        {
            result.found = true;
            result.country_code = get_string(res.entry, "country", "iso_code");
            result.country_name = get_string(res.entry, "country", "names", "en");
            result.continent_code = get_string(res.entry, "continent", "code");
        }
    }

    if (asn_open_ && !db_error)
    {
        int mmdb_err = MMDB_SUCCESS;
        MMDB_lookup_result_s res = MMDB_lookup_sockaddr(&asn_mmdb_, addr.sockaddr_ptr(), &mmdb_err);
        if (mmdb_err != MMDB_SUCCESS)
        {
            db_error = true;
        }
        else if (res.found_entry)
        {
            result.found = true;
            result.asn = get_uint32(res.entry, "autonomous_system_number");
            result.as_org = get_string(res.entry, "autonomous_system_organization");
        }
    }

    if (city_open_ && !db_error)
    {
        int mmdb_err = MMDB_SUCCESS;
        MMDB_lookup_result_s res = MMDB_lookup_sockaddr(&city_mmdb_, addr.sockaddr_ptr(), &mmdb_err);
        if (mmdb_err != MMDB_SUCCESS)
        {
            db_error = true;
        }
        else if (res.found_entry)
        {
            result.found = true;
            result.city_name = get_string(res.entry, "city", "names", "en");
            result.postal_code = get_string(res.entry, "postal", "code");
            result.latitude = get_double(res.entry, "location", "latitude");
            result.longitude = get_double(res.entry, "location", "longitude");
            result.accuracy_radius = get_uint16(res.entry, "location", "accuracy_radius");

            // country_db is authoritative for country/continent fields
            // whenever it's configured (see the class comment) -- but when
            // it isn't, city_db is the only source available, and a real
            // GeoLite2-City.mmdb carries the same country/continent fields
            // GeoLite2-Country.mmdb does. This lets a city_db + asn_db-only
            // setup (e.g. CrowdSec, which only needs those two) skip
            // country_db entirely without losing country/continent data.
            if (!country_open_)
            {
                result.country_code = get_string(res.entry, "country", "iso_code");
                result.country_name = get_string(res.entry, "country", "names", "en");
                result.continent_code = get_string(res.entry, "continent", "code");
            }
        }
    }

    return result;
}

std::optional<GeoIpDbMetadata> GeoIpDatabases::country_metadata() const
{
    std::shared_lock lock(mutex_);
    if (!country_open_)
    {
        return std::nullopt;
    }
    return make_metadata(country_mmdb_);
}

std::optional<GeoIpDbMetadata> GeoIpDatabases::asn_metadata() const
{
    std::shared_lock lock(mutex_);
    if (!asn_open_)
    {
        return std::nullopt;
    }
    return make_metadata(asn_mmdb_);
}

std::optional<GeoIpDbMetadata> GeoIpDatabases::city_metadata() const
{
    std::shared_lock lock(mutex_);
    if (!city_open_)
    {
        return std::nullopt;
    }
    return make_metadata(city_mmdb_);
}

bool print_db_metadata(const std::string &path, DbMetadataFormat format, std::string &err)
{
    MMDB_s mmdb{};
    if (!open_mmdb(path, mmdb, err))
    {
        return false;
    }

    GeoIpDbMetadata info = make_metadata(mmdb);

    switch (format)
    {
    case DbMetadataFormat::Table:
        print_metadata_table(path, mmdb, info);
        break;
    case DbMetadataFormat::Json:
        print_metadata_json(path, mmdb, info);
        break;
    case DbMetadataFormat::Ini:
        print_metadata_ini(path, mmdb, info);
        break;
    }

    MMDB_close(&mmdb);
    return true;
}

} // namespace nshgeoip
