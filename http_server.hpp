// HTTP 服务端（P1 协议正确性版）：
//   - method + path 路由（HEAD 复用 GET 处理器；OPTIONS/405 带 Allow）；
//   - URL 拆 path/query；请求行/版本解析；
//   - HEAD 不写正文（Content-Length 仍为真实长度）；
//   - Date / Connection / 标准 reason phrase；HTTP/1.0 默认 close；
//   - 流水线：保留 Content-Length 之后的剩余字节，按序处理；
//   - Content-Length 严格化（缺失 411 / 非法 400 / 重复 400）、chunked → 501；
//   - 头部上限 431、URL 上限 414、正文上限 413、连接数/超时（P0 延续）。
//
// 与 P0 相同的连接状态机骨架：读 → 校验 → 路由 → 写 → 回到读（keep-alive）。
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <netinet/tcp.h>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "expected.hpp"
#include "io_context.hpp"
#include "stop_source.hpp"
#include "http_codec.hpp"
#include "http_delivery.hpp"
#include "metrics.hpp"
#include "chat_business.hpp"
#include "realtime.hpp"
#include "ws_hub.hpp"

using namespace std::chrono_literals;

struct http_server : std::enable_shared_from_this<http_server> {
    using pointer = std::shared_ptr<http_server>;

    struct config {
        size_t max_header = 16 * 1024;
        size_t max_url = 8 * 1024;
        size_t max_body = 1 * 1024 * 1024;
        size_t max_conn = 1024;
        int request_timeout_ms = 10000;
        int write_timeout_ms = 10000;
        int poll_timeout_ms = 3000;
        int sse_heartbeat_ms = 15000; // SSE 心跳/注释帧间隔
    };

    static pointer make() {
        return std::make_shared<pointer::element_type>();
    }

    struct http_connection_handler;

    struct http_request {
        http_method method = http_method::UNKNOWN;
        std::string path;
        std::string query;
        std::string version;
        std::string body;
        bool keep_alive = true;
        bool head = false;
        bool m_tls = false; // 当前连接是否 TLS（TLS 下不用 sendfile）

        http_response_writer<> *m_res_writer = nullptr;
        callback<> m_resume;
        std::weak_ptr<http_connection_handler> m_holder;
        std::vector<std::pair<std::string, std::string>> m_extra_headers;
        string_map headers; // 请求头（业务读取 If-None-Match / Accept-Encoding 等）
        int m_status = 0;    // 访问日志/指标用
        size_t m_bytes = 0;
        std::string m_body_out; // 响应正文（与头部拆分，写时用 writev）
        int m_file_fd = -1;     // sendfile 源文件 fd
        size_t m_file_size = 0;

        // 实时推送：SSE / WebSocket 会把连接从"请求-响应"切成流式模式。
        enum class stream_kind { none, sse, websocket };
        stream_kind m_stream = stream_kind::none;
        std::string m_ws_accept;
        size_t m_sse_cursor = 0;

        void begin_sse(size_t cursor) {
            m_stream = stream_kind::sse;
            m_sse_cursor = cursor;
        }
        void begin_websocket(std::string accept) {
            m_stream = stream_kind::websocket;
            m_ws_accept = std::move(accept);
        }

        void set_header(std::string key, std::string value) {
            m_extra_headers.emplace_back(std::move(key), std::move(value));
        }

        void write_response(
            int status, std::string_view content,
            std::string_view content_type = "text/plain;charset=utf-8") {
            auto *w = m_res_writer;
            w->begin_header(status);
            w->write_header("Server", "co_http");
            w->write_header("X-Pid", std::to_string(getpid()));
            w->write_header("Date", http_date_string());
            w->write_header("Content-type", content_type);
            w->write_header("Connection", keep_alive ? "keep-alive" : "close");
            for (auto const &[key, value] : m_extra_headers) {
                w->write_header(key, value);
            }
            w->write_header("Content-length", std::to_string(content.size()));
            w->end_header();
            m_body_out = head ? std::string() : std::string(content);
            m_status = status;
            m_bytes = w->buffer().size() + m_body_out.size();
            m_resume();
        }

