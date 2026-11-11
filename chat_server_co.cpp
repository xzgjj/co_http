// 协程版聊天服务器（P2）：与回调版共用 io_context / codec / chat 业务。
//
// 构建：CMake 对"仅本文件"开启 C++20（全局仍是 C++17），产物 chat_server_co。
// 自检：./chat_server_co --selftest（Task 组合 + 协程 sleep + 工作窃取线程池）。
#include "chat_business.hpp"
#include "co_http_server.hpp"
#include "co_io.hpp"
#include "scheduler.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>

using namespace std::chrono_literals;

// ---------------- 环境变量 ----------------

static size_t env_size(char const *name, size_t def) {
    char const *v = std::getenv(name);
    if (!v || !*v) return def;
    try {
        return static_cast<size_t>(std::stoull(v));
    } catch (std::exception const &) {
        return def;
    }
}

static int env_int(char const *name, int def) {
    char const *v = std::getenv(name);
    if (!v || !*v) return def;
    try {
        return std::stoi(v);
    } catch (std::exception const &) {
        return def;
    }
}

// ---------------- 协程业务处理器 ----------------

static co_response static_for(co_request &request, std::string const &url) {
    auto *asset = chat::find_static(url);
    co_response res;
    if (!asset) {
        res.status = 404;
        res.body = "404 Not Found";
        return res;
    }
    auto inm = request.headers.find("if-none-match");
    auto ae = request.headers.find("accept-encoding");
    auto built = build_static_response(
        *asset, inm != request.headers.end() ? inm->second : "",
        ae != request.headers.end() ? ae->second : "");
    res.status = built.status;
    res.body = built.body;
    res.content_type = built.content_type;
    res.headers = built.headers;

    bool compressed = false;
    for (auto const &[key, value] : built.headers) {
        if (key == "Content-Encoding") {
            compressed = true;
        }
    }
    if (res.status == 200 && !compressed && !request.head && !request.tls &&
        asset->fd >= 0) {
        res.sendfile_fd = asset->fd;
        res.sendfile_size = asset->content.size();
        res.body.clear();
    }
    return res;
}

static Task<co_response> handle_index(co_request &request) {
    co_return static_for(request, "/");
}

static Task<co_response> handle_favicon(co_request &request) {
    co_return static_for(request, "/favicon.ico");
}

static Task<co_response> handle_metrics(co_request &) {
    co_response res;
    res.status = 200;
    res.body = metrics_json();
    res.content_type = "application/json";
    res.set_header("Cache-Control", "no-store");
    co_return res;
}

static Task<co_response> handle_send(co_request &request) {
    co_response res;
    if (!chat::send_message(request.body)) {
        res.status = 400;
        res.body = "400 Bad Request";
        co_return res;
    }
    res.status = 200;
    res.body = "OK";
    co_return res;
}

static int g_poll_timeout_ms = 3000;

static Task<co_response> handle_recv(co_request &request) {
    co_response res;
    size_t since = 0;
    if (chat::parse_recv(request.body, since) == chat::recv_result::invalid) {
        res.status = 400;
        res.body = "400 Bad Request";
        co_return res;
    }
    if (chat::recv_immediate(since)) {
        res.status = 200;
        res.body = chat::recv_slice(since);
        co_return res;
    }

    // 长轮询：发布方/超时都通过 async_event 唤醒本协程。
    auto event = std::make_shared<coio::async_event>();
    auto state = chat::subscribe_long_poll([event] { event->notify(); });
    state->timer_stop = stop_source::make();
    io_context::get().set_timeout(
        std::chrono::milliseconds(g_poll_timeout_ms),
        [state, event] {
            if (state->done) {
                return;
            }
            state->done = true;
            event->notify();
        },
        state->timer_stop);

    co_await *event;
    state->timer_stop.request_stop();

    res.status = 200;
    res.body = chat::recv_slice(since);
    co_return res;
}

static Task<co_response> handle_healthz(co_request &) {
    co_response res;
    res.status = 200;
    res.body = "ok";
    co_return res;
}

// SSE：交给 serve_sse 写 text/event-stream 流（先重放历史，再推新消息）。
static Task<co_response> handle_events(co_request &request) {
    co_response res;
    res.stream = co_response::stream_kind::sse;
    res.sse_cursor = chat::parse_cursor_query(request.query);
    co_return res;
}

// WebSocket：校验升级头并计算 accept；serve_ws 负责 101 与帧循环。
static Task<co_response> handle_ws(co_request &request) {
    co_response res;
    if (!realtime::ws_is_upgrade(request.headers)) {
        res.status = 400;
        res.body = "400 Bad Request";
        co_return res;
    }
    auto key = request.headers.find("sec-websocket-key");
    if (key == request.headers.end() || key->second.empty()) {
        res.status = 400;
        res.body = "400 Bad Request";
        co_return res;
    }
    res.stream = co_response::stream_kind::websocket;
    res.ws_accept = realtime::ws_accept_key(key->second);
    co_return res;
}

// ---------------- 启动 ----------------

