// Standalone unit-test binary for nshgeoip's pure-function pieces (IP
// parsing, JSON/header sanitization, config parsing). Deliberately not
// assert()-based: a failed check is recorded and printed, and the run
// continues so one broken case doesn't hide the rest. Exit code is 0 only
// if every check passed.
//
// Process/protocol-level behavior (concurrent requests, malformed HTTP on
// the wire, stale socket handling, clean shutdown) is covered separately
// by tests/integration_test.sh against a running daemon.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../src/config.h"
#include "../src/http.h"
#include "../src/ip_addr.h"
#include "../src/metrics.h"
#include "../src/text_util.h"

namespace
{

int g_pass = 0;
int g_fail = 0;

void check(bool condition, const std::string &description)
{
    if (condition)
    {
        ++g_pass;
    }
    else
    {
        ++g_fail;
        std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    }
}

void check_eq(const std::string &actual, const std::string &expected, const std::string &description)
{
    if (actual == expected)
    {
        ++g_pass;
    }
    else
    {
        ++g_fail;
        std::fprintf(stderr, "FAIL: %s\n  expected: %s\n  actual:   %s\n", description.c_str(), expected.c_str(),
                     actual.c_str());
    }
}

// ---------------------------------------------------------------------
// IP address parsing
// ---------------------------------------------------------------------

void test_valid_ipv4()
{
    const char *addrs[] = {"8.8.8.8", "0.0.0.0", "255.255.255.255", "127.0.0.1", "192.168.1.1"};
    for (const char *a : addrs)
    {
        nshgeoip::ParsedAddr out;
        bool ok = nshgeoip::parse_ip_address(a, out);
        check(ok, std::string("valid IPv4 accepted: ") + a);
        if (ok)
        {
            check(out.family == AF_INET, std::string("IPv4 family set correctly: ") + a);
        }
    }
}

void test_valid_ipv6()
{
    const char *addrs[] = {"2001:4860:4860::8888", "::1", "::", "fe80::1", "2001:db8:0:0:0:0:0:1",
                           "::ffff:192.168.1.1"};
    for (const char *a : addrs)
    {
        nshgeoip::ParsedAddr out;
        bool ok = nshgeoip::parse_ip_address(a, out);
        check(ok, std::string("valid IPv6 accepted: ") + a);
        if (ok)
        {
            check(out.family == AF_INET6, std::string("IPv6 family set correctly: ") + a);
        }
    }
}

void test_invalid_ipv4()
{
    const char *addrs[] = {"999.1.1.1", "1.2.3", "1.2.3.4.5", "abc.def.gh.i", "1.2.3.4 ", " 1.2.3.4", "1.2.3.-1"};
    for (const char *a : addrs)
    {
        nshgeoip::ParsedAddr out;
        check(!nshgeoip::parse_ip_address(a, out), std::string("invalid IPv4 rejected: '") + a + "'");
    }
}

void test_invalid_ipv6()
{
    const char *addrs[] = {"gggg::1", "::1::2", "1:2:3:4:5:6:7:8:9", ":::", "2001:db8::g"};
    for (const char *a : addrs)
    {
        nshgeoip::ParsedAddr out;
        check(!nshgeoip::parse_ip_address(a, out), std::string("invalid IPv6 rejected: '") + a + "'");
    }
}

void test_empty_and_oversized()
{
    nshgeoip::ParsedAddr out;
    check(!nshgeoip::parse_ip_address("", out), "empty string rejected");

    std::string huge(200, '1');
    check(!nshgeoip::parse_ip_address(huge, out), "oversized input rejected without touching inet_pton");
}

// ---------------------------------------------------------------------
// JSON escaping
// ---------------------------------------------------------------------

void test_json_escape()
{
    check_eq(nshgeoip::json_escape("plain"), "plain", "json_escape: no-op on plain text");
    check_eq(nshgeoip::json_escape("a\"b"), "a\\\"b", "json_escape: quote");
    check_eq(nshgeoip::json_escape("a\\b"), "a\\\\b", "json_escape: backslash");
    check_eq(nshgeoip::json_escape("a\nb"), "a\\nb", "json_escape: newline");
    check_eq(nshgeoip::json_escape("a\tb"), "a\\tb", "json_escape: tab");
    check_eq(nshgeoip::json_escape(std::string("a\x01"
                                             "b")),
             "a\\u0001b", "json_escape: control character");
    check_eq(nshgeoip::json_escape("GOOGLE"), "GOOGLE", "json_escape: realistic as_org value unchanged");
}

// ---------------------------------------------------------------------
// Header sanitization
// ---------------------------------------------------------------------

void test_sanitize_header_value()
{
    check_eq(nshgeoip::sanitize_header_value("US"), "US", "sanitize_header_value: plain value unchanged");
    check_eq(nshgeoip::sanitize_header_value("US\r\nX-Injected: evil"), "USX-Injected: evil",
             "sanitize_header_value: strips CR/LF, cannot inject a header line");
    check_eq(nshgeoip::sanitize_header_value("A\tB"), "AB", "sanitize_header_value: strips control characters");
    check_eq(nshgeoip::sanitize_header_value("Some Org, Inc."), "Some Org, Inc.",
             "sanitize_header_value: spaces and punctuation preserved");
}

// ---------------------------------------------------------------------
// Config parsing
// ---------------------------------------------------------------------

std::string write_temp_config(const std::string &contents)
{
    char path[] = "/tmp/nshgeoip_test_config_XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp() for test config succeeded");
    if (fd < 0)
    {
        return "";
    }
    ssize_t written = ::write(fd, contents.data(), contents.size());
    check(written == static_cast<ssize_t>(contents.size()), "write() of test config contents succeeded");
    ::close(fd);
    return std::string(path);
}

