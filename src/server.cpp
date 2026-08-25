// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include "http.h"
#include "ip_addr.h"
#include "log.h"
#include "text_util.h"
#include "version.h"

namespace nshgeoip
{

namespace
{

std::string errno_str()
{
    return std::strerror(errno);
}

std::string mode_octal(mode_t mode)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04o", mode & 07777);
    return std::string(buf);
}

// "<path> (<database_type>, built <UTC timestamp>, N.N days old)" if
// metadata is available, or just "<path>" otherwise (metadata is always
// available in practice -- MMDB_open() would have already failed on a
// file without a readable metadata section -- but a database's own
// opened-ness is what callers actually check, not this).
std::string describe_db(const std::string &path, const std::optional<GeoIpDbMetadata> &meta)
{
    if (!meta)
    {
        return path;
    }
    std::ostringstream oss;
    oss << path << " (" << meta->database_type << ", built " << meta->build_time << ", " << std::fixed
        << std::setprecision(1) << meta->age_days << " days old)";
    return oss.str();
}

bool write_all(int fd, const std::string &data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// std::to_string(double) always pads to 6 decimal places (e.g.
// "35.685360"); this keeps the natural precision MaxMind's data has
// instead (e.g. "35.68536") without the noise of trailing zeros. Shared by
// the JSON body, the text body, and the X-GeoIP-Latitude/-Longitude
// headers below.
std::string format_double(double v)
{
    std::ostringstream oss;
    oss << std::setprecision(9) << v;
    return oss.str();
}

std::string build_json_body(const std::string &ip_text, const GeoIpResult &result)
{
    std::ostringstream body;
    body << "{";
    body << "\"ip\":\"" << json_escape(ip_text) << "\"";
    body << ",\"country_code\":" << (result.country_code ? ("\"" + json_escape(*result.country_code) + "\"") : "null");
    body << ",\"country_name\":" << (result.country_name ? ("\"" + json_escape(*result.country_name) + "\"") : "null");
    body << ",\"continent_code\":"
         << (result.continent_code ? ("\"" + json_escape(*result.continent_code) + "\"") : "null");
    body << ",\"asn\":" << (result.asn ? std::to_string(*result.asn) : "null");
    body << ",\"as_org\":" << (result.as_org ? ("\"" + json_escape(*result.as_org) + "\"") : "null");
    body << ",\"city_name\":" << (result.city_name ? ("\"" + json_escape(*result.city_name) + "\"") : "null");
    body << ",\"postal_code\":" << (result.postal_code ? ("\"" + json_escape(*result.postal_code) + "\"") : "null");
    body << ",\"latitude\":" << (result.latitude ? format_double(*result.latitude) : "null");
    body << ",\"longitude\":" << (result.longitude ? format_double(*result.longitude) : "null");
    body << ",\"accuracy_radius\":" << (result.accuracy_radius ? std::to_string(*result.accuracy_radius) : "null");
    body << "}";
    return body.str();
}

// INI-style text/plain representation: one "key=value" line per field,
// values sanitized the same way header values are (CR/LF and other
// control characters stripped) since a naive line-based parser reading
// this body would otherwise be exposed to the same injection risk that
// motivated sanitize_header_value() in the first place -- an embedded
// newline in an MMDB string could otherwise forge an extra "key=value"
// line. Unlike the JSON body, a missing field is simply an omitted line
// rather than an explicit null -- INI has no native "null" to write.
std::string build_ini_body(const std::string &ip_text, const GeoIpResult &result)
{
    std::ostringstream body;
    body << "ip=" << sanitize_header_value(ip_text) << "\n";
    if (result.country_code)
    {
        body << "country_code=" << sanitize_header_value(*result.country_code) << "\n";
    }
    if (result.country_name)
    {
        body << "country_name=" << sanitize_header_value(*result.country_name) << "\n";
    }
    if (result.continent_code)
    {
        body << "continent_code=" << sanitize_header_value(*result.continent_code) << "\n";
    }
    if (result.asn)
    {
        body << "asn=" << std::to_string(*result.asn) << "\n";
    }
    if (result.as_org)
    {
        body << "as_org=" << sanitize_header_value(*result.as_org) << "\n";
    }
    if (result.city_name)
    {
        body << "city_name=" << sanitize_header_value(*result.city_name) << "\n";
    }
    if (result.postal_code)
    {
        body << "postal_code=" << sanitize_header_value(*result.postal_code) << "\n";
    }
    if (result.latitude)
    {
        body << "latitude=" << format_double(*result.latitude) << "\n";
    }
    if (result.longitude)
    {
        body << "longitude=" << format_double(*result.longitude) << "\n";
    }
    if (result.accuracy_radius)
    {
        body << "accuracy_radius=" << std::to_string(*result.accuracy_radius) << "\n";
    }
    return body.str();
}

HttpResponse build_success_response(const std::string &ip_text, const GeoIpResult &result, ResponseFormat format)
{
    HttpResponse resp;
    resp.status = 200;
    resp.format = format;
    resp.body = (format == ResponseFormat::Json) ? build_json_body(ip_text, result) : build_ini_body(ip_text, result);

    // Headers are the same regardless of body format, and are omitted
    // (not null/empty) when a value is unavailable, per spec.
    if (result.country_code)
    {
        resp.headers.push_back({"X-GeoIP-Country", sanitize_header_value(*result.country_code)});
    }
    if (result.continent_code)
    {
        resp.headers.push_back({"X-GeoIP-Continent", sanitize_header_value(*result.continent_code)});
    }
    if (result.asn)
    {
        resp.headers.push_back({"X-GeoIP-ASN", std::to_string(*result.asn)});
    }
    if (result.as_org)
    {
        resp.headers.push_back({"X-GeoIP-AS-Org", sanitize_header_value(*result.as_org)});
    }
    if (result.city_name)
    {
        resp.headers.push_back({"X-GeoIP-City", sanitize_header_value(*result.city_name)});
    }
    if (result.postal_code)
    {
        resp.headers.push_back({"X-GeoIP-Postal-Code", sanitize_header_value(*result.postal_code)});
    }
    if (result.latitude)
    {
        resp.headers.push_back({"X-GeoIP-Latitude", format_double(*result.latitude)});
    }
    if (result.longitude)
    {
        resp.headers.push_back({"X-GeoIP-Longitude", format_double(*result.longitude)});
    }
    if (result.accuracy_radius)
    {
        resp.headers.push_back({"X-GeoIP-Accuracy-Radius", std::to_string(*result.accuracy_radius)});
    }

    return resp;
}

} // namespace

Server::Server(Config cfg) : cfg_(std::move(cfg)), start_time_(std::chrono::steady_clock::now()) {}

Server::~Server()
{
    if (pool_)
    {
        pool_->shutdown();
        pool_.reset();
    }
    if (listen_fd_ >= 0)
    {
        close(listen_fd_);
    }
    for (int fd : tcp_listen_fds_)
    {
        close(fd);
    }
    if (signal_fd_ >= 0)
    {
        close(signal_fd_);
    }
    // socket_created_/unlink is handled by cleanup() on the normal shutdown
    // path; if init() failed partway through, unlink here too so a failed
    // startup does not leave a stale socket behind.
    if (socket_created_)
    {
        unlink(cfg_.socket_path.c_str());
    }
    // GeoIpDatabases (db_) closes its MMDB handles in its own destructor.
}

bool Server::init(std::string &err)
{
    if (!setup_signals(err))
    {
        return false;
    }

    if (!cfg_.country_db.empty())
    {
        if (!db_.open_country(cfg_.country_db, err))
        {
            return false;
        }
        log_info("opened country database: " + describe_db(cfg_.country_db, db_.country_metadata()));
    }
    if (!cfg_.asn_db.empty())
    {
        if (!db_.open_asn(cfg_.asn_db, err))
        {
            return false;
        }
        log_info("opened ASN database: " + describe_db(cfg_.asn_db, db_.asn_metadata()));
    }
    if (!cfg_.city_db.empty())
    {
        if (!db_.open_city(cfg_.city_db, err))
        {
            return false;
        }
        log_info("opened city database: " + describe_db(cfg_.city_db, db_.city_metadata()));
    }
    if (!db_.has_country() && !db_.has_asn() && !db_.has_city())
    {
        err = "no databases configured (set country_db, asn_db, and/or city_db)";
        return false;
    }

    if (!setup_socket(err))
    {
        return false;
    }

    if (!setup_tcp_listeners(err))
    {
        return false;
    }

    pool_ =
        std::make_unique<ThreadPool>(static_cast<std::size_t>(cfg_.threads), [this](int fd) { handle_connection(fd); });
    log_info("worker pool started with " + std::to_string(cfg_.threads) + " threads");

    return true;
}

bool Server::setup_signals(std::string &err)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);

    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0)
    {
        err = "sigprocmask() failed: " + errno_str();
        return false;
    }

    signal_fd_ = signalfd(-1, &mask, SFD_CLOEXEC);
    if (signal_fd_ < 0)
    {
        err = "signalfd() failed: " + errno_str();
        return false;
    }

    return true;
}

