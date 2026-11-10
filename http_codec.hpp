// HTTP/1.1 头部与正文的解析器和写入器，服务端与客户端共用。
// 只处理 Content-Length 正文；头部键统一小写，解析器每轮请求后必须 reset。
#pragma once

#include "bytes_buffer.hpp"
#include "string_map.hpp"
#include "enum_magic.hpp"

//负责解析 HTTP 头部原始字节："头部字节 → 起始行 + 键值表 + 超量正文"
struct http11_header_parser {
    bytes_buffer m_header;    // "GET / HTTP/1.1\nHost: 142857.red\r\nAccept:原始头部字节
                              // */*\r\nConnection: close"
    std::string m_headline;   // "GET / HTTP/1.1"起始行
    string_map m_header_keys; // {"Host": "142857.red", "Accept": "*/*",头部键值表
                              // "Connection": "close"}
    std::string m_body; // 不小心超量读取的正文（如果有的话）
    bool m_header_finished = false;// 头部是否读完

    // 清空上一轮状态，准备解析下一个请求或响应。
    void reset_state() {
        m_header.clear();
        m_headline.clear();
        m_header_keys.clear();
        m_body.clear();
        m_header_finished = 0;
    }

    // 头部是否已经读到空行。
    [[nodiscard]] bool header_finished() {  //提醒调用方不要丢弃返回值
        return m_header_finished; // 如果正文都结束了，就不再需要更多数据
    }

    // 解析请求行和每一行 "键: 值"，键统一转成小写。
    void _extract_headers() {
        std::string_view header = m_header;
        size_t pos = header.find("\r\n", 0, 2);
        m_headline = std::string(header.substr(0, pos)); // 第一行是请求行/状态行;第一个 \r\n 之前是起始行
        while (pos != std::string::npos) {
            // 跳过 "\r\n"
            pos += 2;
            // 从当前位置开始找，先找到下一行位置（可能为 npos）
            size_t next_pos = header.find("\r\n", pos, 2);
            size_t line_len = std::string::npos;
            if (next_pos != std::string::npos) {
                // 如果下一行还不是结束，那么 line_len 设为本行开始到下一行之间的距离
                line_len = next_pos - pos; //找到下一处 \r\n，本行长度 = 下一行起点 − 本行起点。
            }
            // 就能切下本行
            std::string_view line = header.substr(pos, line_len);
            size_t colon = line.find(": ", 0, 2);
            if (colon != std::string::npos) {
                // 每一行都是 "键: 值"
                std::string key = std::string(line.substr(0, colon));
                std::string_view value = line.substr(colon + 2);
                // 键统一转成小写，实现大小写不敏感的判断
                for (char &c: key) {  //某个 \r\n 位置开始，只有在 next_pos == npos 时下一次判断 pos != npos 失败才退出，
                    if ('A' <= c && c <= 'Z') {
                        c += 'a' - 'A';
                    }
                }
                // 古代 C++ 过时的写法：m_header_keys[key] = value;
                // 现代 C++17 的高效写法：
                m_header_keys.insert_or_assign(std::move(key), value);
            }
            pos = next_pos;
        }
    }

    // 追加一段数据，查找头部结束标记 "\r\n\r\n"。
    void push_chunk(bytes_const_view chunk) {
        assert(!m_header_finished);  //头部已结束不应再调用，是状态机前置条件
        size_t old_size = m_header.size();
        m_header.append(chunk);
        std::string_view header = m_header;
        // 如果还在解析头部的话，尝试判断头部是否结束
        if (old_size < 4) {  //防止 size_t 下溢为巨大值
            old_size = 4;
        }
        old_size -= 4; // 从上一次可能的分隔符前缀处开始找
        size_t header_len = header.find("\r\n\r\n", old_size, 4);
        if (header_len != std::string::npos) {
            
            m_header_finished = true;// 头部已经结束      
            m_body = header.substr(header_len + 4);// 把不小心多读取的正文留下
            m_header.resize(header_len); // 截掉分隔符及其后的正文
            // 开始分析头部，尝试提取 Content-length 字段
            _extract_headers();
        }
    }
    //--------- 访问器引用返回，允许外部修改 ----------------
    // 返回请求行/状态行。
    std::string &headline() {
        return m_headline;
    }