void test_config_valid()
{
    std::string path = write_temp_config("# comment\n"
                                         "\n"
                                         "country_db=/var/lib/GeoIP/GeoLite2-Country.mmdb\n"
                                         "asn_db=/var/lib/GeoIP/GeoLite2-ASN.mmdb\n"
                                         "socket=/run/nshgeoip/nshgeoip.sock\n"
                                         "socket_mode=0660\n"
                                         "threads=4\n"
                                         "debug_log=true\n");

    nshgeoip::Config cfg;
    std::string err;
    bool ok = nshgeoip::load_config(path, cfg, err);
    check(ok, "valid config file parses: " + err);
    check_eq(cfg.country_db, "/var/lib/GeoIP/GeoLite2-Country.mmdb", "config: country_db parsed");
    check_eq(cfg.asn_db, "/var/lib/GeoIP/GeoLite2-ASN.mmdb", "config: asn_db parsed");
    check_eq(cfg.socket_path, "/run/nshgeoip/nshgeoip.sock", "config: socket parsed");
    check(cfg.socket_mode == 0660, "config: socket_mode parsed as octal");
    check(cfg.threads == 4, "config: threads parsed");
    check(cfg.debug_log == true, "config: debug_log parsed");

    unlink(path.c_str());
}

void test_config_defaults()
{
    std::string path = write_temp_config("country_db=/some/path.mmdb\n");

    nshgeoip::Config cfg;
    std::string err;
    bool ok = nshgeoip::load_config(path, cfg, err);
    check(ok, "minimal config file parses: " + err);
    check_eq(cfg.socket_path, "/run/nshgeoip/nshgeoip.sock", "config: default socket path used when unset");
    check(cfg.socket_mode == 0660, "config: default socket_mode used when unset");
    check(cfg.threads == nshgeoip::default_thread_count(), "config: default threads used when unset");

    unlink(path.c_str());
}