bool Server::path_has_live_listener(const std::string &path) const
{
    // A UNIX domain socket connect() never blocks on a handshake the way a
    // TCP one can: it either succeeds immediately (something is listening
    // and has backlog room) or fails immediately, typically ECONNREFUSED
    // when the path exists but nothing is bound/listening there anymore.
    // This is just a liveness probe -- the connection is torn down right
    // away either way.
    int probe_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe_fd < 0)
    {
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    int rc = connect(probe_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    close(probe_fd);
    return rc == 0;
}

bool Server::ensure_socket_directory(const std::string &socket_path, std::string &err)
{
    size_t slash = socket_path.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
    {
        return true; // socket path has no separate parent directory to create
    }
    std::string dir = socket_path.substr(0, slash);

    // Single mkdir() level, not a recursive "mkdir -p": the realistic cases
    // (/run/nshgeoip, /tmp/nshgeoip) are always exactly one level below a
    // directory that already exists on every Linux system. Under systemd,
    // RuntimeDirectory=nshgeoip will usually have already created this
    // directory (pre-chowned to the service user) before nshgeoip even
    // starts, in which case this is a harmless EEXIST; this exists so
    // nshgeoip also works the same way run directly, without systemd, for
    // any parent directory the process already has write access to (e.g.
    // /tmp, or /run itself if running as root).
    if (mkdir(dir.c_str(), 0755) == 0)
    {
        log_info("created socket directory: " + dir);
        return true;
    }
    if (errno != EEXIST)
    {
        err = "failed to create socket directory '" + dir + "': " + errno_str() +
              " (if running under systemd, set RuntimeDirectory=nshgeoip; "
              "otherwise create it manually, e.g. 'mkdir -p " +
              dir + "')";
        return false;
    }

    // Something is already there. Don't just trust it's a directory nshgeoip
    // can actually use -- confirm both. Checking write access rather than
    // exact ownership (st_uid == getuid()) is deliberately more permissive:
    // it accepts a directory nshgeoip doesn't own but can write into (e.g. a
    // shared tmpfs mode=1777 mount, common in containers), not just one it
    // created itself.
    struct stat st
    {};
    if (lstat(dir.c_str(), &st) != 0)
    {
        err = "failed to stat socket directory '" + dir + "': " + errno_str();
        return false;
    }
    if (!S_ISDIR(st.st_mode))
    {
        err = "socket directory path '" + dir + "' exists but is not a directory";
        return false;
    }
    if (access(dir.c_str(), W_OK) != 0)
    {
        err = "socket directory '" + dir + "' exists but is not writable: " + errno_str();
        return false;
    }
    return true;
}

bool Server::setup_socket(std::string &err)
{
    const std::string &path = cfg_.socket_path;

    if (!ensure_socket_directory(path, err))
    {
        return false;
    }

    struct stat st
    {};
    if (lstat(path.c_str(), &st) == 0)
    {
        if (!S_ISSOCK(st.st_mode))
        {
            err = "refusing to remove non-socket file at " + path;
            return false;
        }
        // A socket file on disk does not by itself mean it's stale --
        // another nshgeoip instance (or an accidental double-start) could
        // still be live and listening on it. Deleting it out from under a
        // running process would silently orphan that process's socket
        // (it keeps running, but nothing can connect() to it anymore).
        if (path_has_live_listener(path))
        {
            err = "another process is already listening on " + path +
                  " -- refusing to remove its socket. Stop that instance "
                  "first, or configure a different socket= path.";
            return false;
        }
        if (unlink(path.c_str()) != 0)
        {
            err = "failed to remove stale socket " + path + ": " + errno_str();
            return false;
        }
        log_warn("removed stale socket: " + path);
    }
    else if (errno != ENOENT)
    {
        err = "failed to stat socket path " + path + ": " + errno_str();
        return false;
    }

    // SOCK_NONBLOCK: the accept loop in run() drains every pending
    // connection in a tight loop until accept4() returns EAGAIN, which
    // only happens if the listening socket is actually non-blocking --
    // without this, the final accept4() call once the backlog is empty
    // would block waiting for the next connection, stalling the main
    // thread inside accept4() instead of returning to poll() where
    // SIGTERM/SIGHUP get noticed.
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0)
    {
        err = "socket() failed: " + errno_str();
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
    {
        err = "socket path too long: " + path;
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        err = "bind(" + path + ") failed: " + errno_str();
        return false;
    }
    socket_created_ = true;

    if (chmod(path.c_str(), cfg_.socket_mode) != 0)
    {
        err = "chmod(" + path + ") failed: " + errno_str();
        return false;
    }

    // SOMAXCONN, not a fixed number: the kernel silently clamps whatever we
    // pass to the current net.core.somaxconn anyway, so asking for the
    // system's actual maximum costs nothing and avoids quietly capping the
    // accept backlog at a small hardcoded value on systems where the
    // configured maximum is much larger (e.g. 4096 on current kernels,
    // vs. the historical 128).
    if (listen(listen_fd_, SOMAXCONN) != 0)
    {
        err = "listen() failed: " + errno_str();
        return false;
    }

    log_info("listening on unix socket " + path + " (mode " + mode_octal(cfg_.socket_mode) + ")");
    return true;
}

bool Server::bind_one_tcp(int family, const sockaddr *addr, socklen_t addrlen, std::string &err)
{
    // SOCK_NONBLOCK for the same reason as the unix listener above: the
    // accept loop in run() drains each listener until EAGAIN.
    int fd = socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        err = "socket() failed for TCP listener: " + errno_str();
        return false;
    }

    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0)
    {
        err = "setsockopt(SO_REUSEADDR) failed: " + errno_str();
        close(fd);
        return false;
    }

    if (family == AF_INET6)
    {
        // Without this, some systems bind an IPv6 socket as dual-stack by
        // default, which would then collide with the separate IPv4 loopback
        // listener bound alongside it.
        int v6only = 1;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) != 0)
        {
            err = "setsockopt(IPV6_V6ONLY) failed: " + errno_str();
            close(fd);
            return false;
        }
    }

    if (bind(fd, addr, addrlen) != 0)
    {
        err = "bind() failed for TCP listener: " + errno_str();
        close(fd);
        return false;
    }

    if (listen(fd, SOMAXCONN) != 0)
    {
        err = "listen() failed for TCP listener: " + errno_str();
        close(fd);
        return false;
    }

    char host[INET6_ADDRSTRLEN] = {};
    uint16_t port = 0;
    if (family == AF_INET)
    {
        const auto *a4 = reinterpret_cast<const sockaddr_in *>(addr);
        inet_ntop(AF_INET, &a4->sin_addr, host, sizeof(host));
        port = ntohs(a4->sin_port);
    }
    else
    {
        const auto *a6 = reinterpret_cast<const sockaddr_in6 *>(addr);
        inet_ntop(AF_INET6, &a6->sin6_addr, host, sizeof(host));
        port = ntohs(a6->sin6_port);
    }

    tcp_listen_fds_.push_back(fd);
    log_info("listening on tcp " + std::string(host) + ":" + std::to_string(port));
    return true;
}

