// timer_context（时间轮）单元测试：到期顺序、取消、溢出迁移、批量定时器。
#include "timer_context.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
        }                                                                    \
    } while (0)

static void run_until_empty(timer_context &timers) {
    for (int i = 0; i < 200000 && !timers.is_empty(); ++i) {
        auto delta = timers.duration_to_next_timer();
        if (delta.count() < 0) {
            break;
        }
        if (delta.count() > 0) {
            std::this_thread::sleep_for(delta);
        } else {
            std::this_thread::yield();
        }
    }
}

static void test_ordering() {
    timer_context timers;
    std::vector<int> order;
    timers.set_timeout(60ms, [&] { order.push_back(60); });
    timers.set_timeout(10ms, [&] { order.push_back(10); });
    timers.set_timeout(35ms, [&] { order.push_back(35); });
    run_until_empty(timers);
    CHECK(order.size() == 3);
    if (order.size() == 3) {
        CHECK(order[0] == 10);
        CHECK(order[1] == 35);
        CHECK(order[2] == 60);
    }
    CHECK(timers.is_empty());
}

static void test_cancel() {
    timer_context timers;
    int fired = 0;
    auto stop = stop_source::make();
    timers.set_timeout(30ms, [&] { ++fired; }, stop);
    stop.request_stop();
    CHECK(timers.is_empty()); // 取消后立即摘除
    run_until_empty(timers);
    CHECK(fired == 0);
}

static void test_overflow_migration() {
    timer_context timers;
    int fired = 0;
    // 1.3s > 256*5ms = 1.28s，会先进入 overflow 再迁移到轮上
    timers.set_timeout(1300ms, [&] { ++fired; });
    CHECK(!timers.is_empty());
    run_until_empty(timers);
    CHECK(fired == 1);
}

static void test_many_timers() {
    timer_context timers;
    int fired = 0;
    for (int i = 0; i < 1000; ++i) {
        timers.set_timeout(std::chrono::milliseconds(5 + (i % 90)),
                           [&] { ++fired; });
    }
    run_until_empty(timers);
    CHECK(fired == 1000);
    CHECK(timers.is_empty());
}

static void test_cancel_after_fire_no_effect() {
    timer_context timers;
    int fired = 0;
    auto stop = stop_source::make();
    timers.set_timeout(5ms, [&] { ++fired; }, stop);
    run_until_empty(timers);
    CHECK(fired == 1);
    stop.request_stop(); // 已触发后再取消：不应崩溃、不应重复执行
    CHECK(fired == 1);
}

int main() {
    test_ordering();
    test_cancel();
    test_overflow_migration();
    test_many_timers();
    test_cancel_after_fire_no_effect();

    if (g_failures == 0) {
        std::printf("timer_tests: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "timer_tests: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
