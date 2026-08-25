#pragma once

// Just enough HTTP/1.1 to serve one request per connection: parse the
// request line, query string, and the one request header nshgeoip actually
// looks at (Accept, for content negotiation) -- everything else is
// ignored, as is any body. No keep-alive, no chunked transfer, no
// pipelining -- every response carries "Connection: close" and the socket
// is closed right after. This is deliberately not a general-purpose HTTP
// implementation.

#include <string>
#include <utility>
#include <vector>

namespace nshgeoip
{

enum class ReadResult
{
    Ok,
    TooLarge,
    ConnectionClosed,
    Timeout,
    IoError
};

// Reads from `fd` until an empty line (end of headers) is seen or
// `max_bytes` is exceeded. No request body is ever read (NGINX auth_request
// is configured with proxy_pass_request_body off, and nshgeoip never needs
// one). A receive timeout is applied so a slow/stalled client cannot tie up
// a worker thread indefinitely.
ReadResult read_http_head(int fd, std::size_t max_bytes, int timeout_seconds, std::string &out);

struct HttpRequest
{
    std::string method;
    std::string path; // decoded, no query string
    std::vector<std::pair<std::string, std::string>> query;
    std::string accept; // raw Accept header value, empty if absent
};

// Parses the request line, query string, and Accept header out of the raw
// bytes returned by read_http_head(). Returns false if the request line is
// malformed (wrong number of tokens, missing leading '/', bad
// percent-encoding, not an HTTP/1.x request line). Other header lines are
// scanned over but not kept -- nshgeoip has no use for them.
bool parse_http_request(const std::string &raw, HttpRequest &req);

// Looks up `key` in a parsed request's query parameters; returns false if
// not present.
bool find_query_param(const HttpRequest &req, const std::string &key, std::string &value);

// The response representations nshgeoip can produce. Json/Text are the two
// negotiable representations of the same /lookup result (see
// negotiate_format()) -- JSON is the default for backward compatibility and
// for clients that don't send Accept at all. Prometheus is never negotiated
// (it's not a representation of lookup data at all) -- it's set directly by
// the /metrics handler regardless of Accept, since a Prometheus scraper
// expects exactly that format.
enum class ResponseFormat
{
    Json,
    Text,
    Prometheus
};

// Picks a response representation from a request's raw Accept header
// value. This is deliberately not full RFC 7231 negotiation (no q-value
// weighting) -- nshgeoip only ever has two representations, so the rule is
// simply: explicit "application/json" wins if present (including when
// both are listed, since without q-values neither has stated a
// preference); otherwise "text/plain" selects Text; anything else
// (absent, "*/*", unrecognized) defaults to Json.
ResponseFormat negotiate_format(const std::string &accept_header);

// True if `accept_header` explicitly asks for JSON (case-insensitive
// substring match on "application/json"). Unlike negotiate_format(), there
// is no default-to-Json fallback here -- used by /health, which (unlike
// /lookup) wants the opposite default: a minimal text body unless a client
// explicitly asks for JSON, since most container health checks (Docker
// HEALTHCHECK, Kubernetes liveness/readiness probes) only look at the HTTP
// status code and rarely send an Accept header at all.
bool accept_wants_json(const std::string &accept_header);

struct HttpResponse
{
    int status = 200;
    ResponseFormat format = ResponseFormat::Json;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

// Serializes a response to raw bytes ready to write() to the socket.
// Content-Length and Connection are added automatically; `headers` should
// contain only the extra response headers. When `include_body` is true,
// Content-Type (from resp.format) is added too and the body bytes are
// written; when false (used for HEAD), there is no body to describe, so
// Content-Type is left off, while Content-Length still reflects
// resp.body's real size -- the one part of a GET's headers that remains
// meaningful without the body itself.
std::string build_http_response(const HttpResponse &resp, bool include_body = true);

// Convenience for the small error body used by every non-200 response, in
// whichever format was negotiated: {"error":"..."} for Json, or a single
// "error=..." line for Text.
HttpResponse make_error_response(int status, const std::string &message, ResponseFormat format);

} // namespace nshgeoip