void test_default_thread_count_is_clamped()
{
    int n = nshgeoip::default_thread_count();
    check(n >= 4 && n <= 20, "default_thread_count: result is within the [4, 20] clamp regardless of host core count");

    // threads= explicit config/env is unaffected by this clamp -- it's
    // validated separately (1-256, see test_config_valid /
    // test_config_invalid_socket_mode-style tests) and never goes through
    // default_thread_count() at all once a value is actually given.
    std::string path = write_temp_config("country_db=/some/path.mmdb\nthreads=100\n");
    nshgeoip::Config cfg;
    std::string err;
    check(nshgeoip::load_config(path, cfg, err), "default_thread_count: config with explicit threads= still parses");
    check(cfg.threads == 100, "default_thread_count: explicit threads= is not clamped to [4, 20]");
    unlink(path.c_str());
}

void test_config_unknown_key_is_warning_not_error()
{
    std::string path = write_temp_config("country_db=/some/path.mmdb\n"
                                         "totally_unknown_key=1\n");

    nshgeoip::Config cfg;
    std::string err;
    std::vector<std::string> warnings;
    bool ok = nshgeoip::load_config(path, cfg, err, &warnings);
    check(ok, "config with unknown key still parses");
    check(warnings.size() == 1, "config: unknown key produces exactly one warning");

    unlink(path.c_str());
}

void test_config_invalid_socket_mode()
{
    std::string path = write_temp_config("country_db=/some/path.mmdb\n"
                                         "socket_mode=not-octal\n");

    nshgeoip::Config cfg;
    std::string err;
    bool ok = nshgeoip::load_config(path, cfg, err);
    check(!ok, "config: invalid socket_mode is rejected");

    unlink(path.c_str());
}

void test_config_missing_file()
{
    nshgeoip::Config cfg;
    std::string err;
    bool ok = nshgeoip::load_config("/nonexistent/path/nshgeoip.conf", cfg, err);
    check(!ok, "config: missing file is rejected");
    check(!err.empty(), "config: missing file produces an error message");
}

void test_default_db_paths_fills_when_present()
{
    char dir_template[] = "/tmp/nshgeoip_test_geoipdir_XXXXXX";
    char *dir = mkdtemp(dir_template);
    check(dir != nullptr, "mkdtemp() for default-db-path test succeeded");
    if (dir == nullptr)
    {
        return;
    }

    std::string geoip_dir(dir);
    std::string city_path = geoip_dir + "/GeoLite2-City.mmdb";
    {
        std::ofstream f(city_path);
        f << "dummy";
    }
    // Deliberately do NOT create GeoLite2-ASN.mmdb, to also verify a
    // missing file at the standard location leaves the corresponding
    // field untouched rather than pointing at a nonexistent path.
    // GeoLite2-Country.mmdb is likewise not created -- country_db's
    // search is skipped once city_db is found regardless, so its absence
    // here isn't actually what's being tested (see the dedicated
    // skip-country test for that).

    nshgeoip::Config cfg;
    std::vector<std::string> notes;
    nshgeoip::apply_default_db_paths(cfg, {geoip_dir}, &notes);

    check_eq(cfg.city_db, city_path, "default db path: city_db filled in when the file exists");
    check(cfg.country_db.empty(), "default db path: country_db left empty (city_db covers it)");
    check(cfg.asn_db.empty(), "default db path: asn_db left empty when the file doesn't exist");
    check(notes.size() == 2, "default db path: one note for the city_db substitution, one for skipping country_db");

    unlink(city_path.c_str());
    rmdir(geoip_dir.c_str());
}


