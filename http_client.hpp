// 极简 HTTP/1.1 客户端：URL 解析、连接池复用、请求序列化、响应解析。
// 使用方必须先包含 io_context.hpp，本文件只使用 async_file/address_resolver 而不引入它们。

/*
解析 URL，拆出 scheme / hostname（含端口）/ path。
按 scheme + hostname维护连接池，同 key 复用连接（客户端侧 keep-alive）。
把`http_request 序列化成 HTTP 请求报文。

异步 connect / write / read。
增量解析响应，`Content-Length` 收满后组装 `http_response`。
通过 `callback<expected<int>, http_response const&>` 把结果交回调用方。
通过 `stop_source` 支持外部取消
*/
#pragma once

#include <map>
#include <memory>
#include <string>
#include "expected.hpp"
#include "stop_source.hpp"
#include "http_codec.hpp"

//客户端主体。URL 解析 + 连接池调度
struct http_client : std::enable_shared_from_this<http_client> {
    using pointer = std::shared_ptr<http_client>;

    // 创建客户端对象，由 shared_ptr 管理生命周期。
    static pointer make() {
        return std::make_shared<pointer::element_type>();//工厂模式:构造函数不暴露
    }

    struct http_request {//保存 m_cb / m_stop / m_request
        std::string method;
        std::string url;
        std::string body = {};
        string_map headers = {}; // 预留字段，当前序列化未使用
    };

    struct http_response { //请求与响应的纯数据载体
        int status;
        std::string body;
        string_map headers;
    };

    //URL 拆分器:把完整 URL 拆成 scheme / hostname(含端口) / path 三部分，缺省时补默认值
    struct _http_url_parser {
        std::string m_hostname;
        std::string m_scheme;
        std::string m_url;

        
        _http_url_parser() = default;// 默认构造空解析结果。

        // 解析 URL：拆出 scheme、hostname（含端口）和路径。
        _http_url_parser(std::string url) : m_url(std::move(url)) {
            // 解析 URL:先用 move 保存完整 URL
            auto pos = m_url.find("://");
            if (pos == std::string::npos) { //第一段按 "://";有 → scheme = 前段，m_url = 后段 ;无 → scheme = "http"，m_url 不变
                pos = 0;
                m_scheme = "http"; // 没有 scheme 时默认 http
            } else {
                m_scheme = m_url.substr(0, pos);
                m_url = m_url.substr(pos + 3);
            }
            pos = m_url.find('/');
            if (pos == std::string::npos) { //有 → hostname = 前段，m_url = 从 / 开始的路径；无 → hostname = 全部
                m_hostname = m_url;
                m_url = "/"; // 没有路径时补根路径
            } else {
                m_hostname = m_url.substr(0, pos);
                m_url = m_url.substr(pos);
            }
        }
    };

