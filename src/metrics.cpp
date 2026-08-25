// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "metrics.h"

#include <iomanip>
#include <sstream>

#include "text_util.h"

namespace nshgeoip
{

void Metrics::record_request(const std::string &path)
{
    if (path == "/lookup")
    {
        lookup_requests_total.fetch_add(1, std::memory_order_relaxed);
    }
    else if (path == "/health")
    {
        health_requests_total.fetch_add(1, std::memory_order_relaxed);
    }
    else if (path == "/metrics")
    {
        metrics_requests_total.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        other_requests_total.fetch_add(1, std::memory_order_relaxed);
    }
}

void Metrics::record_response(int status)
{
    switch (status)
    {
    case 200:
        responses_200.fetch_add(1, std::memory_order_relaxed);
        break;
    case 400:
        responses_400.fetch_add(1, std::memory_order_relaxed);
        break;
    case 404:
        responses_404.fetch_add(1, std::memory_order_relaxed);
        break;
    case 405:
        responses_405.fetch_add(1, std::memory_order_relaxed);
        break;
    case 500:
        responses_500.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        break;
    }
}

namespace
{

// Prometheus text format's own escaping for a label value: backslash,
// double-quote, and newline. Every label value here is an internally
// controlled string (a database name, the build version, a status code),
// not untrusted input -- this is correctness/spec-compliance, not a
// security boundary.
std::string escape_label_value(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in)
    {
        switch (c)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::uint64_t load(const std::atomic<std::uint64_t> &counter)
{
    return counter.load(std::memory_order_relaxed);
}

} // namespace

std::string render_prometheus_metrics(const Metrics &metrics, const std::vector<MetricsDbInfo> &dbs,
                                       const std::string &version, const std::string &mmdb_lib_version,
                                       double uptime_seconds)
{
    std::ostringstream out;

    out << "# HELP nshgeoip_build_info nshgeoip build information.\n";
    out << "# TYPE nshgeoip_build_info gauge\n";
    out << "nshgeoip_build_info{version=\"" << escape_label_value(version) << "\",libmaxminddb_version=\""
        << escape_label_value(mmdb_lib_version) << "\"} 1\n";

    out << "# HELP nshgeoip_uptime_seconds Seconds since the daemon started.\n";
    out << "# TYPE nshgeoip_uptime_seconds gauge\n";
    out << "nshgeoip_uptime_seconds " << std::fixed << std::setprecision(0) << uptime_seconds << "\n";

    out << "# HELP nshgeoip_db_open Whether a database is currently open (1) or not (0).\n";
    out << "# TYPE nshgeoip_db_open gauge\n";
    for (const auto &db : dbs)
    {
        out << "nshgeoip_db_open{db=\"" << escape_label_value(db.name) << "\"} " << (db.open ? 1 : 0) << "\n";
    }

    out << "# HELP nshgeoip_db_build_epoch_seconds Database build time as a Unix timestamp.\n";
    out << "# TYPE nshgeoip_db_build_epoch_seconds gauge\n";
    for (const auto &db : dbs)
    {
        if (!db.metadata)
        {
            continue;
        }
        out << "nshgeoip_db_build_epoch_seconds{db=\"" << escape_label_value(db.name) << "\"} "
            << db.metadata->build_epoch << "\n";
    }

    out << "# HELP nshgeoip_db_age_seconds Seconds since the database was built.\n";
    out << "# TYPE nshgeoip_db_age_seconds gauge\n";
    for (const auto &db : dbs)
    {
        if (!db.metadata)
        {
            continue;
        }
        out << "nshgeoip_db_age_seconds{db=\"" << escape_label_value(db.name) << "\"} " << std::fixed
            << std::setprecision(0) << (db.metadata->age_days * 86400.0) << "\n";
    }

    out << "# HELP nshgeoip_requests_total Total HTTP requests received, by path.\n";
    out << "# TYPE nshgeoip_requests_total counter\n";
    out << "nshgeoip_requests_total{path=\"lookup\"} " << load(metrics.lookup_requests_total) << "\n";
    out << "nshgeoip_requests_total{path=\"health\"} " << load(metrics.health_requests_total) << "\n";
    out << "nshgeoip_requests_total{path=\"metrics\"} " << load(metrics.metrics_requests_total) << "\n";
    out << "nshgeoip_requests_total{path=\"other\"} " << load(metrics.other_requests_total) << "\n";

    out << "# HELP nshgeoip_http_responses_total HTTP responses sent, by status code.\n";
    out << "# TYPE nshgeoip_http_responses_total counter\n";
    out << "nshgeoip_http_responses_total{code=\"200\"} " << load(metrics.responses_200) << "\n";
    out << "nshgeoip_http_responses_total{code=\"400\"} " << load(metrics.responses_400) << "\n";
    out << "nshgeoip_http_responses_total{code=\"404\"} " << load(metrics.responses_404) << "\n";
    out << "nshgeoip_http_responses_total{code=\"405\"} " << load(metrics.responses_405) << "\n";
    out << "nshgeoip_http_responses_total{code=\"500\"} " << load(metrics.responses_500) << "\n";

    out << "# HELP nshgeoip_lookup_results_total /lookup outcomes, by result.\n";
    out << "# TYPE nshgeoip_lookup_results_total counter\n";
    out << "nshgeoip_lookup_results_total{result=\"found\"} " << load(metrics.lookup_found_total) << "\n";
    out << "nshgeoip_lookup_results_total{result=\"not_found\"} " << load(metrics.lookup_not_found_total) << "\n";

    return out.str();
}

std::string render_health_json(const std::vector<MetricsDbInfo> &dbs, const std::string &version,
                                const std::string &mmdb_lib_version, double uptime_seconds)
{
    std::ostringstream out;
    out << "{";
    out << "\"status\":\"ok\"";
    out << ",\"version\":\"" << json_escape(version) << "\"";
    out << ",\"libmaxminddb_version\":\"" << json_escape(mmdb_lib_version) << "\"";
    out << ",\"uptime_seconds\":" << std::fixed << std::setprecision(0) << uptime_seconds;

    out << ",\"databases\":{";
    for (std::size_t i = 0; i < dbs.size(); ++i)
    {
        const MetricsDbInfo &db = dbs[i];
        if (i > 0)
        {
            out << ",";
        }
        out << "\"" << json_escape(db.name) << "\":{\"open\":" << (db.open ? "true" : "false");
        if (db.metadata)
        {
            out << ",\"database_type\":\"" << json_escape(db.metadata->database_type) << "\"";
            out << ",\"build_epoch\":" << db.metadata->build_epoch;
            out << ",\"build_date\":\"" << json_escape(db.metadata->build_time) << "\"";
            out << ",\"age_days\":" << std::fixed << std::setprecision(1) << db.metadata->age_days;
            out << ",\"age_ms\":" << db.metadata->age_ms;
            out << ",\"age_ns\":" << db.metadata->age_ns;
        }
        out << "}";
    }
    out << "}";

    out << "}";
    return out.str();
}

} // namespace nshgeoip