        // 静态文件：值只写头部，正文交给 sendfile（零拷贝）。
        void write_file_response(int status, int fd, size_t size,
                                 std::string_view content_type) {
            auto *w = m_res_writer;
            w->begin_header(status);
            w->write_header("Server", "co_http");
            w->write_header("X-Pid", std::to_string(getpid()));
            w->write_header("Date", http_date_string());
            w->write_header("Content-type", content_type);
            w->write_header("Connection", keep_alive ? "keep-alive" : "close");
            for (auto const &[key, value] : m_extra_headers) {
                w->write_header(key, value);
            }
            w->write_header("Content-length", std::to_string(size));
            w->end_header();
            m_file_fd = fd;
            m_file_size = size;
            m_status = status;
            m_bytes = w->buffer().size() + (head ? 0 : size);
            m_resume();
        }
    };

    // 路由表：path -> method -> handler；HEAD 回退到 GET；兼容"任意方法"注册。
    struct http_router {
        static constexpr int kAnyMethod = -1;
        using handler = callback<http_request &>;
        std::map<std::string, std::map<int, handler>> m_routes;

        void route(http_method method, std::string path, handler cb) {
            m_routes[path][static_cast<int>(method)] = std::move(cb);
        }

        // 兼容旧示例：任意方法都交给同一个处理器。
        void route_any(std::string path, handler cb) {
            m_routes[path][kAnyMethod] = std::move(cb);
        }

        // 兼容旧接口：等价于 route_any。
        void route(std::string path, handler cb) {
            route_any(std::move(path), std::move(cb));
        }

        std::string allow_list(std::map<int, handler> const &by_method) const {
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

        void do_handle(http_request &request) {
            auto path_it = m_routes.find(request.path);
            if (path_it == m_routes.end()) {
                return request.write_response(404, "404 Not Found");
            }
            auto &by_method = path_it->second;
            if (!by_method.empty()) {
                request.set_header("Allow", allow_list(by_method));
            }

            if (request.method == http_method::OPTIONS) {
                return request.write_response(200, "");
            }

            int lookup = static_cast<int>(request.method);
            if (request.method == http_method::HEAD) {
                lookup = static_cast<int>(http_method::GET); // HEAD 复用 GET
            }
            auto it = by_method.find(lookup);
            if (it == by_method.end()) {
                it = by_method.find(kAnyMethod);
            }
            if (it == by_method.end()) {
                return request.write_response(405, "405 Method Not Allowed");
            }
            return it->second(multishot_call, request);
        }
    };

