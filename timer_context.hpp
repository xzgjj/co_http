// 定时器堆：用 multimap 按到期时间排序，为事件循环提供最近超时时间。
// 每个定时器可绑定一个 stop_source，用于被 I/O 完成后取消。
#pragma once

#include <map>
#include <chrono>
#include "callback.hpp"
#include "stop_source.hpp"

//到期时刻 → 回调”的表，并回答一个问题——“距离下一个定时器到期还有多久
// dt = duration_to_next_timer();     // ① 清理过期定时器 + 算出等多久
struct timer_context {
    struct _timer_entry {
        callback<> m_cb; // 到期或被唤醒时执行
        stop_source m_stop; // 外部取消该定时器的令牌
    };

    std::multimap<std::chrono::steady_clock::time_point, _timer_entry> //单调始终 时间点 绝对到期时刻；红黑树能取最近
        m_timer_heap; // “绝对时刻” = 某个时钟原点 + 一段经过的时间

    // 默认构造空定时器堆。
    timer_context() = default;

    // 定时器持有回调和迭代器，禁止移动整个上下文。
    timer_context(timer_context &&) = delete;

    // 注册定时器：到期执行 cb，或由 stop 提前取消。；另一个开始的时候必须唤醒
    void set_timeout(std::chrono::steady_clock::duration dt, callback<> cb,
                     stop_source stop = {}) {
        auto expire_time = std::chrono::steady_clock::now() + dt;                       // 折算成绝对时刻
        auto it = m_timer_heap.insert(
            {expire_time, _timer_entry{std::move(cb), stop}});             // 把“取消通道”接到表项上
        stop.set_stop_callback([this, it] {
            auto cb = std::move(it->second.m_cb);
            m_timer_heap.erase(it); // 先摘除，保证回调只执行一次
            cb();     //// 再执行
        });
    }

    // 计算距离最近定时器的时间；顺带执行所有已经到期的定时器。
    std::chrono::steady_clock::duration duration_to_next_timer() {
        while (!m_timer_heap.empty()) {
            auto it = m_timer_heap.begin();
            // 看看最近的一次计时器事件，是否已经过时？
            auto now = std::chrono::steady_clock::now();
            if (it->first <= now) {
                // 如果已经过时，则触发该定时器的回调，并删除
                it->second.m_stop.clear_stop_callback(); // 断开取消通道
                auto cb = std::move(it->second.m_cb);
                m_timer_heap.erase(it); // 先摘除，再执行
                cb();
            } else {
                return it->first - now; //最小的最先到 最小都没到就都没有
            }
        }
        return std::chrono::nanoseconds(-1); // 没有定时器，epoll 可以无限等待
    }

    // 定时器堆是否为空。
    bool is_empty() const {
        return m_timer_heap.empty();
    }
};