void test_default_db_paths_skips_country_when_city_present()
{
    char dir_template[] = "/tmp/nshgeoip_test_geoipdir_XXXXXX";
    char *dir = mkdtemp(dir_template);
    check(dir != nullptr, "mkdtemp() for skip-country test succeeded");
    if (dir == nullptr)
    {
        return;
    }

    std::string geoip_dir(dir);
    std::string city_path = geoip_dir + "/GeoLite2-City.mmdb";
    std::string country_path = geoip_dir + "/GeoLite2-Country.mmdb";
    {
        std::ofstream f(city_path);
        f << "dummy";
    }
    {
        std::ofstream f(country_path);
        f << "dummy";
    }
    // Both files exist here -- proves country_db is skipped because
    // city_db already covers country/continent data, not merely because
    // no Country file happened to be found.

    nshgeoip::Config cfg;
    std::vector<std::string> notes;
    nshgeoip::apply_default_db_paths(cfg, {geoip_dir}, &notes);

    check_eq(cfg.city_db, city_path, "skip-country: city_db still filled in when present");
    check(cfg.country_db.empty(), "skip-country: country_db left unset even though a Country file exists, since city_db covers it");

    unlink(city_path.c_str());
    unlink(country_path.c_str());
    rmdir(geoip_dir.c_str());
}

void test_default_db_paths_does_not_override_explicit_config()
{
    char dir_template[] = "/tmp/nshgeoip_test_geoipdir_XXXXXX";
    char *dir = mkdtemp(dir_template);
    check(dir != nullptr, "mkdtemp() for default-db-path override test succeeded");
    if (dir == nullptr)
    {
        return;
    }

    std::string geoip_dir(dir);
    std::string country_path = geoip_dir + "/GeoLite2-Country.mmdb";
    {
        std::ofstream f(country_path);
        f << "dummy";
    }

    nshgeoip::Config cfg;
    cfg.country_db = "/explicit/path.mmdb";
    std::vector<std::string> notes;
    nshgeoip::apply_default_db_paths(cfg, {geoip_dir}, &notes);

    check_eq(cfg.country_db, "/explicit/path.mmdb", "default db path: explicit country_db is never overridden");
    check(notes.empty(), "default db path: no note recorded when nothing was substituted");

    unlink(country_path.c_str());
    rmdir(geoip_dir.c_str());
}

void test_default_db_paths_searches_dirs_in_order()
{
    char first_template[] = "/tmp/nshgeoip_test_geoipdir_first_XXXXXX";
    char second_template[] = "/tmp/nshgeoip_test_geoipdir_second_XXXXXX";
    char *first_dir = mkdtemp(first_template);
    char *second_dir = mkdtemp(second_template);
    check(first_dir != nullptr && second_dir != nullptr, "mkdtemp() for search-order test succeeded");
    if (first_dir == nullptr || second_dir == nullptr)
    {
        return;
    }

    // Only the second directory has ASN -- verifies the first directory
    // being tried (and missing the file) doesn't stop the search.
    std::string asn_path = std::string(second_dir) + "/GeoLite2-ASN.mmdb";
    {
        std::ofstream f(asn_path);
        f << "dummy";
    }

    // Both directories have City -- verifies the first directory wins when
    // both have the file, matching the documented search order.
    std::string first_city_path = std::string(first_dir) + "/GeoLite2-City.mmdb";
    std::string second_city_path = std::string(second_dir) + "/GeoLite2-City.mmdb";
    {
        std::ofstream f(first_city_path);
        f << "dummy";
    }
    {
        std::ofstream f(second_city_path);
        f << "dummy";
    }

    nshgeoip::Config cfg;
    std::vector<std::string> notes;
    nshgeoip::apply_default_db_paths(cfg, {first_dir, second_dir}, &notes);

    check_eq(cfg.asn_db, asn_path, "default db path: falls through to the second directory when the first lacks the file");
    check_eq(cfg.city_db, first_city_path, "default db path: first directory wins when both have the file");

    unlink(asn_path.c_str());
    unlink(first_city_path.c_str());
    unlink(second_city_path.c_str());
    rmdir(first_dir);
    rmdir(second_dir);
}

// ---------------------------------------------------------------------
// Content negotiation (Accept header -> ResponseFormat)
// ---------------------------------------------------------------------

