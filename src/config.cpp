// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "config.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace nshgeoip
{

int default_thread_count()
{
    constexpr int kFloor = 4;
    constexpr int kCap = 20;
    constexpr int kFallback = 8; // hardware_concurrency() is allowed to return 0 when it can't tell

    unsigned int detected = std::thread::hardware_concurrency();
    if (detected == 0)
    {
        return kFallback;
    }

    int n = static_cast<int>(detected);
    if (n < kFloor)
    {
        return kFloor;
    }
    if (n > kCap)
    {
        return kCap;
    }
    return n;
}

namespace
{

std::string trim(const std::string &s)
{
    size_t start = 0;
    size_t end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
    {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
    {
        --end;
    }
    return s.substr(start, end - start);
}

bool parse_long(const std::string &value, long &out, int base = 10)
{
    if (value.empty())
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    long v = std::strtol(value.c_str(), &end, base);
    if (errno != 0 || end == value.c_str() || *end != '\0')
    {
        return false;
    }
    out = v;
    return true;
}

bool parse_bool(const std::string &value, bool &out)
{
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    if (v == "1" || v == "true" || v == "yes" || v == "on")
    {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off")
    {
        out = false;
        return true;
    }
    return false;
}

} // namespace

bool parse_port(const std::string &text, int &out)
{
    long v = 0;
    if (!parse_long(text, v) || v < 1 || v > 65535)
    {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool load_config(const std::string &path, Config &cfg, std::string &err, std::vector<std::string> *warnings)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        err = "cannot open config file: " + path;
        return false;
    }

    std::string line;
    int line_no = 0;
    while (std::getline(file, line))
    {
        ++line_no;
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#')
        {
            continue;
        }

        size_t eq = trimmed.find('=');
        if (eq == std::string::npos)
        {
            err = path + ":" + std::to_string(line_no) + ": expected key=value";
            return false;
        }

        std::string key = trim(trimmed.substr(0, eq));
        std::string value = trim(trimmed.substr(eq + 1));

        if (key == "country_db")
        {
            cfg.country_db = value;
        }
        else if (key == "asn_db")
        {
            cfg.asn_db = value;
        }
        else if (key == "city_db")
        {
            cfg.city_db = value;
        }
        else if (key == "socket")
        {
            cfg.socket_path = value;
        }
        else if (key == "socket_mode")
        {
            long mode = 0;
            if (!parse_long(value, mode, 8) || mode < 0 || mode > 07777)
            {
                err = path + ":" + std::to_string(line_no) + ": invalid socket_mode (expected octal, e.g. 0660)";
                return false;
            }
            cfg.socket_mode = static_cast<mode_t>(mode);
        }
        else if (key == "threads")
        {
            long threads = 0;
            if (!parse_long(value, threads) || threads < 1 || threads > 256)
            {
                err = path + ":" + std::to_string(line_no) + ": invalid threads (expected 1-256)";
                return false;
            }
            cfg.threads = static_cast<int>(threads);
        }
        else if (key == "max_request_bytes")
        {
            long bytes = 0;
            if (!parse_long(value, bytes) || bytes < 256 || bytes > (1 << 20))
            {
                err = path + ":" + std::to_string(line_no) + ": invalid max_request_bytes (expected 256-1048576)";
                return false;
            }
            cfg.max_request_bytes = static_cast<std::size_t>(bytes);
        }
        else if (key == "debug_log")
        {
            bool debug = false;
            if (!parse_bool(value, debug))
            {
                err = path + ":" + std::to_string(line_no) + ": invalid debug_log (expected true/false)";
                return false;
            }
            cfg.debug_log = debug;
        }
        else if (key == "tcp_port")
        {
            int port = 0;
            if (!parse_port(value, port))
            {
                err = path + ":" + std::to_string(line_no) + ": invalid tcp_port (expected 1-65535)";
                return false;
            }
            cfg.tcp_port = port;
        }
        else if (key == "tcp_address")
        {
            cfg.tcp_address = value;
        }
        else if (key == "metrics_file")
        {
            cfg.metrics_file = value;
        }
        else if (key == "metrics_interval_seconds")
        {
            long interval = 0;
            if (!parse_long(value, interval) || interval < 1 || interval > 86400)
            {
                err = path + ":" + std::to_string(line_no) + ": invalid metrics_interval_seconds (expected 1-86400)";
                return false;
            }
            cfg.metrics_interval_seconds = static_cast<int>(interval);
        }
        else
        {
            // Unknown key: ignored, not fatal, so newer/older config files
            // stay compatible. Caller may still want to log this.
            if (warnings != nullptr)
            {
                warnings->push_back(path + ":" + std::to_string(line_no) + ": unknown config key '" + key + "'");
            }
            continue;
        }
    }

    return true;
}

namespace
{

bool env_value(const char *name, std::string &out)
{
    const char *v = std::getenv(name);
    if (v == nullptr)
    {
        return false;
    }
    out = v;
    return true;
}

} // namespace

bool apply_env_overrides(Config &cfg, std::string &err)
{
    std::string value;

    if (env_value("NSHGEOIP_COUNTRY_DB", value))
    {
        cfg.country_db = value;
    }
    if (env_value("NSHGEOIP_ASN_DB", value))
    {
        cfg.asn_db = value;
    }
    if (env_value("NSHGEOIP_CITY_DB", value))
    {
        cfg.city_db = value;
    }
    if (env_value("NSHGEOIP_SOCKET", value))
    {
        cfg.socket_path = value;
    }

    if (env_value("NSHGEOIP_SOCKET_MODE", value))
    {
        long mode = 0;
        if (!parse_long(value, mode, 8) || mode < 0 || mode > 07777)
        {
            err = "NSHGEOIP_SOCKET_MODE: invalid value (expected octal, e.g. 0660)";
            return false;
        }
        cfg.socket_mode = static_cast<mode_t>(mode);
    }

    if (env_value("NSHGEOIP_THREADS", value))
    {
        long threads = 0;
        if (!parse_long(value, threads) || threads < 1 || threads > 256)
        {
            err = "NSHGEOIP_THREADS: invalid value (expected 1-256)";
            return false;
        }
        cfg.threads = static_cast<int>(threads);
    }

    if (env_value("NSHGEOIP_MAX_REQUEST_BYTES", value))
    {
        long bytes = 0;
        if (!parse_long(value, bytes) || bytes < 256 || bytes > (1 << 20))
        {
            err = "NSHGEOIP_MAX_REQUEST_BYTES: invalid value (expected 256-1048576)";
            return false;
        }
        cfg.max_request_bytes = static_cast<std::size_t>(bytes);
    }

    if (env_value("NSHGEOIP_DEBUG_LOG", value))
    {
        bool debug = false;
        if (!parse_bool(value, debug))
        {
            err = "NSHGEOIP_DEBUG_LOG: invalid value (expected true/false)";
            return false;
        }
        cfg.debug_log = debug;
    }

    if (env_value("NSHGEOIP_TCP_PORT", value))
    {
        int port = 0;
        if (!parse_port(value, port))
        {
            err = "NSHGEOIP_TCP_PORT: invalid value (expected 1-65535)";
            return false;
        }
        cfg.tcp_port = port;
    }
    if (env_value("NSHGEOIP_TCP_ADDRESS", value))
    {
        cfg.tcp_address = value;
    }

    if (env_value("NSHGEOIP_METRICS_FILE", value))
    {
        cfg.metrics_file = value;
    }
    if (env_value("NSHGEOIP_METRICS_INTERVAL_SECONDS", value))
    {
        long interval = 0;
        if (!parse_long(value, interval) || interval < 1 || interval > 86400)
        {
            err = "NSHGEOIP_METRICS_INTERVAL_SECONDS: invalid value (expected 1-86400)";
            return false;
        }
        cfg.metrics_interval_seconds = static_cast<int>(interval);
    }

    return true;
}

namespace
{

bool regular_file_exists(const std::string &path)
{
    struct stat st
    {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Fills `field` with the first "<dir>/<filename>" (searched in the given
// order) that actually exists, doing nothing if `field` is already set or
// no candidate exists. Appends a note on an actual substitution.
void resolve_db_slot(std::string &field, const char *key, const char *filename,
                      const std::vector<std::string> &geoip_dirs, std::vector<std::string> *notes)
{
    if (!field.empty())
    {
        return;
    }

    for (const std::string &dir : geoip_dirs)
    {
        std::string candidate = dir + "/" + filename;
        if (regular_file_exists(candidate))
        {
            field = candidate;
            if (notes != nullptr)
            {
                notes->push_back(std::string(key) + " not set, using " + candidate + " (auto-detected)");
            }
            return;
        }
    }
}

} // namespace

void apply_default_db_paths(Config &cfg, const std::vector<std::string> &geoip_dirs, std::vector<std::string> *notes)
{
    // city_db is resolved first, and country_db's search is skipped
    // entirely once city_db ends up set (whether it was already configured
    // or just found here): a real GeoLite2-City.mmdb already carries
    // country/continent data, and GeoIpDatabases already falls back to
    // reading those fields from city_db when country_db isn't configured
    // (see geoip.cpp) -- so opening a whole separate Country database on
    // top would be pure overhead for no new information. This is exactly
    // the CrowdSec case this auto-detection exists for: it ships
    // GeoLite2-ASN/-City, never GeoLite2-Country.
    resolve_db_slot(cfg.city_db, "city_db", "GeoLite2-City.mmdb", geoip_dirs, notes);
    resolve_db_slot(cfg.asn_db, "asn_db", "GeoLite2-ASN.mmdb", geoip_dirs, notes);

    if (!cfg.city_db.empty())
    {
        if (cfg.country_db.empty() && notes != nullptr)
        {
            notes->push_back("country_db not set, skipping auto-detect: city_db already supplies country/continent data");
        }
        return;
    }

    resolve_db_slot(cfg.country_db, "country_db", "GeoLite2-Country.mmdb", geoip_dirs, notes);
}

} // namespace nshgeoip