    //do_request 是客户端对外入口。解析 URL得到连接池 key，命中则复用已有 handler，否则新建并登记；
    //然后把请求、回调、取消令牌一并委派给 handler
    struct http_connection_handler
        : std::enable_shared_from_this<http_connection_handler> {
        using pointer = std::shared_ptr<http_connection_handler>;

        // 创建连接处理器，由 shared_ptr 管理生命周期。
        static pointer make() {
            return std::make_shared<pointer::element_type>();
        }

        http_request m_request; // 当前请求
        http_request_writer<> m_req_writer; // 请求写缓冲
        http_response_parser<> m_res_parser; // 响应解析器
        async_file m_conn; // 与服务器之间的连接
        callback<expected<int>, http_response const &> m_cb; // 完成回调
        stop_source m_stop; // 取消令牌
        _http_url_parser m_parsed_url; // 当前 URL 的解析结果
        bytes_buffer m_readbuf{1024}; // 固定 1KB 读缓冲

        // 解析主机名和服务名；hostname 里可能带端口。
        address_resolver::address_info _resolve_address(address_resolver &res) {
            std::string service = m_parsed_url.m_scheme;
            std::string name = m_parsed_url.m_hostname;
            auto colon = name.rfind(':');
            if (colon != std::string::npos) {
                service = name.substr(colon + 1); //有 : 时端口优先 端口充当服务名
                name = name.substr(0, colon);
            }
            return res.resolve(name, service);
        }

        // 发起请求：保存本次请求的回调、取消令牌、请求体与解析结果；若连接已存在（池命中）直接序列化，否则解析地址、建 socket、异步连接
        void do_request(http_request request,  //先存 m_cb / m_stop / m_request，再判复用
                        _http_url_parser const &parsed_url,
                        callback<expected<int>, http_response const &> cb,
                        stop_source stop = {}) {  
            m_cb = std::move(cb);
            m_stop = stop;
            m_request = std::move(request);
            if (m_conn) {  //m_conn 存在 → 直接 do_compose()，不重新解析地址
                // fmt::println("复用现有连接");
                do_compose(); // 连接池命中，直接发请求
                return;
            }
            m_parsed_url = std::move(parsed_url);
            // fmt::println("连接到 {}，服务 {}，路径 {}", m_parsed_url.m_hostname, m_parsed_url.m_scheme, m_request.url);
            // "http://142857.red/" 变成
            // m_hostname = "142857.red";
            // m_scheme = "http";
            // m_request.url = "/";
            address_resolver res;
            auto addr = _resolve_address(res);
            m_conn = async_file{addr.create_socket()};
            // fmt::println("开始连接...");
            return m_conn.async_connect(
                addr,
                [self = shared_from_this()](expected<int> ret) mutable {
                    ret.expect("connect"); //onnect 回调捕获 self 保活 连接失败直接抛异常
                    // fmt::println("连接成功...");
                    self->do_compose();
                },
                m_stop);//stop 按值拷贝传入
        }

        // 序列化请求行、固定头部和可选正文，然后开始写。:
        //请求行：method + " " + url（url 已是路径）;固定头：Host、User-agent: co_http、Accept: */
        void do_compose() {
            m_req_writer.begin_header(m_request.method, m_request.url);
            m_req_writer.write_header("Host", m_parsed_url.m_hostname);
            m_req_writer.write_header("User-agent", "co_http");
            m_req_writer.write_header("Accept", "*/*");
            if (!m_request.body.empty()) {  //只有 body 非空才写 Content-length
                m_req_writer.write_header(
                    "Content-length", std::to_string(m_request.body.size()));
            }
            m_req_writer.end_header();
            if (!m_request.body.empty()) {
                m_req_writer.write_body(m_request.body);
            }
            http_response response;
            // fmt::println("正在写入请求...");
            return do_write(m_req_writer.buffer());
        }

        // 写请求；没有写完时递归写剩余部分，写完开始读响应。
        void do_write(bytes_const_view buffer) {
            return m_conn.async_write(
                buffer,//buffer 按值捕获：bytes_const_view 是轻量视图
                [self = shared_from_this(), buffer](expected<size_t> ret) { //部分写递归：buffer.subspan(n)
                    if (ret.error()) {
                        return self->m_cb(ret.error(), {}); // 写失败，回调报错
                    }
                    auto n = ret.value(); //全部写完 → m_req_writer.reset_state() → do_read()
                    // fmt::println("写入 {} 字节", n);

                    if (buffer.size() == n) {
                        // fmt::println("写入请求成功，开始读取");
                        self->m_req_writer.reset_state();
                        return self->do_read();
                    }
                    return self->do_write(buffer.subspan(n)); // 只写出一部分
                },
                m_stop);
        }

        // 异步读响应；按 Content-Length :循环异步读连接字节，喂给响应解析器；解析器未完成则继续读（处理半包），
        void do_read() {
            // 开始读取;正常数据 → push_chunk → 未完成继续读 → 完成 do_finish()
            return m_conn.async_read(
                m_readbuf,
                [self = this->shared_from_this()](expected<size_t> ret) {
                    if (ret.error()) {
                        // fmt::println("读取出错 {}，放弃连接", strerror(-ret.error()));
                        return self->m_cb(ret.error(), {}); // 读失败，回调报错
                    }
                    size_t n = ret.value();
                    // 如果读到 EOF，说明对面，关闭了连接
                    if (n == 0) {
                        // fmt::println("对面关闭了连接");
                        return; // 当前不回调，调用方可能一直等待
                    }
                    // fmt::println("读取到了 {} 个字节: {}", n, std::string_view{self->m_readbuf.data(), n});
                    // 成功读取，则推入解析
                    self->m_res_parser.push_chunk(
                        self->m_readbuf.subspan(0, n));
                    if (!self->m_res_parser.request_finished()) {//仅支持 Content-Length，不支持 chunked。
                        return self->do_read(); // 半包，继续读
                    } else {
                        return self->do_finish();
                    }
                },
                m_stop);
        }

        // 组装响应对象并触发完成回调。从解析器取出状态码、body、headers 组装成 http_response
        void do_finish() {
            if (m_stop.stop_requested()) {
                return m_cb(-ECANCELED, {}); // 显式再查一次取消已被取消 回调 -ECANCELED。
            }

            auto response = http_response{
                m_res_parser.status(),
                std::move(m_res_parser.body()),
                std::move(m_res_parser.headers()),
            };
            m_res_parser.reset_state();//reset_state() 为下一次响应准备（连接复用场景）
            return m_cb(0, response); //回调 (0, response) 表示成功。错误 (error_code, {})，取消 (-ECANCELED, {})。
        }
    };

    std::map<std::string, http_connection_handler::pointer> m_conn_pool; // 按 scheme+hostname 复用连接

    // 解析 URL、查找/创建连接，然后委派给连接处理器。
    //先保存 m_cb、m_stop、m_request，再判断是否复用连接——顺序正确，复用分支也能拿到回调
    void do_request(http_request request,
                    callback<expected<int>, http_response const &> cb,
                    stop_source stop = {}) {
        auto parsed_url = _http_url_parser{request.url};//request.url（此时是完整 URL,构造解析器
        auto key = parsed_url.m_scheme + parsed_url.m_hostname; // hostname 含端口:池 key = scheme + hostname，无分隔符直接拼接
        auto it = m_conn_pool.find(key);
        http_connection_handler::pointer conn;
        if (it != m_conn_pool.end()) {  
            conn = it->second;
        } else {
            conn = http_connection_handler::make();
            m_conn_pool.insert({key, conn});
        }
        request.url = parsed_url.m_url; // 请求行只写路径
        conn->do_request(std::move(request), std::move(parsed_url),//parsed_url 按值 move 进 handler
                         std::move(cb), stop);
    }
};
