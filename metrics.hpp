// 进程级可观测指标（回调版与协程版共用）：/healthz、/metrics 用。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

struct server_metrics {
    std::atomic<size_t> requests{0};
    std::atomic<size_t> errors{0};
    std::atomic<size_t> active_connections{0};
    std::atomic<size_t> connections_total{0};
    std::atomic<size_t> bytes_sent{0};
    std::atomic<size_t> timeouts{0};
    std::atomic<size_t> sendfile_calls{0};
    std::atomic<size_t> writev_calls{0};
    std::atomic<size_t> sse_total{0};
    std::atomic<size_t> ws_total{0};
    std::atomic<int64_t> started_ms{0};
};

inline server_metrics &metrics() {
    static server_metrics instance;
    return instance;
}

inline int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline std::string metrics_json() {
    auto &m = metrics();
    int64_t started = m.started_ms.load();
    int64_t uptime_s = started == 0 ? 0 : (now_ms() - started) / 1000;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"requests_total\":%zu,\"errors_total\":%zu,"
                  "\"active_connections\":%zu,\"connections_total\":%zu,"
                  "\"bytes_sent\":%zu,\"timeouts_total\":%zu,"
                  "\"sendfile_total\":%zu,\"writev_total\":%zu,\"sse_total\":%zu,\"ws_total\":%zu,\"uptime_s\":%lld}\n",
                  m.requests.load(), m.errors.load(),
                  m.active_connections.load(), m.connections_total.load(),
                  m.bytes_sent.load(), m.timeouts.load(),
                  m.sendfile_calls.load(), m.writev_calls.load(), m.sse_total.load(),
                  m.ws_total.load(),
                  static_cast<long long>(uptime_s));
    return buf;
}
