// 工作窃取线程池（自建；co_async 本身没有 work-stealing 实现）。
//
// 结构：
//   - 每个 worker 一个本地 deque + 互斥量 + 条件变量；
//   - submit 轮询选一个 worker 入队并唤醒；
//   - 空闲 worker 先看本地，再从其他 worker 的队尾一次窃取一个任务；
//   - worker 只跑"纯 CPU 任务"（fd/定时器必须留在 I/O 线程）。
//
// 默认构建下服务器仍单线程运行；`chat_server_co --selftest` 会启动线程池做自检。
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "callback.hpp"

struct work_stealing_scheduler {
    struct worker {
        std::deque<callback<>> queue;
        std::mutex mutex;
        std::condition_variable cv;
        bool stop = false;
    };

    std::vector<std::unique_ptr<worker>> m_workers;
    std::vector<std::thread> m_threads;
    std::atomic<size_t> m_next{0};
    bool m_started = false;

    explicit work_stealing_scheduler(size_t threads = 0) {
        if (threads > 0) {
            start(threads);
        }
    }

    ~work_stealing_scheduler() {
        stop();
    }

    work_stealing_scheduler(work_stealing_scheduler const &) = delete;
    work_stealing_scheduler &operator=(work_stealing_scheduler const &) = delete;

    void start(size_t threads) {
        if (m_started || threads == 0) {
            return;
        }
        m_started = true;
        for (size_t i = 0; i < threads; ++i) {
            m_workers.push_back(std::make_unique<worker>());
        }
        for (size_t i = 0; i < threads; ++i) {
            m_threads.emplace_back([this, i] { run_worker(i); });
        }
    }

    [[nodiscard]] size_t worker_count() const {
        return m_workers.size();
    }

    // 投递一个任务；线程安全，可从 I/O 线程或 worker 调用。
    void submit(callback<> job) {
        if (!m_started) {
            job();
            return;
        }
        size_t index = m_next.fetch_add(1) % m_workers.size();
        auto &w = *m_workers[index];
        {
            std::lock_guard<std::mutex> lock(w.mutex);
            w.queue.push_back(std::move(job));
        }
        w.cv.notify_one();
    }

    void stop() {
        if (!m_started) {
            return;
        }
        m_started = false;
        for (auto &w : m_workers) {
            {
                std::lock_guard<std::mutex> lock(w->mutex);
                w->stop = true;
            }
            w->cv.notify_all();
        }
        for (auto &t : m_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        m_threads.clear();
        m_workers.clear();
    }

private:
    void run_worker(size_t index) {
        for (;;) {
            callback<> job;
            auto &self = *m_workers[index];
            {
                std::unique_lock<std::mutex> lock(self.mutex);
                self.cv.wait(lock, [&] { return self.stop || !self.queue.empty(); });
                if (self.stop && self.queue.empty()) {
                    return;
                }
                if (!self.queue.empty()) {
                    job = std::move(self.queue.front());
                    self.queue.pop_front();
                }
            }
            if (!job) {
                job = steal(index);
            }
            if (job) {
                job();
            }
        }
    }

    // 从其他 worker 队尾窃取一个任务；窃不到返回空。
    callback<> steal(size_t self_index) {
        size_t count = m_workers.size();
        for (size_t offset = 1; offset < count; ++offset) {
            auto &victim = *m_workers[(self_index + offset) % count];
            std::lock_guard<std::mutex> lock(victim.mutex);
            if (!victim.queue.empty()) {
                callback<> job = std::move(victim.queue.back());
                victim.queue.pop_back();
                return job;
            }
        }
        // 没窃到：短暂让出 CPU，避免空转。
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        return {};
    }

public:
    // 自检：任务全部执行、至少用到 2 个线程。
    static bool selftest() {
        work_stealing_scheduler pool(4);
        std::atomic<int> executed{0};
        std::mutex ids_mutex;
        std::vector<std::thread::id> ids;

        for (int i = 0; i < 200; ++i) {
            bool slow = (i % 8 == 0);
            pool.submit([&executed, &ids, &ids_mutex, slow] {
                if (slow) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                executed.fetch_add(1);
                std::lock_guard<std::mutex> lock(ids_mutex);
                ids.push_back(std::this_thread::get_id());
            });
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (executed.load() < 200 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        bool ok = executed.load() == 200 && ids.size() >= 2;
        std::printf("scheduler selftest: executed=%d threads=%zu %s\n",
                    executed.load(), ids.size(), ok ? "OK" : "FAILED");
        return ok;
    }
};
