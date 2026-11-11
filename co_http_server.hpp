// 协程版 HTTP 服务端（P2）：线性 co_await 取代连接状态机回调。
//
// 复用（统一设计）：
//   - 同一个 io_context / 操作槽 / 定时器 / 取消机制（P0 内核）；
//   - 同一个 http_codec 解析器与响应写入器；
//   - 同一份 chat 业务逻辑（chat_business.hpp）。
// 差异只在控制流：serve() 是线性协程，不再有 do_read/do_handle/do_write 三段续体。
#pragma once

#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "co_io.hpp"
#include "expected.hpp"
#include "http_server.hpp" // 复用 config / http_method / http_codec
#include "io_context.hpp"
#include "task.hpp"
#include "ws_hub.hpp"

struct co_request {
    http_method method = http_method::UNKNOWN;
    std::string path;
    std::string query;
    std::string version;
    std::string body;
    string_map headers;
    bool keep_alive = true;
    bool head = false;
    bool tls = false;
};

struct co_response {
    int status = 200;
    std::string body;
    std::string content_type = "text/plain;charset=utf-8";
    std::vector<std::pair<std::string, std::string>> headers;
    bool head = false;
    bool keep_alive = true;
    int sendfile_fd = -1; // >=0 时正文走 sendfile
    size_t sendfile_size = 0;

    enum class stream_kind { none, sse, websocket };
    stream_kind stream = stream_kind::none;
    std::string ws_accept;
    size_t sse_cursor = 0;

    void set_header(std::string key, std::string value) {
        headers.emplace_back(std::move(key), std::move(value));
    }
};

// 协议错误：由 read_request 抛出，统一转成错误响应。
struct protocol_error {
    int status;
};

struct co_http_server : std::enable_shared_from_this<co_http_server> {
    using ptr = std::shared_ptr<co_http_server>;
    using handler = std::function<Task<co_response>(co_request &)>;

    static constexpr int kAnyMethod = -1;

    struct router_type {
        std::map<std::string, std::map<int, handler>> routes;

        void route(http_method method, std::string path, handler cb) {
            routes[std::move(path)][static_cast<int>(method)] = std::move(cb);
        }
        void route_any(std::string path, handler cb) {
            routes[std::move(path)][kAnyMethod] = std::move(cb);
        }

        static std::string allow_list(std::map<int, handler> const &by_method) {
            std::string allow;
            for (auto const &[method, cb] : by_method) {
                (void)cb;
                if (method == kAnyMethod) {
                    continue;
                }
                if (!allow.empty()) {
                    allow += ", ";
                }
                allow += http_method_name(static_cast<http_method>(method));
            }
            if (!allow.empty()) {
                allow += ", ";
            }
            allow += "OPTIONS";
            return allow;
        }
    };

    http_server::config m_conf;
    async_file m_listening;
    address_resolver::address m_addr;
    router_type m_router;
    std::shared_ptr<size_t> m_active = std::make_shared<size_t>(0);
    stop_source m_accept_backoff;

    static ptr make() {
        return std::make_shared<ptr::element_type>();
    }

    http_server::config &get_config() {
        return m_conf;
    }
    router_type &get_router() {
        return m_router;
    }

    void do_start(std::string name, std::string port) {
        address_resolver resolver;
        auto entry = resolver.resolve(name, port);
        m_listening = async_file::async_bind(entry);
        do_accept();
    }