    struct http_connection_handler
        : std::enable_shared_from_this<http_connection_handler> {
        async_file m_conn;
        bytes_buffer m_readbuf{8192};
        http_request_parser<> m_req_parser;
        http_response_writer<> m_res_writer;
        http_router *m_router = nullptr;
        http_server *m_server = nullptr;
        std::shared_ptr<size_t> m_active;
        http_request m_request;

        stop_source m_request_deadline;
        stop_source m_write_deadline;
        std::chrono::steady_clock::time_point m_request_start{};

        std::string m_pending; // 流水线剩余字节
        bool m_dead = false;
        bool m_keep_alive = true;

        // 流式（SSE / WebSocket）状态
        bool m_streaming = false;
        std::string m_stream_out; // 当前流式写缓冲（必须活到写出完成）
        std::string m_ws_buf;     // WebSocket 接收帧缓冲
        size_t m_sse_cursor = 0;
        std::deque<std::pair<std::string, bool>> m_ws_out; // {frame, close_after}
        std::shared_ptr<ws_subscriber> m_ws_sub;
        stop_source m_ws_read_stop;
        bool m_ws_reading = false;
        bool m_ws_writing = false;

        using pointer = std::shared_ptr<http_connection_handler>;

        static pointer make() {
            return std::make_shared<pointer::element_type>();
        }

        void do_start(http_server *server, int connfd) {
            m_server = server;
            m_router = &server->get_router();
            m_active = server->m_active;
            ++*m_active;
            ++metrics().active_connections;
            ++metrics().connections_total;
            int on = 1;
            setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            m_conn = async_file{connfd};
            if (tls().enabled()) {
                m_conn.adopt_tls(tls().new_ssl(connfd));
                m_conn.async_tls_handshake([self = shared_from_this()](
                    expected<int> ret) {
                    if (ret.error()) {
                        return self->close_conn();
                    }
                    self->start_request();
                });
                return;
            }
            start_request();
        }

        ~http_connection_handler() {
            if (m_active && *m_active > 0) {
                --*m_active;
            }
            auto &m = metrics();
            if (m.active_connections.load() > 0) {
                --m.active_connections;
            }
        }

        [[nodiscard]] bool is_closed() const {
            return m_dead;
        }

        void start_request() {
            if (m_dead) {
                return;
            }
            if (m_request_deadline) {
                m_request_deadline.request_stop();
                m_request_deadline = {};
            }
            m_request = http_request{};
            m_req_parser.reset_state();
            m_res_writer.reset_state();
            m_request.m_holder = shared_from_this();
            m_request.m_tls = m_conn.tls_enabled();
            m_request.m_res_writer = &m_res_writer;
            m_request.m_resume = [self = shared_from_this()] {
                self->write_out_response();
            };

            auto &conf = m_server->m_conf;
            m_request_start = std::chrono::steady_clock::now();
            m_request_deadline = stop_source::make();
            io_context::get().set_timeout(
                std::chrono::milliseconds(conf.request_timeout_ms),
                [self = shared_from_this()] {
                    ++metrics().timeouts;
                    self->close_conn();
                },
                m_request_deadline);

            if (!m_pending.empty()) {
                std::string buf = std::move(m_pending);
                m_pending.clear();
                feed(bytes_const_view{buf.data(), buf.size()});
                // 注意：buf 在 feed 内同步用完（解析器/请求体会拷贝），可以安全释放
                return;
            }
            do_read();
        }

        // 响应写完（已被投递到事件循环）：继续流水线/下一个请求，或关闭连接。
        void finish_response() {
            if (m_dead) {
                return;
            }
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - m_request_start)
                                  .count();
            auto &m = metrics();
            ++m.requests;
            m.bytes_sent.fetch_add(m_request.m_bytes);
            std::fprintf(stderr, "[access] %s %s %d %zuB %lldms\n",
                         std::string(http_method_name(m_request.method)).c_str(),
                         m_request.path.c_str(), m_request.m_status,
                         m_request.m_bytes, static_cast<long long>(elapsed_ms));
            if (!m_keep_alive) {
                return close_conn();
            }
            start_request();
        }

        void do_read() {
            if (m_dead) {
                return;
            }
            m_conn.async_read(
                m_readbuf,
                [self = shared_from_this()](expected<size_t> ret) {
                    if (ret.error()) {
                        return self->close_conn();
                    }
                    size_t n = ret.value();
                    if (n == 0) {
                        return self->close_conn();
                    }
                    self->feed(self->m_readbuf.subspan(0, n));
                });
        }

