// 聊天服务器"应用层"共享实现：回调版单进程、多进程实验共用。
// 与协程版共用 chat_business/http_delivery/metrics（统一设计）。
#pragma once

#include "io_context.hpp"
#include "http_server.hpp"
#include "chat_business.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace chat_app {

inline size_t env_size(char const *name, size_t def) {
    char const *v = std::getenv(name);
    if (!v || !*v) {
        return def;
    }
    try {
        return static_cast<size_t>(std::stoull(v));
    } catch (std::exception const &) {
        return def;
    }
}

inline int env_int(char const *name, int def) {
    char const *v = std::getenv(name);
    if (!v || !*v) {
        return def;
    }
    try {
        return std::stoi(v);
    } catch (std::exception const &) {
        return def;
    }
}

// 静态资源：ETag/Last-Modified + 条件 304 + gzip + sendfile。
inline void serve_static(http_server::http_request &request, std::string const &url) {
    auto *asset = chat::find_static(url);
    if (!asset) {
        return request.write_response(404, "404 Not Found");
    }
    auto inm = request.headers.find("if-none-match");
    auto ae = request.headers.find("accept-encoding");
    auto response = build_static_response(
        *asset, inm != request.headers.end() ? inm->second : "",
        ae != request.headers.end() ? ae->second : "");
    bool compressed = false;
    for (auto const &[key, value] : response.headers) {
        if (key == "Content-Encoding") {
            compressed = true;
        }
        request.set_header(key, value);
    }
    if (response.status == 200 && !compressed && !request.head && !request.m_tls &&
        asset->fd >= 0) {
        return request.write_file_response(200, asset->fd, asset->content.size(),
                                           response.content_type);
    }
    request.write_response(response.status, response.body, response.content_type);
}

inline void register_routes(http_server &server) {
    auto &router = server.get_router();

    router.route(http_method::GET, "/", [](http_server::http_request &request) {
        serve_static(request, "/");
    });
    router.route(http_method::GET, "/favicon.ico",
                 [](http_server::http_request &request) {
                     serve_static(request, "/favicon.ico");
                 });
    router.route(http_method::GET, "/healthz", [](http_server::http_request &request) {
        request.set_header("Cache-Control", "no-store");
        request.write_response(200, "ok");
    });
    router.route(http_method::GET, "/metrics", [](http_server::http_request &request) {
        request.set_header("Cache-Control", "no-store");
        request.write_response(200, metrics_json(), "application/json");
    });

    // SSE：服务端推送（Connection: close 定界；浏览器 EventSource 可直接用）。
    router.route(http_method::GET, "/events",
                 [](http_server::http_request &request) {
                     request.begin_sse(chat::parse_cursor_query(request.query));
                 });
    // WebSocket：校验升级头 → 101 → 帧循环（文本消息发布到聊天 hub）。
    router.route(http_method::GET, "/ws", [](http_server::http_request &request) {
        if (!realtime::ws_is_upgrade(request.headers)) {
            return request.write_response(400, "400 Bad Request");
        }
        auto key = request.headers.find("sec-websocket-key");
        if (key == request.headers.end() || key->second.empty()) {
            return request.write_response(400, "400 Bad Request");
        }
        request.begin_websocket(realtime::ws_accept_key(key->second));
    });
    router.route(http_method::POST, "/send",
                 [](http_server::http_request &request) {
                     if (!chat::send_message(request.body)) {
                         return request.write_response(400, "400 Bad Request");
                     }
                     request.write_response(200, "OK");
                 });
    router.route(http_method::POST, "/recv",
                 [&server](http_server::http_request &request) {
                     size_t since = 0;
                     if (chat::parse_recv(request.body, since) ==
                         chat::recv_result::invalid) {
                         return request.write_response(400, "400 Bad Request");
                     }
                     if (chat::recv_immediate(since)) {
                         return request.write_response(200, chat::recv_slice(since));
                     }
                     auto holder = request.m_holder;
                     auto state = chat::subscribe_long_poll([holder, since] {
                         auto conn = holder.lock();
                         if (!conn || conn->is_closed()) {
                             return;
                         }
                         conn->m_request.write_response(200, chat::recv_slice(since));
                     });
                     state->timer_stop = stop_source::make();
                     int timeout_ms = server.get_config().poll_timeout_ms;
                     io_context::get().set_timeout(
                         std::chrono::milliseconds(timeout_ms),
                         [state] {
                             if (state->done) {
                                 return;
                             }
                             state->done = true;
                             io_context::get().post([state] { state->respond(); });
                         },
                         state->timer_stop);
                 });
}

// 信号处理：只置标志 + 写 eventfd（async-signal-safe）。
inline void on_signal(int) {
    g_quit = 1;
    if (g_signal_wake_fd >= 0) {
        uint64_t one = 1;
        ssize_t r = write(g_signal_wake_fd, &one, sizeof(one));
        (void)r;
    }
}

inline void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// 加载静态资源、初始化业务状态（含可选 SQLite），注册路由并进入事件循环。
inline int run() {
    io_context ctx;
    metrics().started_ms = now_ms();

    char const *tls_cert = std::getenv("TLS_CERT");
    char const *tls_key = std::getenv("TLS_KEY");
    if (tls_cert && *tls_cert && tls_key && *tls_key) {
        if (!tls().init(tls_cert, tls_key)) {
            throw std::runtime_error("TLS init failed");
        }
        std::fprintf(stderr, "[chat] TLS enabled\n");
    }

    char const *static_root = std::getenv("STATIC_ROOT");
    if (!static_root || !*static_root) {
        static_root = "../static";
    }
    if (chdir(static_root) != 0) {
        std::fprintf(stderr, "[chat] chdir '%s' failed: %s\n", static_root,
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

    auto srv = http_server::make();
    auto &conf = srv->get_config();
    conf.max_header = env_size("MAX_HEADER", conf.max_header);
    conf.max_url = env_size("MAX_URL", conf.max_url);
    conf.max_body = env_size("MAX_BODY", conf.max_body);
    conf.max_conn = env_size("MAX_CONN", conf.max_conn);
    conf.request_timeout_ms = env_int("REQUEST_TIMEOUT_MS", conf.request_timeout_ms);
    conf.write_timeout_ms = env_int("WRITE_TIMEOUT_MS", conf.write_timeout_ms);
    conf.poll_timeout_ms = env_int("POLL_TIMEOUT_MS", conf.poll_timeout_ms);
    conf.sse_heartbeat_ms = env_int("SSE_HEARTBEAT_MS", conf.sse_heartbeat_ms);

    register_routes(*srv);

    char const *port = std::getenv("PORT");
    if (!port || !*port) {
        port = "8080";
    }
    std::fprintf(stderr, "[chat] listening on 0.0.0.0:%s (static=%s, pid=%d)\n",
                 port, static_root, static_cast<int>(getpid()));
    srv->do_start("0.0.0.0", port);

    ctx.join();
    return 0;
}

} // namespace chat_app
