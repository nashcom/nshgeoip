// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "geoip.h"
#include "log.h"
#include "server.h"
#include "version.h"

namespace
{

const char *kDefaultConfigPath = "/etc/nshgeoip/nshgeoip.conf";

// Standard locations to search (in order) for GeoLite2 databases not
// otherwise configured. /var/lib/GeoIP is geoipupdate's own default
// DatabaseDirectory (confirmed against a real "geoipupdate -v" run: "Using
// database directory /var/lib/GeoIP") -- also what nshgeoipctl.sh
// download-db uses. /var/lib/crowdsec/data is where CrowdSec bundles its
// own copy of GeoLite2-ASN/-City (no Country -- nshgeoip already derives
// country/continent from city_db when country_db isn't set, so that's not
// a gap). See apply_default_db_paths().
const std::vector<std::string> kDefaultGeoipDirs = {
    "/var/lib/GeoIP",
    "/var/lib/crowdsec/data",
};

struct ConfigParam
{
    const char *key;     // config file key, e.g. "tcp_port="
    const char *env;     // equivalent NSHGEOIP_* environment variable
    const char *desc;    // short description
    const char *default_; // value when neither the config file nor the environment sets it
};

// clang-format off
const ConfigParam kParams[] = {
    {"country_db=",         "NSHGEOIP_COUNTRY_DB",        "Country MMDB path",                                "disabled"},
    {"asn_db=",              "NSHGEOIP_ASN_DB",            "ASN MMDB path",                                    "disabled"},
    {"city_db=",             "NSHGEOIP_CITY_DB",           "City MMDB path",                                   "disabled"},
    {"socket=",              "NSHGEOIP_SOCKET",            "UNIX socket path",                                 "/run/nshgeoip/nshgeoip.sock"},
    {"socket_mode=",         "NSHGEOIP_SOCKET_MODE",       "UNIX socket permissions (octal)",                  "0660"},
    {"threads=",             "NSHGEOIP_THREADS",           "Worker thread pool size (1-256)",                  "CPU cores, clamped to 4-20"},
    {"max_request_bytes=",   "NSHGEOIP_MAX_REQUEST_BYTES", "Max request header bytes (256-1048576)",           "8192"},
    {"debug_log=",           "NSHGEOIP_DEBUG_LOG",         "Log rejected/malformed requests",                  "false"},
    {"tcp_port=",            "NSHGEOIP_TCP_PORT",          "Optional TCP listener port (1-65535), 0=disabled", "0"},
    {"tcp_address=",         "NSHGEOIP_TCP_ADDRESS",       "TCP bind address, empty=IPv4+IPv6 loopback",       "(loopback)"},
    {"metrics_file=",        "NSHGEOIP_METRICS_FILE",      "Path to periodically write Prometheus metrics to", "disabled"},
    {"metrics_interval_seconds=", "NSHGEOIP_METRICS_INTERVAL_SECONDS", "How often to write metrics_file (1-86400)", "60"},
};
// clang-format on

void print_help(const char *argv0)
{
    std::printf("nshgeoip is a small local GeoIP lookup daemon. It answers\n"
                "'GET /lookup?ip=<address>' over a UNIX domain socket (and, optionally,\n"
                "TCP) using MaxMind MMDB databases, for use with NGINX auth_request or\n"
                "any other local client. It also always answers 'GET /health' (a minimal\n"
                "text status by default, or JSON with 'Accept: application/json') and\n"
                "'GET /metrics' (Prometheus text format) -- neither has its own on/off\n"
                "switch; restrict access with NGINX (or your firewall) if needed.\n"
                "\n"
                "Usage: %s [--config PATH] | --check-db PATH [--format table|json|ini] | --version | --help\n"
                "\n"
                "  --config PATH        Path to config file (default: %s)\n"
                "  --check-db PATH      Print an .mmdb file's own metadata (type, build date/age,\n"
                "                       node count, languages, ...) and exit -- a diagnostic mode,\n"
                "                       independent of any configured country_db/asn_db/city_db\n"
                "  --format FORMAT      table (default), json, or ini -- only with --check-db;\n"
                "                       json/ini use libmaxminddb's own field names (database_type,\n"
                "                       ip_version, node_count, ...) plus build_date/age_days\n"
                "  --version            Print version and exit\n"
                "  --help               Print this help and exit\n"
                "\n"
                "Every config file key below also has an NSHGEOIP_<KEY> environment\n"
                "variable that overrides it. The config file itself is optional at the\n"
                "default path (but not when --config names a path explicitly), so a\n"
                "container can run on environment variables alone. Precedence:\n"
                "environment > config file > default.\n"
                "\n",
                argv0, kDefaultConfigPath);

    std::printf("%-27s %-35s %-52s %s\n", "CONFIG KEY", "ENVIRONMENT VARIABLE", "DESCRIPTION", "DEFAULT");
    for (const auto &p : kParams)
    {
        std::printf("%-27s %-35s %-52s %s\n", p.key, p.env, p.desc, p.default_);
    }

    std::printf("\n"
                "Any of country_db/asn_db/city_db left unset (by all of the above) falls\n"
                "back to GeoLite2-{Country,ASN,City}.mmdb under the first of these\n"
                "directories where that file actually exists (checked at startup, and\n"
                "logged when a fallback is used):\n");
    for (const auto &dir : kDefaultGeoipDirs)
    {
        std::printf("  %s\n", dir.c_str());
    }
    std::printf("country_db's auto-detection is skipped entirely once city_db ends up set\n"
                "(by config or by this same auto-detection) -- a real GeoLite2-City.mmdb\n"
                "already carries country/continent data, so there is nothing a separate\n"
                "Country database would add.\n");
}

} // namespace