void test_negotiate_format()
{
    using nshgeoip::negotiate_format;
    using nshgeoip::ResponseFormat;

    check(negotiate_format("") == ResponseFormat::Json, "negotiate_format: absent Accept defaults to Json");
    check(negotiate_format("*/*") == ResponseFormat::Json, "negotiate_format: */* defaults to Json");
    check(negotiate_format("application/json") == ResponseFormat::Json,
          "negotiate_format: application/json selects Json");
    check(negotiate_format("text/plain") == ResponseFormat::Text, "negotiate_format: text/plain selects Text");
    check(negotiate_format("TEXT/PLAIN") == ResponseFormat::Text,
          "negotiate_format: header matching is case-insensitive");
    check(negotiate_format("text/plain;q=0.9") == ResponseFormat::Text,
          "negotiate_format: q-value suffix doesn't break matching");
    check(negotiate_format("application/json, text/plain") == ResponseFormat::Json,
          "negotiate_format: both listed without weights -> Json wins");
    check(negotiate_format("text/html") == ResponseFormat::Json,
          "negotiate_format: unrecognized type defaults to Json");
}

void test_accept_wants_json()
{
    using nshgeoip::accept_wants_json;

    // Opposite default from negotiate_format() by design: /health wants a
    // minimal text body unless a client explicitly asks for JSON, since
    // most container health checks never send an Accept header at all.
    check(!accept_wants_json(""), "accept_wants_json: absent Accept does NOT default to JSON");
    check(!accept_wants_json("*/*"), "accept_wants_json: */* does NOT default to JSON");
    check(!accept_wants_json("text/plain"), "accept_wants_json: text/plain is not JSON");
    check(!accept_wants_json("text/html"), "accept_wants_json: unrecognized type is not JSON");
    check(accept_wants_json("application/json"), "accept_wants_json: explicit application/json is JSON");
    check(accept_wants_json("APPLICATION/JSON"), "accept_wants_json: header matching is case-insensitive");
    check(accept_wants_json("application/json, text/plain"), "accept_wants_json: JSON present among multiple types");
}

// ---------------------------------------------------------------------
// HTTP request line / query string / Accept header parsing
// ---------------------------------------------------------------------

void test_parse_http_request_basic()
{
    nshgeoip::HttpRequest req;
    bool ok = nshgeoip::parse_http_request("GET /lookup?ip=8.8.8.8 HTTP/1.1\r\nHost: localhost\r\n\r\n", req);
    check(ok, "parse_http_request: well-formed GET parses");
    check_eq(req.method, "GET", "parse_http_request: method extracted");
    check_eq(req.path, "/lookup", "parse_http_request: path extracted");
    std::string ip;
    check(nshgeoip::find_query_param(req, "ip", ip), "parse_http_request: ip query param found");
    check_eq(ip, "8.8.8.8", "parse_http_request: ip query param value");
    check(req.accept.empty(), "parse_http_request: Accept stays empty when absent");
}

void test_parse_http_request_accept_header()
{
    nshgeoip::HttpRequest req;
    bool ok = nshgeoip::parse_http_request("GET /lookup?ip=8.8.8.8 HTTP/1.1\r\n"
                                         "Host: localhost\r\n"
                                         "Accept: text/plain\r\n"
                                         "User-Agent: curl/8.0\r\n"
                                         "\r\n",
                                         req);
    check(ok, "parse_http_request: request with multiple headers parses");
    check_eq(req.accept, "text/plain", "parse_http_request: Accept header value extracted");
}

void test_parse_http_request_accept_header_case_and_spacing()
{
    nshgeoip::HttpRequest req;
    bool ok = nshgeoip::parse_http_request("GET /lookup HTTP/1.1\r\naccept:   text/plain   \r\n\r\n", req);
    check(ok, "parse_http_request: lowercase header name parses");
    check_eq(req.accept, "text/plain", "parse_http_request: Accept value trimmed regardless of header name case");
}