bool Server::setup_tcp_listeners(std::string &err)
{
    if (cfg_.tcp_port == 0)
    {
        return true; // disabled by default
    }

    if (cfg_.tcp_address.empty())
    {
        sockaddr_in addr4{};
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(static_cast<uint16_t>(cfg_.tcp_port));
        addr4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (!bind_one_tcp(AF_INET, reinterpret_cast<sockaddr *>(&addr4), sizeof(addr4), err))
        {
            return false;
        }

        sockaddr_in6 addr6{};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(static_cast<uint16_t>(cfg_.tcp_port));
        addr6.sin6_addr = in6addr_loopback;
        if (!bind_one_tcp(AF_INET6, reinterpret_cast<sockaddr *>(&addr6), sizeof(addr6), err))
        {
            return false;
        }

        return true;
    }

    ParsedAddr addr;
    if (!parse_ip_address(cfg_.tcp_address, addr))
    {
        err = "invalid tcp_address: " + cfg_.tcp_address;
        return false;
    }

    if (addr.family == AF_INET)
    {
        reinterpret_cast<sockaddr_in *>(&addr.storage)->sin_port = htons(static_cast<uint16_t>(cfg_.tcp_port));
    }
    else if (addr.family == AF_INET6)
    {
        reinterpret_cast<sockaddr_in6 *>(&addr.storage)->sin6_port = htons(static_cast<uint16_t>(cfg_.tcp_port));
    }
    else
    {
        err = "unsupported address family for tcp_address: " + cfg_.tcp_address;
        return false;
    }

    return bind_one_tcp(addr.family, addr.sockaddr_ptr(), addr.len, err);
}

