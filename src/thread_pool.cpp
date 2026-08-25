// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "thread_pool.h"

#include <unistd.h>

namespace nshgeoip
{

ThreadPool::ThreadPool(std::size_t num_threads, std::function<void(int)> handler) : handler_(std::move(handler))
{
    workers_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
    {
        workers_.emplace_back(&ThreadPool::worker_loop, this);
    }
}

ThreadPool::~ThreadPool()
{
    shutdown();
}

void ThreadPool::submit(int fd)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_)
        {
            ::close(fd);
            return;
        }
        queue_.push(fd);
    }
    cv_.notify_one();
}

void ThreadPool::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_)
        {
            return;
        }
        stop_ = true;
    }
    cv_.notify_all();

    // Workers keep draining the queue after stop_ is set (see
    // worker_loop()), so by the time join() returns everything that was
    // queued has already been handled.
    for (auto &t : workers_)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    workers_.clear();
}

void ThreadPool::worker_loop()
{
    for (;;)
    {
        int fd = -1;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty())
            {
                return;
            }
            if (queue_.empty())
            {
                // stop_ was set with items still queued elsewhere being
                // drained by shutdown(); nothing to do here.
                continue;
            }
            fd = queue_.front();
            queue_.pop();
        }
        handler_(fd);
    }
}

} // namespace nshgeoip