void test_parse_http_request_malformed()
{
    nshgeoip::HttpRequest req;
    check(!nshgeoip::parse_http_request("NOT AN HTTP REQUEST\r\n\r\n", req),
          "parse_http_request: garbage request line rejected");
    check(!nshgeoip::parse_http_request("GET /lookup HTTP/9.9\r\n\r\n", req),
          "parse_http_request: unsupported HTTP version rejected");
}

// ---------------------------------------------------------------------
// Metrics rendering (/health, /metrics)
// ---------------------------------------------------------------------

void test_metrics_record_response()
{
    nshgeoip::Metrics m;
    m.record_response(200);
    m.record_response(200);
    m.record_response(400);
    m.record_response(404);
    m.record_response(405);
    m.record_response(500);
    m.record_response(999); // unknown status: dropped, not miscounted elsewhere

    check(m.responses_200.load() == 2, "metrics: responses_200 counted correctly");
    check(m.responses_400.load() == 1, "metrics: responses_400 counted correctly");
    check(m.responses_404.load() == 1, "metrics: responses_404 counted correctly");
    check(m.responses_405.load() == 1, "metrics: responses_405 counted correctly");
    check(m.responses_500.load() == 1, "metrics: responses_500 counted correctly");
}

void test_metrics_record_request()
{
    nshgeoip::Metrics m;
    m.record_request("/lookup");
    m.record_request("/lookup");
    m.record_request("/health");
    m.record_request("/metrics");
    m.record_request("/nonexistent");
    m.record_request(""); // unknown path (malformed/too-large request)

    check(m.lookup_requests_total.load() == 2, "metrics: lookup_requests_total counted correctly");
    check(m.health_requests_total.load() == 1, "metrics: health_requests_total counted correctly");
    check(m.metrics_requests_total.load() == 1, "metrics: metrics_requests_total counted correctly");
    check(m.other_requests_total.load() == 2, "metrics: other_requests_total counted correctly (unknown path + empty)");
}

void test_render_prometheus_metrics_basic()
{
    nshgeoip::Metrics m;
    m.lookup_requests_total.store(42);
    m.health_requests_total.store(7);
    m.metrics_requests_total.store(3);
    m.other_requests_total.store(1);
    m.responses_200.store(30);
    m.lookup_found_total.store(25);
    m.lookup_not_found_total.store(5);

    nshgeoip::GeoIpDbMetadata city_meta;
    city_meta.database_type = "GeoLite2-City";
    city_meta.build_epoch = 1787323152;
    city_meta.build_time = "2026-08-21T14:39:12Z";
    city_meta.age_days = 3.9;

    std::vector<nshgeoip::MetricsDbInfo> dbs;
    dbs.push_back({"country", false, std::nullopt});
    dbs.push_back({"city", true, city_meta});

    std::string text = nshgeoip::render_prometheus_metrics(m, dbs, "0.1.0", "1.7.1", 12345.0);

    check(text.find("nshgeoip_build_info{version=\"0.1.0\",libmaxminddb_version=\"1.7.1\"} 1") != std::string::npos,
          "prometheus: build_info line present with both versions");
    check(text.find("nshgeoip_uptime_seconds 12345") != std::string::npos, "prometheus: uptime_seconds line present");
    check(text.find("nshgeoip_db_open{db=\"country\"} 0") != std::string::npos,
          "prometheus: closed db reported as db_open=0");
    check(text.find("nshgeoip_db_open{db=\"city\"} 1") != std::string::npos,
          "prometheus: open db reported as db_open=1");
    check(text.find("nshgeoip_db_build_epoch_seconds{db=\"city\"} 1787323152") != std::string::npos,
          "prometheus: build_epoch reported for open db");
    check(text.find("nshgeoip_db_build_epoch_seconds{db=\"country\"}") == std::string::npos,
          "prometheus: no build_epoch line for a closed db");
    check(text.find("nshgeoip_requests_total{path=\"lookup\"} 42") != std::string::npos,
          "prometheus: requests_total{path=lookup} value correct");
    check(text.find("nshgeoip_requests_total{path=\"health\"} 7") != std::string::npos,
          "prometheus: requests_total{path=health} value correct");
    check(text.find("nshgeoip_requests_total{path=\"metrics\"} 3") != std::string::npos,
          "prometheus: requests_total{path=metrics} value correct");
    check(text.find("nshgeoip_requests_total{path=\"other\"} 1") != std::string::npos,
          "prometheus: requests_total{path=other} value correct");
    check(text.find("nshgeoip_http_responses_total{code=\"200\"} 30") != std::string::npos,
          "prometheus: http_responses_total{200} value correct");
    check(text.find("nshgeoip_lookup_results_total{result=\"found\"} 25") != std::string::npos,
          "prometheus: lookup_results_total{found} value correct");
    check(text.find("# HELP nshgeoip_build_info") != std::string::npos, "prometheus: HELP line present");
    check(text.find("# TYPE nshgeoip_build_info gauge") != std::string::npos, "prometheus: TYPE line present");
}