        // 喂入一段字节；可能完成一个请求（可能带流水线剩余），也可能继续读。
        void feed(bytes_const_view chunk) {
            if (m_dead) {
                return;
            }
            auto &conf = m_server->m_conf;
            m_req_parser.push_chunk(chunk);

            // 头部上限：无论本次是否刚好读完头部，都按累计长度判断。
            if (m_req_parser.headers_raw().size() > conf.max_header) {
                return fail(431, "431 Request Header Fields Too Large");
            }
            if (!m_req_parser.header_finished()) {
                return do_read();
            }

            // 头部完整：先做语法/语义校验。
            if (m_req_parser.url().size() > conf.max_url) {
                return fail(414, "414 URI Too Long");
            }
            if (m_req_parser.bad_header_line()) {
                return fail(400, "400 Bad Request");
            }
            if (m_req_parser.chunked()) {
                return fail(501, "501 Not Implemented");
            }
            if (m_req_parser.duplicate_content_length()) {
                return fail(400, "400 Bad Request");
            }
            if (!m_req_parser.cl_valid()) {
                return fail(400, "400 Bad Request");
            }

            auto method = m_req_parser.method();
            auto version = m_req_parser.version();
            if (method == http_method::UNKNOWN) {
                return fail(501, "501 Not Implemented");
            }
            if (version != "HTTP/1.1" && version != "HTTP/1.0") {
                return fail(505, "505 HTTP Version Not Supported");
            }
            auto &headers = m_req_parser.headers();
            if (version == "HTTP/1.1" && headers.find("host") == headers.end()) {
                return fail(400, "400 Bad Request");
            }

            bool expects_body = method == http_method::POST ||
                                method == http_method::PUT ||
                                method == http_method::PATCH;
            if (expects_body && !m_req_parser.cl_present()) {
                return fail(411, "411 Length Required");
            }
            if (m_req_parser.content_length() > conf.max_body ||
                m_req_parser.body().size() > conf.max_body) {
                return fail(413, "413 Payload Too Large");
            }

            if (!m_req_parser.request_finished()) {
                return do_read();
            }

            m_request_deadline.request_stop();
            m_request_deadline = {};

            std::string leftover = m_req_parser.take_leftover();
            do_handle(std::move(leftover));
        }

        void do_handle(std::string leftover) {
            m_request.method = m_req_parser.method();
            m_request.path = m_req_parser.path();
            m_request.query = m_req_parser.query();
            m_request.version = m_req_parser.version();
            m_request.headers = m_req_parser.headers();
            m_request.body = std::move(m_req_parser.body());
            m_request.head = (m_request.method == http_method::HEAD);
            m_keep_alive = compute_keep_alive(m_request.version, m_req_parser.headers());
            m_request.keep_alive = m_keep_alive;
            m_pending = std::move(leftover);
            m_req_parser.reset_state();

            try {
                m_router->do_handle(m_request);
            } catch (std::exception const &e) {
                std::fprintf(stderr, "[http] route exception: %s\n", e.what());
                return fail(500, "500 Internal Server Error");
            } catch (...) {
                std::fprintf(stderr, "[http] route exception: unknown\n");
                return fail(500, "500 Internal Server Error");
            }
            if (m_request.m_stream != http_request::stream_kind::none) {
                return start_stream();
            }
        }

        // ---------------- 流式推送（SSE / WebSocket） ----------------

        void start_stream() {
            if (m_dead) {
                return;
            }
            if (m_request.m_stream == http_request::stream_kind::sse) {
                return start_sse();
            }
            if (m_request.m_stream == http_request::stream_kind::websocket) {
                return start_websocket();
            }
            close_conn();
        }

        // 流式写：把待写数据留在成员里（异步写出期间保持有效）。
        void stream_write(std::string data, callback<> done) {
            m_stream_out = std::move(data);
            do_write(bytes_const_view{m_stream_out.data(), m_stream_out.size()},
                     std::move(done));
        }

        void start_sse() {
            m_streaming = true;
            ++metrics().sse_total;
            m_sse_cursor = m_request.m_sse_cursor;
            stream_write(realtime::sse_response_head(),
                         [self = shared_from_this()] { self->sse_replay(); });
        }

        // 连接建立后先重放历史（≤1000 条），再进入"新消息 + 心跳"订阅循环。
        void sse_replay() {
            if (m_dead) {
                return;
            }
            auto &hub = chat::get_hub();
            if (m_sse_cursor < hub.next_index()) {
                std::string chunk = realtime::sse_event(chat::recv_slice(m_sse_cursor));
                m_sse_cursor = hub.next_index();
                stream_write(std::move(chunk),
                             [self = shared_from_this()] { self->sse_subscribe(); });
                return;
            }
            sse_subscribe();
        }