static void server() {
    io_context ctx;

    char const *static_root = std::getenv("STATIC_ROOT");
    if (!static_root || !*static_root) static_root = "../static";
    if (chdir(static_root) != 0) {
        std::fprintf(stderr, "[chat-co] chdir '%s' failed: %s\n", static_root,
                     strerror(errno));
    }
    chat::load_static_file("index.html", "/", "text/html;charset=utf-8");
    chat::load_static_file("favicon.ico", "/favicon.ico", "image/x-icon");
    chat::get_hub().append({"系统", "你好，欢迎来到聊天室"});
    char const *db = std::getenv("CHAT_DB");
    if (db && *db) {
        chat::init_storage(db);
        chat::load_storage_into_hub(chat::get_hub());
    }

    auto srv = co_http_server::make();
    auto &conf = srv->get_config();
    conf.max_header = env_size("MAX_HEADER", conf.max_header);
    conf.max_url = env_size("MAX_URL", conf.max_url);
    conf.max_body = env_size("MAX_BODY", conf.max_body);
    conf.max_conn = env_size("MAX_CONN", conf.max_conn);
    conf.request_timeout_ms = env_int("REQUEST_TIMEOUT_MS", conf.request_timeout_ms);
    conf.write_timeout_ms = env_int("WRITE_TIMEOUT_MS", conf.write_timeout_ms);
    conf.poll_timeout_ms = env_int("POLL_TIMEOUT_MS", conf.poll_timeout_ms);
    conf.sse_heartbeat_ms = env_int("SSE_HEARTBEAT_MS", conf.sse_heartbeat_ms);
    int poll_timeout_ms = conf.poll_timeout_ms;
    g_poll_timeout_ms = poll_timeout_ms;
    metrics().started_ms = now_ms();

    char const *tls_cert = std::getenv("TLS_CERT");
    char const *tls_key = std::getenv("TLS_KEY");
    if (tls_cert && *tls_cert && tls_key && *tls_key) {
        if (!tls().init(tls_cert, tls_key)) {
            throw std::runtime_error("TLS init failed");
        }
        std::fprintf(stderr, "[chat-co] TLS enabled\n");
    }

    auto &router = srv->get_router();
    router.route(http_method::GET, "/", handle_index);
    router.route(http_method::GET, "/favicon.ico", handle_favicon);
    router.route(http_method::POST, "/send", handle_send);
    router.route(http_method::GET, "/healthz", handle_healthz);
    router.route(http_method::GET, "/metrics", handle_metrics);
    router.route(http_method::GET, "/events", handle_events);
    router.route(http_method::GET, "/ws", handle_ws);
    router.route(http_method::POST, "/recv", handle_recv);

    char const *port = std::getenv("PORT");
    if (!port || !*port) port = "8080";
    std::fprintf(stderr, "[chat-co] listening on 0.0.0.0:%s (static=%s)\n", port,
                 static_root);
    srv->do_start("0.0.0.0", port);

    ctx.join();
}

// ---------------- 信号 ----------------

static void on_signal(int) {
    g_quit = 1;
    if (g_signal_wake_fd >= 0) {
        uint64_t one = 1;
        ssize_t r = write(g_signal_wake_fd, &one, sizeof(one));
        (void)r;
    }
}

// ---------------- 自检（ctest: co_smoke） ----------------

static Task<int> add_numbers(int a, int b) {
    co_return a + b;
}

static Task<void> sleep_once(bool *flag) {
    co_await coio::sleep_for(20ms);
    *flag = true;
}

static Task<void> assign_sum(int *got) {
    *got = co_await add_numbers(2, 3);
}

static bool selftest() {
    bool ok = true;

    // 1) Task 组合 + 返回值
    int got = 0;
    co_spawn(assign_sum(&got));
    if (got != 5) {
        std::fprintf(stderr, "selftest: task composition failed (got %d)\n", got);
        ok = false;
    }

    // 2) 协程 sleep（复用 io_context 定时器）
    {
        io_context ctx;
        bool slept = false;
        co_spawn(sleep_once(&slept));
        ctx.join();
        if (!slept) {
            std::fprintf(stderr, "selftest: coio::sleep_for failed\n");
            ok = false;
        }
    }

    // 3) 工作窃取线程池
    if (!work_stealing_scheduler::selftest()) {
        ok = false;
    }

    std::printf("co selftest: %s\n", ok ? "OK" : "FAILED");
    return ok;
}

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("co_http %s (coroutine)\n", CO_HTTP_VERSION);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--help") == 0) {
        std::printf("usage: %s [--version] [--help] [--selftest]\n"
                    "  env: PORT STATIC_ROOT MAX_HEADER MAX_URL MAX_BODY MAX_CONN\n"
                    "       REQUEST_TIMEOUT_MS WRITE_TIMEOUT_MS POLL_TIMEOUT_MS\n",
                    argv[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--selftest") == 0) {
        signal(SIGPIPE, SIG_IGN);
        return selftest() ? 0 : 1;
    }

    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    try {
        server();
    } catch (std::exception const &e) {
        std::fprintf(stderr, "[chat-co] fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