void test_render_health_json_basic()
{
    nshgeoip::GeoIpDbMetadata asn_meta;
    asn_meta.database_type = "GeoLite2-ASN";
    asn_meta.build_epoch = 1787559317;
    asn_meta.build_time = "2026-08-24T08:15:17Z";
    asn_meta.age_days = 1.1;
    asn_meta.age_ms = 97314000;
    asn_meta.age_ns = 97314000000000;

    std::vector<nshgeoip::MetricsDbInfo> dbs;
    dbs.push_back({"country", false, std::nullopt});
    dbs.push_back({"asn", true, asn_meta});

    std::string body = nshgeoip::render_health_json(dbs, "0.1.0", "1.7.1", 42.0);

    check(body.find("\"status\":\"ok\"") != std::string::npos, "health: status ok present");
    check(body.find("\"version\":\"0.1.0\"") != std::string::npos, "health: version present");
    check(body.find("\"libmaxminddb_version\":\"1.7.1\"") != std::string::npos, "health: libmaxminddb_version present");
    check(body.find("\"uptime_seconds\":42") != std::string::npos, "health: uptime_seconds present");
    check(body.find("\"country\":{\"open\":false}") != std::string::npos,
          "health: closed db reported as open:false with no extra fields");
    check(body.find("\"database_type\":\"GeoLite2-ASN\"") != std::string::npos, "health: open db reports database_type");
    check(body.find("\"build_epoch\":1787559317") != std::string::npos, "health: open db reports build_epoch");
    check(body.find("\"age_days\":1.1") != std::string::npos, "health: open db reports age_days");
    check(body.find("\"age_ms\":97314000") != std::string::npos, "health: open db reports age_ms");
    check(body.find("\"age_ns\":97314000000000") != std::string::npos, "health: open db reports age_ns");
}

} // namespace

int main()
{
    test_valid_ipv4();
    test_valid_ipv6();
    test_invalid_ipv4();
    test_invalid_ipv6();
    test_empty_and_oversized();

    test_json_escape();
    test_sanitize_header_value();

    test_config_valid();
    test_config_defaults();
    test_default_thread_count_is_clamped();
    test_config_unknown_key_is_warning_not_error();
    test_config_invalid_socket_mode();
    test_config_missing_file();
    test_default_db_paths_fills_when_present();
    test_default_db_paths_skips_country_when_city_present();
    test_default_db_paths_does_not_override_explicit_config();
    test_default_db_paths_searches_dirs_in_order();

    test_negotiate_format();
    test_accept_wants_json();
    test_parse_http_request_basic();
    test_parse_http_request_accept_header();
    test_parse_http_request_accept_header_case_and_spacing();
    test_parse_http_request_malformed();

    test_metrics_record_response();
    test_metrics_record_request();
    test_render_prometheus_metrics_basic();
    test_render_health_json_basic();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