        // 每次订阅只等待一次 publish 或心跳；被唤醒后写数据，再订阅下一轮。
        void sse_subscribe() {
            if (m_dead) {
                return;
            }
            std::weak_ptr<http_connection_handler> holder = shared_from_this();
            auto state = chat::subscribe_long_poll([holder] {
                auto conn = holder.lock();
                if (!conn || conn->m_dead) {
                    return;
                }
                conn->sse_pump();
            });
            state->timer_stop = stop_source::make();
            io_context::get().set_timeout(
                std::chrono::milliseconds(m_server->m_conf.sse_heartbeat_ms),
                [state] {
                    if (state->done) {
                        return;
                    }
                    state->done = true;
                    io_context::get().post([state] { state->respond(); });
                },
                state->timer_stop);
        }

        void sse_pump() {
            if (m_dead) {
                return;
            }
            auto &hub = chat::get_hub();
            std::string chunk;
            if (m_sse_cursor < hub.next_index()) {
                chunk = realtime::sse_event(chat::recv_slice(m_sse_cursor));
                m_sse_cursor = hub.next_index();
            } else {
                chunk = realtime::sse_comment("ping");
            }
            stream_write(std::move(chunk),
                         [self = shared_from_this()] { self->sse_subscribe(); });
        }

        void start_websocket() {
            m_streaming = true;
            ++metrics().ws_total;
            m_ws_sub = ws_clients().add([self = shared_from_this()](std::string text) {
                self->ws_enqueue(realtime::ws_text, std::move(text), false);
            });
            stream_write(realtime::ws_101_response(m_request.m_ws_accept),
                         [self = shared_from_this()] { self->ws_read(); });
        }

        // 把一帧放进发送队列；若当前正阻塞在 read，则取消 read 让它先写出去。
        void ws_enqueue(int opcode, std::string payload, bool close_after) {
            if (m_dead) {
                return;
            }
            m_ws_out.emplace_back(realtime::ws_encode_frame(opcode, payload),
                                  close_after);
            if (m_ws_writing) {
                return;
            }
            if (m_ws_reading) {
                m_ws_read_stop.request_stop(); // 唤醒读循环去 flush
                return;
            }
            ws_flush();
        }

        void ws_flush() {
            if (m_dead || m_ws_writing) {
                return;
            }
            if (m_ws_out.empty()) {
                ws_read();
                return;
            }
            m_ws_writing = true;
            auto item = std::move(m_ws_out.front());
            m_ws_out.pop_front();
            stream_write(std::move(item.first),
                         [self = shared_from_this(), close_after = item.second] {
                             self->m_ws_writing = false;
                             if (close_after) {
                                 return self->close_conn();
                             }
                             self->ws_flush();
                         });
        }

        void ws_read() {
            if (m_dead) {
                return;
            }
            m_ws_reading = true;
            m_ws_read_stop = stop_source::make();
            m_conn.async_read(
                m_readbuf,
                [self = shared_from_this()](expected<size_t> ret) {
                    self->m_ws_reading = false;
                    const bool canceled = self->m_ws_read_stop.stop_requested();
                    if (ret.error()) {
                        if (canceled) {
                            return self->ws_flush(); // 被广播唤醒：先发队列
                        }
                        return self->close_conn();
                    }
                    size_t n = ret.value();
                    if (n == 0) {
                        return self->close_conn();
                    }
                    auto view = self->m_readbuf.subspan(0, n);
                    self->m_ws_buf.append(view.data(), view.size());
                    self->ws_handle_frames();
                },
                m_ws_read_stop);
        }

