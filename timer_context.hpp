// 时间轮（P4）：用固定 tick 的哈希轮 + 溢出堆替代单纯 multimap。
//
// 结构：
//   - 256 个 bucket，每个 tick = 5ms（覆盖 1.28s）；
//   - 超过覆盖范围的定时器进 overflow multimap，进入范围后迁移到轮上；
//   - 取消 = 从所在容器摘除（不执行回调）；
//   - duration_to_next_timer() 推进轮、收集到期回调、统一执行后返回下一次超时。
//
// 接口与旧 multimap 版保持一致（set_timeout / duration_to_next_timer / is_empty）。
#pragma once

#include <chrono>
#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <vector>

#include "callback.hpp"
#include "stop_source.hpp"

struct timer_context {
    using clock = std::chrono::steady_clock;
    using duration = clock::duration;
    using time_point = clock::time_point;

    static constexpr size_t kBuckets = 256;
    static constexpr duration kTick = std::chrono::milliseconds(5);

    struct _timer_entry;
    using _bucket = std::list<_timer_entry>;

    struct _timer_state {
        bool active = true;
        int bucket = -1;
        _bucket::iterator bucket_it;
        std::multimap<time_point, _timer_entry>::iterator overflow_it;
        bool in_overflow = false;
    };

    struct _timer_entry {
        callback<> m_cb;
        stop_source m_stop;
        std::shared_ptr<_timer_state> m_state;
    };

    std::vector<_bucket> m_buckets{kBuckets};
    std::multimap<time_point, _timer_entry> m_overflow;
    time_point m_next_tick{};
    size_t m_next_index = 0;
    size_t m_bucket_count = 0;

    timer_context() : m_next_tick(clock::now()) {}
    timer_context(timer_context &&) = delete;
    timer_context &operator=(timer_context &&) = delete;

    // 注册定时器：到期执行 cb；由 stop 取消时只摘除、不执行。
    void set_timeout(duration dt, callback<> cb, stop_source stop = {}) {
        if (m_bucket_count == 0) {
            m_next_tick = clock::now(); // 空闲时对齐轮起点，避免长时间空转
        }
        auto state = std::make_shared<_timer_state>();
        auto entry = _timer_entry{std::move(cb), stop, state};

        if (dt < duration::zero()) {
            dt = duration::zero();
        }
        if (dt < kTick * static_cast<long long>(kBuckets)) {
            size_t slot = (m_next_index + static_cast<size_t>(dt / kTick)) % kBuckets;
            m_buckets[slot].push_back(std::move(entry));
            state->bucket = static_cast<int>(slot);
            state->bucket_it = std::prev(m_buckets[slot].end());
            ++m_bucket_count;
        } else {
            auto expire = clock::now() + dt;
            auto it = m_overflow.insert({expire, std::move(entry)});
            state->in_overflow = true;
            state->overflow_it = it;
        }

        if (stop) {
            stop.set_stop_callback([this, state] {
                if (!state->active) {
                    return;
                }
                state->active = false;
                if (state->in_overflow) {
                    m_overflow.erase(state->overflow_it);
                    state->in_overflow = false;
                } else if (state->bucket >= 0) {
                    m_buckets[state->bucket].erase(state->bucket_it);
                    state->bucket = -1;
                    if (m_bucket_count > 0) {
                        --m_bucket_count;
                    }
                }
            });
        }
    }

    // 推进轮并执行到期回调；返回距下一次到期的时长（无定时器返回 -1ns）。
    duration duration_to_next_timer() {
        std::vector<callback<>> due;
        auto now = clock::now();

        // 1) 溢出堆里已到期的
        while (!m_overflow.empty() && m_overflow.begin()->first <= now) {
            auto it = m_overflow.begin();
            auto state = it->second.m_state;
            if (state && state->active) {
                state->active = false;
                due.push_back(std::move(it->second.m_cb));
            }
            m_overflow.erase(it);
        }

        // 2) 推进时间轮
        if (m_bucket_count > 0) {
            while (m_next_tick <= now) {
                auto &bucket = m_buckets[m_next_index];
                for (auto &entry : bucket) {
                    auto &state = entry.m_state;
                    if (state && state->active) {
                        state->active = false;
                        due.push_back(std::move(entry.m_cb));
                    }
                }
                if (!bucket.empty()) {
                    m_bucket_count -= bucket.size();
                }
                bucket.clear();
                m_next_tick += kTick;
                m_next_index = (m_next_index + 1) % kBuckets;
            }

            // 3) 溢出堆里已进入覆盖范围的迁移到轮上
            while (!m_overflow.empty()) {
                auto it = m_overflow.begin();
                if (it->first >= m_next_tick + kTick * static_cast<long long>(kBuckets)) {
                    break;
                }
                auto delta = it->first > m_next_tick ? (it->first - m_next_tick)
                                                     : duration::zero();
                size_t slot = (m_next_index + static_cast<size_t>(delta / kTick)) % kBuckets;
                auto entry = std::move(it->second);
                m_overflow.erase(it);
                entry.m_state->in_overflow = false;
                entry.m_state->bucket = static_cast<int>(slot);
                m_buckets[slot].push_back(std::move(entry));
                m_buckets[slot].back().m_state->bucket_it = std::prev(m_buckets[slot].end());
                ++m_bucket_count;
            }
        }

        // 4) 统一执回到期回调（可能再次 set_timeout / 取消）
        for (auto &cb : due) {
            cb();
        }

        // 5) 计算下一次超时
        now = clock::now();
        if (m_bucket_count > 0) {
            auto delta = m_next_tick - now;
            return delta.count() < 0 ? duration::zero() : delta;
        }
        if (!m_overflow.empty()) {
            auto delta = m_overflow.begin()->first - now;
            return delta.count() < 0 ? duration::zero() : delta;
        }
        return std::chrono::nanoseconds(-1);
    }

    [[nodiscard]] bool is_empty() const {
        return m_bucket_count == 0 && m_overflow.empty();
    }
};