    void do_accept() {
        m_listening.async_accept(
            m_addr, [self = shared_from_this()](expected<int> ret) {
                if (ret.error()) {
                    int err = ret.error();
                    if (err == -EAGAIN || err == -EINTR) {
                        return self->do_accept();
                    }
                    if (err == -EMFILE || err == -ENFILE) {
                        std::fprintf(stderr, "[co-http] accept: %s, retry in 100ms\n",
                                     strerror(-err));
                        self->m_accept_backoff = stop_source::make();
                        io_context::get().set_timeout(
                            100ms, [self] { self->do_accept(); },
                            self->m_accept_backoff);
                        return;
                    }
                    std::fprintf(stderr, "[co-http] accept error: %s\n", strerror(-err));
                    return self->do_accept();
                }

                int connfd = ret.value();
                if (*self->m_active >= self->m_conf.max_conn) {
                    close(connfd);
                    return self->do_accept();
                }
                int on = 1;
                setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
                ++metrics().active_connections;
                ++metrics().connections_total;
                co_spawn(run_connection(self, connfd));
                return self->do_accept();
            });
    }

    // 持有 server 的 shared_ptr，防止连接协程运行期间 server 被销毁。
    static Task<void> run_connection(ptr self, int fd) {
        struct guard {
            std::shared_ptr<size_t> counter;
            ~guard() {
                if (counter && *counter > 0) {
                    --*counter;
                }
                if (metrics().active_connections.load() > 0) {
                    --metrics().active_connections;
                }
            }
        } g{self->m_active};
        ++*self->m_active;
        co_await self->serve(fd);
    }

