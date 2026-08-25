#pragma once

// Process-wide request counters and the /health, /metrics response bodies
// built from them plus each database's own metadata (see geoip.h). Counters
// are incremented from worker threads concurrently (one per connection,
// see thread_pool.h) and read from whichever thread handles a /metrics
// request or the periodic metrics_file writer (see server.cpp) -- plain
// std::atomic<uint64_t> with relaxed ordering, since these are independent
// counters with no ordering relationship to any other memory a reader would
// need to observe alongside them.

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "geoip.h"

namespace nshgeoip
{

struct Metrics
{
    // Split by path rather than one flat counter, so self-monitoring
    // traffic (health checks, Prometheus scrapes) never gets conflated
    // with real GeoIP lookup traffic -- a health check or a scrape isn't
    // "a GeoIP request" in any meaningful sense. other_requests_total
    // covers everything else: an unknown path, a malformed request whose
    // path couldn't even be parsed, request-too-large, etc.
    std::atomic_uint64_t lookup_requests_total{0};
    std::atomic_uint64_t health_requests_total{0};
    std::atomic_uint64_t metrics_requests_total{0};
    std::atomic_uint64_t other_requests_total{0};

    std::atomic_uint64_t responses_200{0};
    std::atomic_uint64_t responses_400{0};
    std::atomic_uint64_t responses_404{0};
    std::atomic_uint64_t responses_405{0};
    std::atomic_uint64_t responses_500{0};

    std::atomic_uint64_t lookup_found_total{0};
    std::atomic_uint64_t lookup_not_found_total{0};

    // Increments the request counter matching `path` -- see the per-field
    // comments above for the bucketing rule.
    void record_request(const std::string &path);

    // Increments the responses_<code> counter matching `status`; a status
    // outside the small fixed set above (there isn't one today -- see
    // make_error_response()'s callers -- but this is cheap insurance
    // against a future one silently going uncounted) is dropped.
    void record_response(int status);
};

// One database's status for /health and /metrics -- `metadata` is
// std::nullopt when `open` is false (that database isn't configured).
struct MetricsDbInfo
{
    std::string name; // "country", "asn", or "city"
    bool open = false;
    std::optional<GeoIpDbMetadata> metadata;
};

// Prometheus text exposition format (the same content whether served over
// HTTP from /metrics or written to metrics_file). `uptime_seconds` is the
// caller's own elapsed-since-start measurement -- this function has no
// notion of process start time itself. `mmdb_lib_version` is the linked
// libmaxminddb's own version (MMDB_lib_version()) -- taken as a parameter
// rather than called internally so this stays a pure function of its
// inputs, easy to unit test without depending on whatever happens to be
// linked in.
std::string render_prometheus_metrics(const Metrics &metrics, const std::vector<MetricsDbInfo> &dbs,
                                       const std::string &version, const std::string &mmdb_lib_version,
                                       double uptime_seconds);

// The /health JSON body: overall "ok" status (nshgeoip has no other status
// today -- it wouldn't be running to answer this at all if something were
// actually broken) plus per-database open/build/age info, so a human or a
// dashboard can see database freshness without cross-referencing the
// startup log.
std::string render_health_json(const std::vector<MetricsDbInfo> &dbs, const std::string &version,
                                const std::string &mmdb_lib_version, double uptime_seconds);

} // namespace nshgeoip
