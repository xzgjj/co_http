// HTTP 服务端：每连接一个 handler + 读/处理/写状态循环 + 路由分发”的模型
// 每个连接一个 http_connection_handler；响应写完回到读状态，实现 keep-alive。

/*
监听 TCP 端口，接受连接。
每个连接由一个 `http_connection_handler` 独立管理。
使用异步读写 + 回调续体驱动连接状态机。
增量解析 HTTP 请求，支持 TCP 半包/粘包场景下的“继续读”。
将完整请求交给路由表 `http_router` 精确匹配处理。
响应写完后回到读状态，实现 **HTTP keep-alive**
*/

#pragma once

#include <map>
#include <memory>
#include <string>
#include "expected.hpp"
#include "io_context.hpp"
#include "stop_source.hpp"
#include "http_codec.hpp"

//回调在函数返回之后才执行::借用，不该参与所有权。不用引用而用指针;do_write 的回调都是异步续体; 连接对象必须堆分配 + shared_ptr 管理；
//持有监听 socket,保存 accept 得到的对端地址,持有全局路由表,解析地址、绑定监听、进入 accept 循环
struct http_server : std::enable_shared_from_this<http_server> {
    using pointer = std::shared_ptr<http_server>;

    // 创建服务端对象，由 shared_ptr 管理生命周期。
    static pointer make() {
        return std::make_shared<pointer::element_type>();
    }

    struct http_request { //表示一次完整请求:请求行  请求头  请求体
        std::string url;
        http_method method; // GET, POST, PUT, ...
        std::string body;

        http_response_writer<> *m_res_writer = nullptr; // 指向所属连接的写缓冲
        callback<> m_resume; // 写响应后继续连接的续体

        // 写响应并恢复连接：状态行、固定头、正文依次写入。调用 m_resume()，触发连接的 do_write
        void write_response(
            int status, std::string_view content,
            std::string_view content_type = "text/plain;charset=utf-8") {
            m_res_writer->begin_header(status);
            m_res_writer->write_header("Server", "co_http");
            m_res_writer->write_header("Content-type", content_type);
            m_res_writer->write_header("Connection", "keep-alive");
            m_res_writer->write_header("Content-length",
                                       std::to_string(content.size()));
            m_res_writer->end_header();
            m_res_writer->write_body(content);
            m_resume(); // 交给 do_write，写完继续读下一个请求
        }
    };

    //它是整个服务端里唯一一张"URL 字符串 → 业务处理函数"的对照表，也就是分发点
    //注册：启动时把路径和对应处理函数存进表里；分发：每收到一个完整请求，按 request.url 查出处理函数并调用；查不到就统一回 404。
    struct http_router {//路由表：路由器
        std::map<std::string, callback<http_request &>> m_routes;// key 是路径字符串，精确匹配

        // 注册路径处理函数；同一路径重复注册会覆盖旧值。
        void route(std::string url, callback<http_request &> cb) {     
            m_routes.insert_or_assign(url, std::move(cb));//同一路径重复注册 = 覆盖，  为指定路径设置回调函数
        }

        // 查找并执行路由；找到：调用路由回调 找不到路径时返回 404。
        void do_handle(http_request &request) {
            // 寻找匹配的路径
            auto it = m_routes.find(request.url);//只按 URL 查表。method没匹配——GET和POST打到同一个路径
            if (it != m_routes.end()) {
                return it->second(multishot_call, request); // 路由回调可复用
            }
            // fmt::println("找不到路径: {}", request.url);
            return request.write_response(404, "404 Not Found");//单线程前提
        }
    };