void Server::handle_connection(int fd)
{
    std::string raw;
    ReadResult rr = read_http_head(fd, cfg_.max_request_bytes, /*timeout_seconds=*/5, raw);

    HttpResponse resp;

    switch (rr)
    {
    case ReadResult::Ok:
        break;
    case ReadResult::TooLarge:
        // Nothing was successfully parsed (the buffer was cut off before
        // we even tried), so there's no path to bucket by -- counts as
        // "other" -- and no Accept header to negotiate on, so this one
        // error always comes back as JSON.
        metrics_.record_request("");
        resp = make_error_response(400, "request too large", ResponseFormat::Json);
        metrics_.record_response(resp.status);
        write_all(fd, build_http_response(resp));
        close(fd);
        return;
    case ReadResult::ConnectionClosed:
    case ReadResult::Timeout:
    case ReadResult::IoError:
        close(fd);
        return;
    }

    HttpRequest req;
    bool is_head = false;
    if (!parse_http_request(raw, req))
    {
        log_warn("rejected malformed HTTP request");
        metrics_.record_request(""); // path unknown -- "other"
        resp = make_error_response(400, "malformed HTTP request", ResponseFormat::Json);
    }
    else
    {
        metrics_.record_request(req.path);
        ResponseFormat fmt = negotiate_format(req.accept);

        if (req.method != "GET" && req.method != "HEAD")
        {
            resp = make_error_response(405, "method not allowed", fmt);
            resp.headers.push_back({"Allow", "GET, HEAD"});
        }
        else
        {
            // Known-good method past this point: HEAD gets the same
            // status and X-GeoIP-*/Allow headers GET would, minus the
            // body -- build_http_response(..., include_body=false) below
            // also drops Content-Type (nothing to describe) but keeps
            // Content-Length.
            is_head = (req.method == "HEAD");

            if (req.path == "/lookup")
            {
                std::string ip_text;
                if (!find_query_param(req, "ip", ip_text) || ip_text.empty())
                {
                    resp = make_error_response(400, "missing ip parameter", fmt);
                }
                else
                {
                    ParsedAddr addr;
                    if (!parse_ip_address(ip_text, addr))
                    {
                        resp = make_error_response(400, "invalid ip address", fmt);
                    }
                    else
                    {
                        bool db_error = false;
                        GeoIpResult result = db_.lookup(addr, db_error);
                        if (db_error)
                        {
                            log_error("database error while looking up an address");
                            resp = make_error_response(500, "internal database error", fmt);
                        }
                        else if (!result.found)
                        {
                            metrics_.lookup_not_found_total.fetch_add(1, std::memory_order_relaxed);
                            resp = make_error_response(404, "no geoip information found", fmt);
                        }
                        else
                        {
                            metrics_.lookup_found_total.fetch_add(1, std::memory_order_relaxed);
                            resp = build_success_response(ip_text, result, fmt);
                        }
                    }
                }
            }
            else if (req.path == "/health")
            {
                // Deliberately NOT negotiate_format() (fmt above), whose
                // default is Json -- that's the right default for /lookup's
                // backward compatibility, but the wrong one here: most
                // container health checks (Docker HEALTHCHECK, Kubernetes
                // liveness/readiness probes) only look at the HTTP status
                // code and rarely send an Accept header at all. So this
                // defaults to a minimal "status=ok" line unless a client
                // explicitly asks for JSON -- accept_wants_json() has no
                // default-to-Json fallback, unlike negotiate_format(). The
                // full per-database JSON detail is opt-in, for a human or a
                // dashboard that explicitly wants it.
                bool want_json = accept_wants_json(req.accept);
                resp.status = 200;
                resp.format = want_json ? ResponseFormat::Json : ResponseFormat::Text;
                resp.body = want_json ? render_health_json(collect_db_infos(), NSHGEOIP_VERSION, MMDB_lib_version(),
                                                             uptime_seconds())
                                       : "status=ok\n";
            }
            else if (req.path == "/metrics")
            {
                // Always Prometheus text format, ignoring Accept -- a
                // scraper expects exactly that, not JSON/text negotiation.
                resp.status = 200;
                resp.format = ResponseFormat::Prometheus;
                resp.body = render_prometheus_metrics(metrics_, collect_db_infos(), NSHGEOIP_VERSION, MMDB_lib_version(),
                                                        uptime_seconds());
            }
            else
            {
                resp = make_error_response(404, "not found", fmt);
            }
        }
    }

    metrics_.record_response(resp.status);
    write_all(fd, build_http_response(resp, /*include_body=*/!is_head));
    close(fd);
}

