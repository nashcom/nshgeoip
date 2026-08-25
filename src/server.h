#pragma once

#include <sys/socket.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "geoip.h"
#include "metrics.h"
#include "thread_pool.h"

namespace nshgeoip
{

class Server
{
public:
    explicit Server(Config cfg);
    ~Server();

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    // Opens the databases and the listening socket(s). Returns false and
    // fills `err` on failure (nothing partially set up is left behind).
    bool init(std::string &err);

    // Runs the accept loop until SIGTERM/SIGINT. Returns a process exit
    // code (0 on clean shutdown).
    int run();

private:
    bool setup_signals(std::string &err);
    bool setup_socket(std::string &err);
    bool ensure_socket_directory(const std::string &socket_path, std::string &err);
    bool path_has_live_listener(const std::string &path) const;
    // Optional TCP/IP listener(s) -- disabled unless cfg_.tcp_port != 0. Binds
    // loopback on both IPv4 and IPv6 by default, or a single explicit
    // cfg_.tcp_address when one is given. No-op (returns true) when disabled.
    bool setup_tcp_listeners(std::string &err);
    bool bind_one_tcp(int family, const sockaddr *addr, socklen_t addrlen, std::string &err);
    void handle_connection(int fd);
    void reload_databases();
    void cleanup();

    // Current status of country_db/asn_db/city_db, for /health, /metrics,
    // and the metrics_file writer -- all three want the same snapshot.
    std::vector<MetricsDbInfo> collect_db_infos() const;
    double uptime_seconds() const;
    // Renders the same content as the /metrics HTTP endpoint and writes it
    // to cfg_.metrics_file, atomically (temp file + rename()). No-op if
    // cfg_.metrics_file is empty. Failures are logged, not fatal -- a
    // metrics_file problem should never take the daemon down.
    void write_metrics_file();

    Config cfg_;
    GeoIpDatabases db_;
    std::unique_ptr<ThreadPool> pool_;
    Metrics metrics_;
    std::chrono::steady_clock::time_point start_time_;

    int listen_fd_ = -1;
    std::vector<int> tcp_listen_fds_;
    int signal_fd_ = -1;
    bool socket_created_ = false;
};

} // namespace nshgeoip