    // 返回头部键值表。
    string_map &headers() {
        return m_header_keys;
    }

    // 返回原始头部字节。
    bytes_buffer &headers_raw() {
        return m_header;
    }

    // 返回随头部一起读到的多余正文。
    std::string &extra_body() {
        return m_body;
    }
};

//在头部解析器之上管理正文长度与"整个消息完成"状态
template <class HeaderParser = http11_header_parser>
struct _http_base_parser {
    HeaderParser m_header_parser;
    size_t m_content_length = 0;
    size_t body_accumulated_size = 0;
    bool m_body_finished = false;

    // 清空头部、长度和正文状态。:先重置头部解析器，再重置正文状态。
    void reset_state() {
        m_header_parser.reset_state();
        m_content_length = 0;
        body_accumulated_size = 0;
        m_body_finished = false;
    }

    // 头部是否完成。
    [[nodiscard]] bool header_finished() {
        return m_header_parser.header_finished();
    }

    // 整个消息（头部 + 正文）是否完成。
    [[nodiscard]] bool request_finished() {
        return m_body_finished;
    }

    // 返回原始头部字节。
    std::string &headers_raw() {
        return m_header_parser.headers_raw();
    }

    // 返回请求行/状态行。
    std::string &headline() {
        return m_header_parser.headline();
    }

    // 返回头部键值表。
    string_map &headers() {
        return m_header_parser.headers();
    }

    // 取首行第一段，如 "GET" 或 "HTTP/1.1"。
    std::string _headline_first() {
        // "GET / HTTP/1.1" request
        // "HTTP/1.1 200 OK" response
        auto &line = headline();
        size_t space = line.find(' ');
        if (space == std::string::npos) {
            return "";
        }
        return line.substr(0, space);
    }

    // 取首行第二段，如 URL 或状态码。
    std::string _headline_second() {
        // "GET / HTTP/1.1"
        auto &line = headline();
        size_t space1 = line.find(' ');
        if (space1 == std::string::npos) {
            return "";
        }
        size_t space2 = line.find(' ', space1 + 1);
        if (space2 == std::string::npos) {
            return "";
        }
        return line.substr(space1 + 1, space2 - (space1 + 1));
    }

    // 取首行剩余部分，如 "HTTP/1.1" 或 "OK"。
    std::string _headline_third() {
        // "GET / HTTP/1.1"
        auto &line = headline();
        size_t space1 = line.find(' ');
        if (space1 == std::string::npos) {
            return "";
        }
        size_t space2 = line.find(' ', space1 + 1);
        if (space2 == std::string::npos) {
            return "";
        }
        return line.substr(space2);
    }

    // 返回当前累计的正文。
    std::string &body() {
        return m_header_parser.extra_body();
    }

    // 从头部读取 Content-Length，缺失或格式错误时按 0 处理。
    size_t _extract_content_length() {
        auto &headers = m_header_parser.headers();
        auto it = headers.find("content-length"); //选择迭代器 失败能获取值
        if (it == headers.end()) {
            return 0;
        }
        try {
            return std::stoi(it->second);
        } catch (std::logic_error const &) {
            return 0;
        }
    }

    // 追加数据：头部未完成先喂给头部解析器，完成后按长度累计正文。
    //头部之后的多余字节存入 m_body，作为正文起点；正文按长度截断
    void push_chunk(bytes_const_view chunk) {
        assert(!m_body_finished);
        if (!m_header_parser.header_finished()) {  //头部未完成:交给头部解析器；它可能仍在读头部
            m_header_parser.push_chunk(chunk);
            if (m_header_parser.header_finished()) {
                body_accumulated_size = body().size(); // 头部里夹带的正文;它的 size() 初始化 body_accumulated_size
                m_content_length = _extract_content_length();
                if (body_accumulated_size >= m_content_length) { //若超量读取部分已经 ≥ 长度 → 完成
                    m_body_finished = true; // 正文已经一次读满
                }
            }
        } else {  //头部已完成
            body().append(chunk);  // 持续累积
            body_accumulated_size += chunk.size();
            if (body_accumulated_size >= m_content_length) {
                m_body_finished = true; // 正文收满
            }
        }
    }