    struct http_connection_handler
        : std::enable_shared_from_this<http_connection_handler> {
        async_file m_conn; // 当前连接
        bytes_buffer m_readbuf{1024}; // 固定 1KB 读缓冲
        http_request_parser<> m_req_parser; // 请求解析器
        http_response_writer<> m_res_writer; // 响应写缓冲
        http_router *m_router = nullptr; // 指向服务端的路由表
        http_request m_request; // 当前请求，长轮询时会延迟使用

        using pointer = std::shared_ptr<http_connection_handler>;

        // 创建连接处理器，由 shared_ptr 管理生命周期。
        static pointer make() {
            return std::make_shared<pointer::element_type>();
        }

        // 接管已接受的 fd，开始读请求。
        void do_start(http_router *router, int connfd) {
            m_router = router;
            m_conn = async_file{connfd};
            return do_read();
        }

        // 读取并解析一个完整请求，然后交给路由。;多数复用连接 新建时是新的;stop_soure定时器
        void do_read() {
            // 注意：TCP 基于流，可能粘包 fmt::println("开始读取...");
            // 设置一个 10 秒的定时器，若 10秒内没有读到任何请求，则视为对方放弃，关闭连接
            stop_source stop_io(std::in_place);
            stop_source stop_timer(std::in_place); 
            io_context::get().set_timeout(
                std::chrono::seconds(10),  //当前动态的异步定时器(新注册)
                [stop_io] {
                    stop_io.request_stop(); //定时器(新注册)先完成时，取消读取;看起来参数实际返回值
                },
                stop_timer);
            // 开始读取
            return m_conn.async_read(
                m_readbuf,
                [self = shared_from_this(),//回调不需要锁 没有内存序问多线程；只有一个线程
                 stop_timer](expected<size_t> ret) { //按值 拷贝
                    stop_timer.request_stop(); // 读取先完成时，取消定时器
                    if (ret.error()) {
                        // fmt::println("读取出错 {}，放弃连接", strerror(-ret.error()));
                        return;
                    }
                    size_t n = ret.value();
                    // 如果读到 EOF，说明对面，关闭了连接
                    if (n == 0) {
                        // fmt::println("对面关闭了连接");
                        return;
                    }
                    // fmt::println("读取到了 {} 个字节: {}", n, std::string_view{self->m_readbuf.data(), n});
                    // 成功读取，则推入解析
                    self->m_req_parser.push_chunk(
                        self->m_readbuf.subspan(0, n));//bytes_buffer：自写span 
                    if (!self->m_req_parser.request_finished()) {
                        return self->do_read(); // 半包，继续读
                    } else {
                        return self->do_handle();
                    }
                },
                stop_io);
        }

        // 把解析结果搬进 http_request，设置续体后交给路由。
        void do_handle() {
            m_request.url = m_req_parser.url();
            m_request.method = m_req_parser.method();
            m_request.body = std::move(m_req_parser.body());
            m_request.m_res_writer = &m_res_writer;
            m_request.m_resume = [self = shared_from_this()] {
                self->do_write(self->m_res_writer.buffer()); // 自引用保证写完前不析构
            };
            m_req_parser.reset_state(); // 为下一个请求清空状态

            // fmt::println("我的响应头: {}", buffer);
            // fmt::println("我的响应正文: {}", body);
            // fmt::println("正在响应");
            m_router->do_handle(m_request);
        }

        // 写出响应；没有写完时递归写剩余部分，写完回到读状态。
        void do_write(bytes_const_view buffer) {
            return m_conn.async_write(buffer, [self = shared_from_this(),
                                               buffer](expected<size_t> ret) {
                if (ret.error()) {
                    // fmt::println("写入错误，放弃连接");
                    return;
                }
                auto n = ret.value();

                if (buffer.size() == n) {
                    self->m_res_writer.reset_state(); // 响应已完整写出
                    return self->do_read(); // keep-alive：继续处理下一个请求
                }
                return self->do_write(buffer.subspan(n)); // 只写出一部分
            });
        }
    };

    async_file m_listening; // 监听 socket
    address_resolver::address m_addr; // accept 时写入对端地址
    http_router m_router;

    // 返回路由表，供外部注册路径。
    http_router &get_router() {
        return m_router;
    }

    // 解析监听地址、创建监听 socket，然后进入 accept 循环。
    void do_start(std::string name, std::string port) {
        address_resolver resolver;
        auto entry = resolver.resolve(name, port);
        m_listening = async_file::async_bind(entry);
        return do_accept();
    }

    // 接受一个连接并为它创建处理器，然后继续 accept。
    void do_accept() {
        return m_listening.async_accept(m_addr, [self = shared_from_this()](
                                                    expected<int> ret) {
            auto connfd = ret.expect("accept"); // accept 失败直接抛异常

            // fmt::println("接受了一个连接: {}", connfd);
            http_connection_handler::make()->do_start(&self->m_router, connfd);
            return self->do_accept(); // 继续等下一个连接
        });
    }
};