void Server::reload_databases()
{
    bool ok = true;

    if (!cfg_.country_db.empty())
    {
        std::string err;
        if (db_.reload_country(cfg_.country_db, err))
        {
            log_info("reloaded country database: " + describe_db(cfg_.country_db, db_.country_metadata()));
        }
        else
        {
            log_error("failed to reload country database, keeping previous "
                      "handle open: " +
                      err);
            ok = false;
        }
    }
    if (!cfg_.asn_db.empty())
    {
        std::string err;
        if (db_.reload_asn(cfg_.asn_db, err))
        {
            log_info("reloaded ASN database: " + describe_db(cfg_.asn_db, db_.asn_metadata()));
        }
        else
        {
            log_error("failed to reload ASN database, keeping previous "
                      "handle open: " +
                      err);
            ok = false;
        }
    }
    if (!cfg_.city_db.empty())
    {
        std::string err;
        if (db_.reload_city(cfg_.city_db, err))
        {
            log_info("reloaded city database: " + describe_db(cfg_.city_db, db_.city_metadata()));
        }
        else
        {
            log_error("failed to reload city database, keeping previous "
                      "handle open: " +
                      err);
            ok = false;
        }
    }

    log_info(ok ? "database reload complete" : "database reload finished with errors");
}

std::vector<MetricsDbInfo> Server::collect_db_infos() const
{
    std::vector<MetricsDbInfo> infos;

    MetricsDbInfo country;
    country.name = "country";
    country.open = db_.has_country();
    country.metadata = db_.country_metadata();
    infos.push_back(std::move(country));

    MetricsDbInfo asn;
    asn.name = "asn";
    asn.open = db_.has_asn();
    asn.metadata = db_.asn_metadata();
    infos.push_back(std::move(asn));

    MetricsDbInfo city;
    city.name = "city";
    city.open = db_.has_city();
    city.metadata = db_.city_metadata();
    infos.push_back(std::move(city));

    return infos;
}