int main(int argc, char **argv)
{
    std::string config_path = kDefaultConfigPath;
    bool config_path_explicit = false;

    bool check_db_requested = false;
    std::string check_db_path;
    std::string check_db_format = "table";

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--version")
        {
            std::printf("nshgeoip %s\n", NSHGEOIP_VERSION);
            return 0;
        }
        if (arg == "--help" || arg == "-h")
        {
            print_help(argv[0]);
            return 0;
        }
        if (arg == "--check-db")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--check-db requires a path argument\n");
                return 2;
            }
            check_db_path = argv[++i];
            check_db_requested = true;
            continue;
        }
        if (arg == "--format")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--format requires a value (table, json, or ini)\n");
                return 2;
            }
            check_db_format = argv[++i];
            continue;
        }
        if (arg == "--config")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--config requires a path argument\n");
                return 2;
            }
            config_path = argv[++i];
            config_path_explicit = true;
            continue;
        }
        std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
        print_help(argv[0]);
        return 2;
    }

    if (check_db_requested)
    {
        nshgeoip::DbMetadataFormat format;
        if (check_db_format == "table")
        {
            format = nshgeoip::DbMetadataFormat::Table;
        }
        else if (check_db_format == "json")
        {
            format = nshgeoip::DbMetadataFormat::Json;
        }
        else if (check_db_format == "ini")
        {
            format = nshgeoip::DbMetadataFormat::Ini;
        }
        else
        {
            std::fprintf(stderr, "unknown --format value: %s (expected table, json, or ini)\n", check_db_format.c_str());
            return 2;
        }

        std::string db_err;
        if (!nshgeoip::print_db_metadata(check_db_path, format, db_err))
        {
            std::fprintf(stderr, "Error: %s\n", db_err.c_str());
            return 1;
        }
        return 0;
    }

    // A worker thread's write() to a client that has already closed its
    // end of the socket must not kill the process.
    signal(SIGPIPE, SIG_IGN);

    nshgeoip::log_info(std::string("nshgeoip ") + NSHGEOIP_VERSION + " starting");
    nshgeoip::log_info(std::string("libmaxminddb version: ") + MMDB_lib_version());
    nshgeoip::log_info("uses GeoLite2 data created by MaxMind, available from https://www.maxmind.com");

    nshgeoip::Config cfg;
    std::string err;

    // A missing config file is only fatal if the caller named a path
    // explicitly via --config (a typo protection). At the default path,
    // it's a normal way to run nshgeoip purely off environment variables
    // (e.g. in a container with no config file mounted at all) -- a file
    // that exists but fails to load (bad permissions, malformed content)
    // is still always a fatal error either way.
    if (access(config_path.c_str(), F_OK) == 0)
    {
        nshgeoip::log_info("using config file: " + config_path);
        std::vector<std::string> warnings;
        if (!nshgeoip::load_config(config_path, cfg, err, &warnings))
        {
            nshgeoip::log_fatal("failed to load config: " + err);
            return 1;
        }
        for (const auto &w : warnings)
        {
            nshgeoip::log_warn(w);
        }
    }
    else if (config_path_explicit)
    {
        nshgeoip::log_fatal("config file not found: " + config_path);
        return 1;
    }
    else
    {
        nshgeoip::log_info("no config file at " + config_path + ", using defaults and environment variables");
    }

    if (!nshgeoip::apply_env_overrides(cfg, err))
    {
        nshgeoip::log_fatal("failed to apply environment variables: " + err);
        return 1;
    }

    nshgeoip::Logger::instance().set_debug_enabled(cfg.debug_log);

    std::vector<std::string> default_db_notes;
    nshgeoip::apply_default_db_paths(cfg, kDefaultGeoipDirs, &default_db_notes);
    for (const auto &note : default_db_notes)
    {
        nshgeoip::log_info(note);
    }

    nshgeoip::log_info("country_db = " + (cfg.country_db.empty() ? "(disabled)" : cfg.country_db));
    nshgeoip::log_info("asn_db = " + (cfg.asn_db.empty() ? "(disabled)" : cfg.asn_db));
    nshgeoip::log_info("city_db = " + (cfg.city_db.empty() ? "(disabled)" : cfg.city_db));
    nshgeoip::log_info("socket = " + cfg.socket_path);
    nshgeoip::log_info("threads = " + std::to_string(cfg.threads));
    nshgeoip::log_info("tcp_port = " + (cfg.tcp_port != 0 ? std::to_string(cfg.tcp_port) : std::string("(disabled)")));
    if (cfg.tcp_port != 0)
    {
        nshgeoip::log_info("tcp_address = " + (cfg.tcp_address.empty() ? std::string("(loopback)") : cfg.tcp_address));
    }
    nshgeoip::log_info("metrics_file = " + (cfg.metrics_file.empty() ? "(disabled)" : cfg.metrics_file));
    if (!cfg.metrics_file.empty())
    {
        nshgeoip::log_info("metrics_interval_seconds = " + std::to_string(cfg.metrics_interval_seconds));
    }

    nshgeoip::Server server(cfg);
    if (!server.init(err))
    {
        nshgeoip::log_fatal("startup failed: " + err);
        return 1;
    }

    return server.run();
}