    // 取走正文，通常只调用一次。
    std::string read_some_body() {
        return std::move(body());
    }
};

//请求/响应解析器
enum class http_method {
    UNKNOWN = -1,
    GET,
    POST,
    PUT,
    DELETE,
    HEAD,
    OPTIONS,
    PATCH,
    TRACE,
    CONNECT,
};

template <class HeaderParser = http11_header_parser>
struct http_request_parser : _http_base_parser<HeaderParser> {
    // 解析请求行第一段得到 HTTP 方法。
    http_method method() {
        return parse_enum<http_method>(this->_headline_first());
    }

    // 解析请求行第二段得到 URL。
    std::string url() {
        return this->_headline_second();
    }
};

template <class HeaderParser = http11_header_parser>
struct http_response_parser : _http_base_parser<HeaderParser> {
    // 解析状态行第二段得到状态码。;封装上状态码
    int status() {
        auto s = this->_headline_second();
        try {
            return std::stoi(s);
        } catch (std::logic_error const &) {
            return -1;
        }
    }
};

struct http11_header_writer {
    bytes_buffer m_buffer;

    // 清空待发送缓冲区。
    void reset_state() {
        m_buffer.clear();
    }

    // 返回待发送缓冲区。
    bytes_buffer &buffer() {
        return m_buffer;
    }

    // 写起始行，三段之间用空格分隔。
    void begin_header(std::string_view first, std::string_view second,
                      std::string_view third) {
        m_buffer.append(first);
        m_buffer.append_literial(" ");
        m_buffer.append(second);
        m_buffer.append_literial(" ");
        m_buffer.append(third);
    }

    // 写一行头部，格式为 "键: 值"。
    void write_header(std::string_view key, std::string_view value) {
        m_buffer.append_literial("\r\n");
        m_buffer.append(key);
        m_buffer.append_literial(": ");
        m_buffer.append(value);
    }

    // 写空行，头部结束。
    void end_header() {
        m_buffer.append_literial("\r\n\r\n");
    }
};

template <class HeaderWriter = http11_header_writer>
struct _http_base_writer {
    HeaderWriter m_header_writer;

    // 写起始行，具体内容由派生类决定。
    void _begin_header(std::string_view first, std::string_view second,
                       std::string_view third) {
        m_header_writer.begin_header(first, second, third);
    }

    // 清空待发送缓冲区。
    void reset_state() {
        m_header_writer.reset_state();
    }

    // 返回待发送缓冲区。
    bytes_buffer &buffer() {
        return m_header_writer.buffer();
    }

    // 写一行头部。
    void write_header(std::string_view key, std::string_view value) {
        m_header_writer.write_header(key, value);
    }

    // 写空行，头部结束。
    void end_header() {
        m_header_writer.end_header();
    }

    // 追加正文。
    void write_body(std::string_view body) {
        m_header_writer.buffer().append(body);
    }
};

template <class HeaderWriter = http11_header_writer>
struct http_request_writer : _http_base_writer<HeaderWriter> {
    // 写请求行："方法 URL HTTP/1.1"。
    void begin_header(std::string_view method, std::string_view url) {
        this->_begin_header(method, url, "HTTP/1.1");
    }
};

template <class HeaderWriter = http11_header_writer>
struct http_response_writer : _http_base_writer<HeaderWriter> {
    // 写状态行："HTTP/1.1 状态码 OK"。
    void begin_header(int status) {
        this->_begin_header("HTTP/1.1", std::to_string(status), "OK");
    }
};