double Server::uptime_seconds() const
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now() - start_time_).count();
}

void Server::write_metrics_file()
{
    std::string text =
        render_prometheus_metrics(metrics_, collect_db_infos(), NSHGEOIP_VERSION, MMDB_lib_version(), uptime_seconds());

    // Atomic replacement (temp file in the same directory + rename(), which
    // is atomic on POSIX within one filesystem) so a reader -- e.g.
    // node_exporter's textfile collector -- never sees a partially-written
    // file mid-update.
    std::string tmp_path = cfg_.metrics_file + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out)
        {
            log_warn("failed to open metrics file for writing: " + tmp_path);
            return;
        }
        out << text;
        if (!out)
        {
            log_warn("failed to write metrics file: " + tmp_path);
            return;
        }
    }

    if (std::rename(tmp_path.c_str(), cfg_.metrics_file.c_str()) != 0)
    {
        log_warn("failed to rename metrics file into place (" + tmp_path + " -> " + cfg_.metrics_file +
                  "): " + errno_str());
    }
}

void Server::cleanup()
{
    log_info("shutting down");

    if (pool_)
    {
        pool_->shutdown();
        pool_.reset();
    }

    if (listen_fd_ >= 0)
    {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    for (int fd : tcp_listen_fds_)
    {
        close(fd);
    }
    tcp_listen_fds_.clear();

    // MMDB databases are closed via GeoIpDatabases' own destructor when
    // this Server is destroyed, after cleanup() returns.

    if (socket_created_)
    {
        if (unlink(cfg_.socket_path.c_str()) != 0 && errno != ENOENT)
        {
            log_warn("failed to remove socket " + cfg_.socket_path + ": " + errno_str());
        }
        else
        {
            log_info("removed socket " + cfg_.socket_path);
        }
        socket_created_ = false;
    }

    if (signal_fd_ >= 0)
    {
        close(signal_fd_);
        signal_fd_ = -1;
    }

    log_info("shutdown complete");
}

int Server::run()
{
    log_info("nshgeoip ready");

    // Listener fds first (unix, then any TCP listeners), signal fd last --
    // signal_idx below marks that boundary so the accept loop and the
    // signal handling loop each only look at their own slice.
    std::vector<pollfd> fds;
    fds.push_back({listen_fd_, POLLIN, 0});
    for (int fd : tcp_listen_fds_)
    {
        fds.push_back({fd, POLLIN, 0});
    }
    const size_t signal_idx = fds.size();
    fds.push_back({signal_fd_, POLLIN, 0});

    // Written immediately on the first loop iteration if metrics_file is
    // set (so the file exists as soon as possible rather than only after
    // the first full interval), then every metrics_interval_seconds after
    // that. Unused (poll() keeps its -1/infinite timeout, same as before
    // this feature existed) when metrics_file is empty -- no periodic
    // wake-up cost for anyone not using it.
    auto next_metrics_write = std::chrono::steady_clock::now();

    bool running = true;
    while (running)
    {
        int poll_timeout_ms = -1;
        if (!cfg_.metrics_file.empty())
        {
            auto remaining = next_metrics_write - std::chrono::steady_clock::now();
            auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            poll_timeout_ms = static_cast<int>(remaining_ms > 0 ? remaining_ms : 0);
        }

        int pr = poll(fds.data(), fds.size(), poll_timeout_ms);
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            log_error("poll() failed: " + errno_str());
            break;
        }

        for (size_t i = 0; i < signal_idx; ++i)
        {
            if (!(fds[i].revents & POLLIN))
            {
                continue;
            }

            // Drain every connection already queued before going back to
            // poll(), rather than accepting one per wake-up: under a burst,
            // the latter means one poll()+accept4() round trip per
            // connection instead of catching up with the kernel's backlog
            // in a tight loop. accept4() itself never blocks here -- POLLIN
            // only fired because at least one connection is ready, and once
            // the queue is empty it returns EAGAIN and we go back to poll().
            for (;;)
            {
                int client_fd = accept4(fds[i].fd, nullptr, nullptr, SOCK_CLOEXEC);
                if (client_fd < 0)
                {
                    if (errno != EINTR && errno != EAGAIN)
                    {
                        log_warn("accept() failed: " + errno_str());
                    }
                    break;
                }
                pool_->submit(client_fd);
            }
        }

        if (fds[signal_idx].revents & POLLIN)
        {
            signalfd_siginfo si{};
            ssize_t n = read(signal_fd_, &si, sizeof(si));
            if (n == static_cast<ssize_t>(sizeof(si)))
            {
                if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT)
                {
                    log_info(std::string("received signal ") + (si.ssi_signo == SIGTERM ? "SIGTERM" : "SIGINT") +
                             ", shutting down");
                    running = false;
                }
                else if (si.ssi_signo == SIGHUP)
                {
                    log_info("received SIGHUP, reloading databases");
                    reload_databases();
                }
            }
        }

        if (!cfg_.metrics_file.empty() && std::chrono::steady_clock::now() >= next_metrics_write)
        {
            write_metrics_file();
            next_metrics_write = std::chrono::steady_clock::now() + std::chrono::seconds(cfg_.metrics_interval_seconds);
        }
    }

    cleanup();
    return 0;
}

} // namespace nshgeoip
