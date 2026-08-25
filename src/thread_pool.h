#pragma once

// Fixed-size worker pool: the accept loop hands off each accepted
// connection fd to the pool, which queues it for whichever worker thread
// picks it up next. Chosen over thread-per-connection or an epoll event
// loop because nshgeoip only ever serves short-lived, low-QPS synchronous
// subrequests (NGINX auth_request) -- a small bounded pool gives
// predictable resource use without the complexity of an async reactor.

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace nshgeoip
{

class ThreadPool
{
public:
    // `handler` is invoked (from a worker thread) with each connection fd
    // handed to submit(); it is responsible for closing the fd when done.
    ThreadPool(std::size_t num_threads, std::function<void(int)> handler);
    ~ThreadPool();

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // Queues `fd` for handling by a worker thread. Safe to call from the
    // accept loop thread while workers are running.
    void submit(int fd);

    // Stops accepting new submissions and wakes all workers. Connections
    // already queued or in flight are allowed to finish (a clean shutdown
    // finishes serving what was already accepted; the accept loop itself
    // is responsible for not handing off anything new once shutdown has
    // begun). Blocks until all workers have exited.
    void shutdown();

private:
    void worker_loop();

    std::function<void(int)> handler_;
    std::vector<std::thread> workers_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<int> queue_;
    bool stop_ = false;
};

} // namespace nshgeoip