        void ws_handle_frames() {
            if (m_dead) {
                return;
            }
            for (;;) {
                realtime::ws_frame frame;
                int r = realtime::ws_parse_frame(m_ws_buf, frame);
                if (r == 0) {
                    break;
                }
                if (r < 0) {
                    return close_conn();
                }
                if (frame.opcode == realtime::ws_text) {
                    // 聊天消息：发布到共享 hub（SSE/长轮询同样可见），
                    // 回显给自己，并广播给其他 WS 客户端。
                    chat::Message msg;
                    std::error_code ec;
                    if (reflect::json_decode(std::string_view(frame.payload), msg, ec)) {
                        if (!msg.user.empty() && !msg.content.empty()) {
                            chat::get_hub().publish(std::move(msg));
                        }
                    }
                    ws_clients().broadcast(frame.payload, m_ws_sub.get());
                    return ws_enqueue(realtime::ws_text, std::move(frame.payload), false);
                }
                if (frame.opcode == realtime::ws_ping) {
                    return ws_enqueue(realtime::ws_pong, std::move(frame.payload), false);
                }
                if (frame.opcode == realtime::ws_close) {
                    return ws_enqueue(realtime::ws_close, "", true);
                }
            }
            ws_read();
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

        static bool compute_keep_alive(std::string const &version,
                                       string_map const &headers) {
            if (header_has_token(headers, "connection", "close")) {
                return false;
            }
            if (version == "HTTP/1.0") {
                return header_has_token(headers, "connection", "keep-alive");
            }
            return true;
        }

        // ---------------- 响应写出（普通 / writev / sendfile） ----------------

        void arm_write_deadline() {
            if (m_write_deadline) {
                m_write_deadline.request_stop();
                m_write_deadline = {};
            }
            m_write_deadline = stop_source::make();
            io_context::get().set_timeout(
                std::chrono::milliseconds(m_server->m_conf.write_timeout_ms),
                [self = shared_from_this()] { self->close_conn(); },
                m_write_deadline);
        }

        void clear_write_deadline() {
            if (m_write_deadline) {
                m_write_deadline.request_stop();
                m_write_deadline = {};
            }
        }

        // 路由写响应后统一入口：根据是静态文件/普通响应选择 sendfile/writev/write。
        void write_out_response() {
            if (m_dead) {
                return;
            }
            auto finish = [self = shared_from_this()] {
                // 写完响应后延迟到事件循环再开始下一个请求（m_resume 可能就在调用栈上）。
                io_context::get().post([self] { self->finish_response(); });
            };
            if (m_request.m_file_fd >= 0 && !m_request.head && !m_request.m_tls) {
                ++metrics().sendfile_calls;
                int fd = m_request.m_file_fd;
                size_t size = m_request.m_file_size;
                do_write(m_res_writer.buffer(), [self = shared_from_this(), fd, size,
                                                 finish = std::move(finish)]() mutable {
                    self->do_sendfile(fd, size, 0, std::move(finish));
                });
                return;
            }
            if (!m_request.m_body_out.empty() && !m_request.head) {
                ++metrics().writev_calls;
                do_writev(m_res_writer.buffer(),
                          bytes_const_view{m_request.m_body_out.data(),
                                           m_request.m_body_out.size()},
                          std::move(finish));
                return;
            }
            do_write(m_res_writer.buffer(), std::move(finish));
        }

        void do_write(bytes_const_view buffer, callback<> on_done) {
            if (m_dead) {
                return;
            }
            arm_write_deadline();
            m_conn.async_write(
                buffer,
                [self = shared_from_this(), buffer,
                 on_done = std::move(on_done)](expected<size_t> ret) mutable {
                    if (ret.error()) {
                        return self->close_conn();
                    }
                    size_t n = ret.value();
                    if (n >= buffer.size()) {
                        self->clear_write_deadline();
                        on_done();
                        return;
                    }
                    self->do_write(buffer.subspan(n), std::move(on_done));
                });
        }

        // 头 + 正文一次 writev 发出；短写时继续写剩余部分。
        void do_writev(bytes_const_view header, bytes_const_view body,
                       callback<> on_done) {
            if (m_dead) {
                return;
            }
            arm_write_deadline();
            m_conn.async_writev(
                header, body,
                [self = shared_from_this(), header, body,
                 on_done = std::move(on_done)](expected<size_t> ret) mutable {
                    if (ret.error()) {
                        return self->close_conn();
                    }
                    size_t n = ret.value();
                    size_t total = header.size() + body.size();
                    if (n >= total) {
                        self->clear_write_deadline();
                        on_done();
                        return;
                    }
                    if (n >= header.size()) {
                        self->do_writev(bytes_const_view{}, body.subspan(n - header.size()),
                                        std::move(on_done));
                    } else {
                        self->do_writev(header.subspan(n), body, std::move(on_done));
                    }
                });
        }

        // sendfile 循环直到整个文件送达。
        void do_sendfile(int in_fd, size_t total, size_t offset, callback<> on_done) {
            if (m_dead) {
                return;
            }
            if (offset >= total) {
                clear_write_deadline();
                on_done();
                return;
            }
            arm_write_deadline();
            m_conn.async_sendfile(
                in_fd, static_cast<off_t>(offset), total - offset,
                [self = shared_from_this(), in_fd, total, offset,
                 on_done = std::move(on_done)](expected<size_t> ret) mutable {
                    if (ret.error()) {
                        return self->close_conn();
                    }
                    size_t n = ret.value();
                    if (n == 0) {
                        return self->close_conn();
                    }
                    size_t next = offset + n;
                    if (next >= total) {
                        self->clear_write_deadline();
                        on_done();
                        return;
                    }
                    self->do_sendfile(in_fd, total, next, std::move(on_done));
                });
        }

        // 错误响应：写完即关闭连接。
        void fail(int status, std::string_view message) {
            if (m_dead) {
                return;
            }
            m_dead = true;
            ++metrics().errors;
            std::fprintf(stderr, "[access] FAIL %s %d\n",
                         m_request.path.c_str(), status);
            if (m_request_deadline) {
                m_request_deadline.request_stop();
                m_request_deadline = {};
            }
            m_request.m_resume = nullptr;

            m_res_writer.reset_state();
            m_res_writer.begin_header(status);
            m_res_writer.write_header("Server", "co_http");
            m_res_writer.write_header("Date", http_date_string());
            m_res_writer.write_header("Content-type", "text/plain;charset=utf-8");
            m_res_writer.write_header("Connection", "close");
            m_res_writer.write_header("Content-length", std::to_string(message.size()));
            m_res_writer.end_header();
            m_res_writer.write_body(message);

            bytes_const_view buffer = m_res_writer.buffer();
            m_conn.async_write(buffer, [self = shared_from_this()](
                                           expected<size_t> ret) {
                (void)ret;
                self->m_conn.close_now();
            });
        }

        void close_conn() {
            if (m_dead) {
                return;
            }
            m_dead = true;
            if (m_request_deadline) {
                m_request_deadline.request_stop();
                m_request_deadline = {};
            }
            if (m_write_deadline) {
                m_write_deadline.request_stop();
                m_write_deadline = {};
            }
            m_request.m_resume = nullptr;
            m_request.m_res_writer = nullptr;
            m_ws_sub.reset(); // 从 WS 广播注册表移除
            m_ws_out.clear();
            m_conn.close_now();
        }
    };

    config m_conf;
    async_file m_listening;
    address_resolver::address m_addr;
    http_router m_router;
    std::shared_ptr<size_t> m_active = std::make_shared<size_t>(0);
    stop_source m_accept_backoff;

    http_router &get_router() {
        return m_router;
    }
    config &get_config() {
        return m_conf;
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
                        std::fprintf(stderr, "[http] accept: %s, retry in 100ms\n",
                                     strerror(-err));
                        self->m_accept_backoff = stop_source::make();
                        io_context::get().set_timeout(
                            100ms, [self] { self->do_accept(); },
                            self->m_accept_backoff);
                        return;
                    }
                    std::fprintf(stderr, "[http] accept error: %s\n", strerror(-err));
                    return self->do_accept();
                }

                int connfd = ret.value();
                if (*self->m_active >= self->m_conf.max_conn) {
                    close(connfd);
                    return self->do_accept();
                }
                http_connection_handler::make()->do_start(self.get(), connfd);
                return self->do_accept();
            });
    }
};
