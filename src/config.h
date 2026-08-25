#pragma once

// Parser for the deliberately simple nshgeoip.conf key=value format.

#include <cstdint>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace nshgeoip
{

// Default for Config::threads below when threads= isn't set by the config
// file or NSHGEOIP_THREADS: the host's own CPU core count
// (std::thread::hardware_concurrency()), clamped to [4, 20]. nshgeoip's
// workload is short, synchronous, in-memory lookups -- no I/O wait to hide
// -- so the useful thread count tracks core count rather than some large
// fixed number; the cap exists because going well past core count stopped
// helping (and started showing bimodal tail latency) in load testing on
// this project. Declared here (not just a literal default member
// initializer) so it's callable from config.cpp with external linkage, and
// documented once rather than wherever Config is constructed. Falls back
// to 8 if hardware_concurrency() can't determine a value (it's allowed to
// return 0) -- the old fixed default, kept as a safe universal fallback.
int default_thread_count();

struct Config
{
    std::string country_db; // empty = disabled
    std::string asn_db;     // empty = disabled
    std::string city_db;    // empty = disabled
    std::string socket_path = "/run/nshgeoip/nshgeoip.sock";
    mode_t socket_mode = 0660;
    int threads = default_thread_count();
    std::size_t max_request_bytes = 8192;
    bool debug_log = false;

    // Optional TCP/IP listener, off by default (see README's "TCP listener"
    // section). 0 = disabled. When enabled and tcp_address is empty, binds
    // loopback on both IPv4 (127.0.0.1) and IPv6 (::1); an explicit
    // tcp_address binds that one address only, in place of the loopback
    // default.
    int tcp_port = 0;
    std::string tcp_address;

    // Path to periodically write Prometheus-format metrics to (the same
    // content the /metrics HTTP endpoint serves), for node_exporter's
    // textfile collector or similar. Independent of /metrics itself, which
    // is always available regardless of this -- this is only for a
    // separate scrape-free delivery path. Empty = disabled (default).
    // Written atomically (temp file + rename()) every
    // metrics_interval_seconds.
    std::string metrics_file;
    int metrics_interval_seconds = 60;
};

// Parses `text` as a TCP port number (1-65535). Returns false (leaving
// `out` unchanged) if it isn't a valid integer in range. Shared by config
// file, environment variable, and command-line parsing so all three apply
// the same validation.
bool parse_port(const std::string &text, int &out);

// Loads key=value pairs from `path` into `cfg`. Returns false and fills
// `err` with a human-readable message on failure (missing file, bad
// permissions, or a malformed value for a recognized key). Unknown keys are
// ignored, not an error, so the config format can grow without breaking
// older config files -- but each one is appended to `warnings` (if given)
// so the caller can log it.
bool load_config(const std::string &path, Config &cfg, std::string &err, std::vector<std::string> *warnings = nullptr);

// Overlays NSHGEOIP_* environment variables onto `cfg`, one per config-file
// key (e.g. NSHGEOIP_TCP_PORT for tcp_port=, NSHGEOIP_SOCKET for socket=).
// Only variables that are actually set in the environment override; every
// other field is left as load_config() (or the built-in default) set it.
// Meant to be called after load_config() so environment variables take
// precedence over the config file -- the intended way to configure
// nshgeoip in a container without mounting a config file at all. Returns
// false and fills `err` if a set variable has an invalid value.
bool apply_env_overrides(Config &cfg, std::string &err);

// Fills in country_db/asn_db/city_db with
// "<dir>/GeoLite2-{Country,ASN,City}.mmdb" for any of the three still unset
// (empty) after the config file and environment have been applied -- never
// overrides a field that's already set. `geoip_dirs` is searched in order
// per field, first match wins, so e.g. the standard geoipupdate location
// can be preferred over CrowdSec's own bundled copy. country_db's search is
// skipped entirely once city_db ends up set (by config or by this same
// auto-detection): a real GeoLite2-City.mmdb already carries
// country/continent data, so there's nothing a separate Country database
// would add. Each substitution actually made is appended to `notes` (if
// given) so the caller can log it -- this is a real behavior change, not a
// silent no-op default, and should be visible at startup the same way
// explicit config is.
void apply_default_db_paths(Config &cfg, const std::vector<std::string> &geoip_dirs, std::vector<std::string> *notes = nullptr);

} // namespace nshgeoip