    Task<void> serve(int fd) {
        async_file conn{fd};
        if (tls().enabled()) {
            conn.adopt_tls(tls().new_ssl(fd));
            auto hs = co_await coio::tls_handshake{conn};
            if (hs.error()) {
                co_return;
            }
        }
        http_request_parser<> parser;
        std::string pending;
        bool head = false;
        int error_status = 0; // 协程 catch 里不能 co_await，先记状态再统一回错误
        try {
            for (;;) {
                auto request_start = std::chrono::steady_clock::now();
                // 整请求 deadline：到点取消当前读操作（不是直接关连接，避免悬垂 awaiter）。
                stop_source read_stop = stop_source::make();
                stop_source deadline = stop_source::make();
                io_context::get().set_timeout(
                    std::chrono::milliseconds(m_conf.request_timeout_ms),
                    [read_stop] {
                        ++metrics().timeouts;
                        read_stop.request_stop();
                    },
                    deadline);

                co_request request;
                bool got = co_await read_request(conn, pending, parser, request, read_stop);
                deadline.request_stop(); // 取消 deadline 定时器
                if (!got) {
                    break;
                }
                request.tls = conn.tls_enabled();
                head = request.head;
                co_response response = co_await dispatch(request);
                response.head = request.head;
                response.keep_alive = request.keep_alive;

                if (response.stream == co_response::stream_kind::sse) {
                    co_await serve_sse(conn, response.sse_cursor);
                    break;
                }
                if (response.stream == co_response::stream_kind::websocket) {
                    co_await serve_ws(conn, response.ws_accept, std::move(pending));
                    break;
                }
                co_await write_response(conn, response);

                auto elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - request_start)
                        .count();
                auto &m = metrics();
                ++m.requests;
                m.bytes_sent.fetch_add(response.body.size());
                std::fprintf(stderr, "[access] %s %s %d %zuB %lldms\n",
                             std::string(http_method_name(request.method)).c_str(),
                             request.path.c_str(), response.status,
                             response.body.size(),
                             static_cast<long long>(elapsed_ms));

                if (!request.keep_alive) {
                    break;
                }
            }
        } catch (protocol_error const &e) {
            ++metrics().errors;
            error_status = e.status;
        } catch (std::exception const &e) {
            ++metrics().errors;
            std::fprintf(stderr, "[co-http] connection exception: %s\n", e.what());
        }
        if (error_status != 0) {
            co_await send_error(conn, error_status, head);
        }
    }

    // 读一个完整请求；可能从 pending（流水线剩余）继续，也可能继续 socket 读。
    Task<bool> read_request(async_file &conn, std::string &pending,
                            http_request_parser<> &parser, co_request &request,
                            stop_source read_stop) {
        parser.reset_state();
        for (;;) {
            if (!pending.empty()) {
                std::string chunk = std::move(pending);
                pending.clear();
                parser.push_chunk(bytes_const_view{chunk.data(), chunk.size()});
            } else {
                bytes_buffer in{8192};
                auto ret = co_await coio::read{conn, in, read_stop};
                if (ret.error()) {
                    co_return false;
                }
                size_t n = ret.value();
                if (n == 0) {
                    co_return false; // EOF
                }
                parser.push_chunk(in.subspan(0, n));
            }

            // 头部上限：无论本次是否刚好读完头部，都按累计长度判断。
            if (parser.headers_raw().size() > m_conf.max_header) {
                throw protocol_error{431};
            }
            if (!parser.header_finished()) {
                continue;
            }

            // 头部完整后的校验（与回调版保持同一套规则）。
            if (parser.url().size() > m_conf.max_url) {
                throw protocol_error{414};
            }
            if (parser.bad_header_line() || parser.duplicate_content_length() ||
                !parser.cl_valid()) {
                throw protocol_error{400};
            }
            if (parser.chunked()) {
                throw protocol_error{501};
            }
            http_method method = parser.method();
            std::string version = parser.version();
            if (method == http_method::UNKNOWN) {
                throw protocol_error{501};
            }
            if (version != "HTTP/1.1" && version != "HTTP/1.0") {
                throw protocol_error{505};
            }
            auto &headers = parser.headers();
            if (version == "HTTP/1.1" && headers.find("host") == headers.end()) {
                throw protocol_error{400};
            }
            bool expects_body = method == http_method::POST ||
                                method == http_method::PUT ||
                                method == http_method::PATCH;
            if (expects_body && !parser.cl_present()) {
                throw protocol_error{411};
            }
            if (parser.content_length() > m_conf.max_body ||
                parser.body().size() > m_conf.max_body) {
                throw protocol_error{413};
            }
            if (!parser.request_finished()) {
                continue;
            }

            request.method = method;
            request.path = parser.path();
            request.query = parser.query();
            request.version = version;
            request.headers = headers;
            request.body = std::move(parser.body());
            request.head = (method == http_method::HEAD);
            request.keep_alive = keep_alive_for(version, headers);
            pending = parser.take_leftover();
            parser.reset_state();
            co_return true;
        }
    }

    static bool header_has_token(string_map const &headers, char const *name,
                                 char const *token) {
        auto it = headers.find(name);
        if (it == headers.end()) {
            return false;
        }
        std::string value = it->second;
        for (char &c : value) {
            if ('A' <= c && c <= 'Z') {
                c += 'a' - 'A';
            }
        }
        return value.find(token) != std::string::npos;
    }

    static bool keep_alive_for(std::string const &version, string_map const &headers) {
        if (header_has_token(headers, "connection", "close")) {
            return false;
        }
        if (version == "HTTP/1.0") {
            return header_has_token(headers, "connection", "keep-alive");
        }
        return true;
    }

    Task<co_response> dispatch(co_request &request) {
        auto path_it = m_router.routes.find(request.path);
        if (path_it == m_router.routes.end()) {
            co_return make_error(404);
        }
        auto &by_method = path_it->second;
        std::string allow = router_type::allow_list(by_method);

        if (request.method == http_method::OPTIONS) {
            co_response res;
            res.status = 200;
            res.set_header("Allow", allow);
            co_return res;
        }
        int lookup = static_cast<int>(request.method);
        if (request.method == http_method::HEAD) {
            lookup = static_cast<int>(http_method::GET);
        }
        auto it = by_method.find(lookup);
        if (it == by_method.end()) {
            it = by_method.find(kAnyMethod);
        }
        if (it == by_method.end()) {
            co_response res = make_error(405);
            res.set_header("Allow", allow);
            co_return res;
        }
        co_return co_await it->second(request);
    }

    static co_response make_error(int status) {
        co_response res;
        res.status = status;
        res.body = std::string(http_status_reason(status));
        if (!res.body.empty()) {
            res.body = std::to_string(status) + " " + res.body;
        }
        return res;
    }

    Task<void> write_response(async_file &conn, co_response const &response) {
        http_response_writer<> writer;
        writer.begin_header(response.status);
        writer.write_header("Server", "co_http");
        writer.write_header("X-Pid", std::to_string(getpid()));
        writer.write_header("Date", http_date_string());
        writer.write_header("Content-type", response.content_type);
        writer.write_header("Connection", response.keep_alive ? "keep-alive" : "close");
        for (auto const &[key, value] : response.headers) {
            writer.write_header(key, value);
        }
        size_t content_length =
            response.sendfile_fd >= 0 ? response.sendfile_size : response.body.size();
        writer.write_header("Content-length", std::to_string(content_length));
        writer.end_header();

        if (response.sendfile_fd >= 0 && !response.head && response.sendfile_size > 0) {
            ++metrics().sendfile_calls;
            co_await write_all(conn, writer.buffer());
            co_await sendfile_all(conn, response.sendfile_fd, response.sendfile_size);
            co_return;
        }
        if (!response.head && !response.body.empty()) {
            ++metrics().writev_calls;
            co_await writev_all(conn, writer.buffer(),
                                bytes_const_view{response.body.data(),
                                                 response.body.size()});
            co_return;
        }
        co_await write_all(conn, writer.buffer());
    }

    Task<void> write_all(async_file &conn, bytes_const_view buffer) {
        while (buffer.size() != 0) {
            auto ret = co_await coio::write{conn, buffer};
            if (ret.error()) {
                co_return;
            }
            size_t n = ret.value();
            if (n == 0) {
                co_return;
            }
            buffer = buffer.subspan(n);
        }
    }

    Task<void> writev_all(async_file &conn, bytes_const_view header,
                          bytes_const_view body) {
        while (header.size() != 0 || body.size() != 0) {
            auto ret = co_await coio::writev{conn, header, body};
            if (ret.error()) {
                co_return;
            }
            size_t n = ret.value();
            size_t total = header.size() + body.size();
            if (n == 0 || n >= total) {
                co_return;
            }
            if (n >= header.size()) {
                body = body.subspan(n - header.size());
                header = {};
            } else {
                header = header.subspan(n);
            }
        }
    }

    Task<void> sendfile_all(async_file &conn, int in_fd, size_t size) {
        size_t offset = 0;
        while (offset < size) {
            auto ret = co_await coio::sendfile{conn, in_fd,
                                               static_cast<off_t>(offset),
                                               size - offset};
            if (ret.error()) {
                co_return;
            }
            size_t n = ret.value();
            if (n == 0) {
                co_return;
            }
            offset += n;
        }
    }

    // 带失败检测的写（流式循环需要知道对端是否断开）。
    Task<bool> write_all_ok(async_file &conn, bytes_const_view buffer) {
        while (buffer.size() != 0) {
            auto ret = co_await coio::write{conn, buffer};
            if (ret.error()) {
                co_return false;
            }
            size_t n = ret.value();
            if (n == 0) {
                co_return false;
            }
            buffer = buffer.subspan(n);
        }
        co_return true;
    }

    // SSE：headers（Connection: close 定界）→ 初始注释 → 订阅 hub/心跳 → 写事件。
    Task<void> serve_sse(async_file &conn, size_t cursor) {
        ++metrics().sse_total;
        auto head = realtime::sse_response_head();
        if (!co_await write_all_ok(conn, bytes_const_view{head.data(), head.size()})) {
            co_return;
        }
        // 先重放历史（游标由 ?first= 指定，默认 0）。
        auto &hub0 = chat::get_hub();
        if (cursor < hub0.next_index()) {
            std::string replay = realtime::sse_event(chat::recv_slice(cursor));
            cursor = hub0.next_index();
            if (!co_await write_all_ok(conn, bytes_const_view{replay.data(), replay.size()})) {
                co_return;
            }
        }
        for (;;) {
            auto event = std::make_shared<coio::async_event>();
            auto state = chat::subscribe_long_poll([event] { event->notify(); });
            state->timer_stop = stop_source::make();
            io_context::get().set_timeout(
                std::chrono::milliseconds(m_conf.sse_heartbeat_ms),
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

            auto &hub = chat::get_hub();
            std::string chunk;
            if (cursor < hub.next_index()) {
                chunk = realtime::sse_event(chat::recv_slice(cursor));
                cursor = hub.next_index();
            } else {
                chunk = realtime::sse_comment("ping");
            }
            if (!co_await write_all_ok(conn, bytes_const_view{chunk.data(), chunk.size()})) {
                co_return;
            }
        }
    }

    // WebSocket：101 握手 → 帧循环（文本回显 + 广播给其他 WS 客户端；ping/pong；close）。
    Task<void> serve_ws(async_file &conn, std::string const &accept,
                        std::string pending) {
        ++metrics().ws_total;
        auto head = realtime::ws_101_response(accept);
        if (!co_await write_all_ok(conn, bytes_const_view{head.data(), head.size()})) {
            co_return;
        }
        std::string buffer = std::move(pending);
        bytes_buffer in{8192};
        std::deque<std::pair<std::string, bool>> out; // {frame, close_after}
        stop_source read_stop;
        bool reading = false;

        // 广播注册：其他连接发来文本时入队，并取消当前读让本协程先写出去。
        auto sub = ws_clients().add(
            [&out, &read_stop, &reading](std::string text) {
                out.emplace_back(realtime::ws_encode_frame(realtime::ws_text, text),
                                 false);
                if (reading) {
                    read_stop.request_stop();
                }
            });

        for (;;) {
            while (!out.empty()) {
                auto item = std::move(out.front());
                out.pop_front();
                if (!co_await write_all_ok(
                        conn, bytes_const_view{item.first.data(), item.first.size()})) {
                    co_return;
                }
                if (item.second) {
                    co_return;
                }
            }

            reading = true;
            read_stop = stop_source::make();
            auto ret = co_await coio::read{conn, in, read_stop};
            reading = false;
            if (ret.error()) {
                if (ret.error() == -ECANCELED) {
                    continue; // 被广播唤醒：回到循环顶部 flush 队列
                }
                co_return;
            }
            if (ret.value() == 0) {
                co_return;
            }
            auto view = in.subspan(0, ret.value());
            buffer.append(view.data(), view.size());

            for (;;) {
                realtime::ws_frame frame;
                int r = realtime::ws_parse_frame(buffer, frame);
                if (r == 0) {
                    break;
                }
                if (r < 0) {
                    co_return;
                }
                if (frame.opcode == realtime::ws_text) {
                    chat::Message msg;
                    std::error_code ec;
                    if (reflect::json_decode(std::string_view(frame.payload), msg, ec)) {
                        if (!msg.user.empty() && !msg.content.empty()) {
                            chat::get_hub().publish(std::move(msg));
                        }
                    }
                    // 广播给其他 WS 客户端 + 回显自己。
                    ws_clients().broadcast(frame.payload, sub.get());
                    out.emplace_back(
                        realtime::ws_encode_frame(realtime::ws_text, frame.payload),
                        false);
                } else if (frame.opcode == realtime::ws_ping) {
                    out.emplace_back(
                        realtime::ws_encode_frame(realtime::ws_pong, frame.payload),
                        false);
                } else if (frame.opcode == realtime::ws_close) {
                    out.emplace_back(
                        realtime::ws_encode_frame(realtime::ws_close, ""), true);
                }
            }
        }
    }

    Task<void> send_error(async_file &conn, int status, bool head) {
        co_response res = make_error(status);
        res.head = head;
        res.keep_alive = false;
        co_await write_response(conn, res);
    }
};
