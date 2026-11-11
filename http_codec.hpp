// HTTP/1.1 头部与正文的解析器和写入器，服务端与客户端共用。
//
// P1 扩展（协议正确性）：
//   - 请求行拆出 method / path / query / version；
//   - 头部语法兼容 "Key: value" 与 "Key:value"，值去掉前导空格；
//   - Content-Length 严格解析（缺失/非法/重复可被上层区分）；
//   - Transfer-Encoding: chunked 可被识别（上层回 501）；
//   - 正文按 Content-Length 截断，超出部分作为"流水线剩余字节"交给上层；
//   - 状态行使用标准 reason phrase；提供 Date 头格式化。
#pragma once

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

#include "bytes_buffer.hpp"
#include "string_map.hpp"
#include "enum_magic.hpp"

// ---------------- 状态码原因短语 ----------------

inline std::string_view http_status_reason(int status) {
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
    }
}

// RFC 7231 IMF-fixdate：Wed, 21 Oct 2015 07:28:00 GMT
inline std::string http_date_string() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

// ---------------- 头部解析 ----------------

struct http11_header_parser {
    bytes_buffer m_header;    // 原始头部字节
    std::string m_headline;   // 起始行
    string_map m_header_keys; // 头部键值（键统一小写）
    std::string m_body;       // 头部之后多读到的字节（可能是正文或流水线剩余）
    bool m_header_finished = false;
    bool m_bad_header_line = false;      // 某行不是合法的 "键: 值"
    bool m_duplicate_content_length = false;
    bool m_chunked = false;              // Transfer-Encoding 含 chunked

    void reset_state() {
        m_header.clear();
        m_headline.clear();
        m_header_keys.clear();
        m_body.clear();
        m_header_finished = false;
        m_bad_header_line = false;
        m_duplicate_content_length = false;
        m_chunked = false;
    }

    [[nodiscard]] bool header_finished() const {
        return m_header_finished;
    }

    // 解析请求行和每一行 "键: 值"。
    void _extract_headers() {
        std::string_view header = std::string_view(m_header);
        size_t pos = header.find("\r\n");
        m_headline = std::string(header.substr(0, pos == std::string_view::npos ? header.size() : pos));

        while (pos != std::string_view::npos) {
            pos += 2;
            size_t next_pos = header.find("\r\n", pos);
            size_t line_len = (next_pos == std::string_view::npos) ? std::string_view::npos
                                                                   : next_pos - pos;
            std::string_view line = header.substr(pos, line_len);
            if (!line.empty()) {
                size_t colon = line.find(':');
                if (colon == std::string_view::npos || colon == 0) {
                    m_bad_header_line = true;
                } else {
                    std::string key(line.substr(0, colon));
                    for (char &c : key) {
                        if ('A' <= c && c <= 'Z') {
                            c += 'a' - 'A';
                        }
                    }
                    std::string_view value = line.substr(colon + 1);
                    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                        value.remove_prefix(1);
                    }
                    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                        value.remove_suffix(1);
                    }

                    if (key == "content-length") {
                        if (m_header_keys.find(key) != m_header_keys.end()) {
                            m_duplicate_content_length = true;
                        }
                    } else if (key == "transfer-encoding") {
                        std::string lower(value);
                        for (char &c : lower) {
                            if ('A' <= c && c <= 'Z') {
                                c += 'a' - 'A';
                            }
                        }
                        if (lower.find("chunked") != std::string::npos) {
                            m_chunked = true;
                        }
                    }
                    m_header_keys.insert_or_assign(std::move(key), std::string(value));
                }
            }
            pos = next_pos;
        }
    }

    void push_chunk(bytes_const_view chunk) {
        assert(!m_header_finished);
        size_t old_size = m_header.size();
        m_header.append(chunk);
        std::string_view header = std::string_view(m_header);
        if (old_size < 4) {
            old_size = 4;
        }
        old_size -= 4;
        size_t header_len = header.find("\r\n\r\n", old_size);
        if (header_len != std::string_view::npos) {
            m_header_finished = true;
            m_body = std::string(header.substr(header_len + 4));
            m_header.resize(header_len);
            _extract_headers();
        }
    }

    // 兼容旧接口（内部使用）
    std::string &headline() {
        return m_headline;
    }
    string_map &headers() {
        return m_header_keys;
    }
    bytes_buffer &headers_raw() {
        return m_header;
    }
    std::string &extra_body() {
        return m_body;
    }
};

// 在头部解析器之上管理正文长度与"整个消息完成"状态。
template <class HeaderParser = http11_header_parser>
struct _http_base_parser {
    HeaderParser m_header_parser;
    size_t m_content_length = 0;
    size_t m_body_accumulated_size = 0;
    bool m_body_finished = false;

    bool m_cl_present = false;
    bool m_cl_valid = true;
    std::string m_leftover; // Content-Length 之后多出的字节（流水线/下一请求）

    void reset_state() {
        m_header_parser.reset_state();
        m_content_length = 0;
        m_body_accumulated_size = 0;
        m_body_finished = false;
        m_cl_present = false;
        m_cl_valid = true;
        m_leftover.clear();
    }

    [[nodiscard]] bool header_finished() const {
        return m_header_parser.header_finished();
    }
    [[nodiscard]] bool request_finished() const {
        return m_body_finished;
    }
    bool bad_header_line() const {
        return m_header_parser.m_bad_header_line;
    }
    bool duplicate_content_length() const {
        return m_header_parser.m_duplicate_content_length;
    }
    bool chunked() const {
        return m_header_parser.m_chunked;
    }
    bool cl_present() const {
        return m_cl_present;
    }
    bool cl_valid() const {
        return m_cl_valid;
    }
    size_t content_length() const {
        return m_content_length;
    }

    bytes_buffer &headers_raw() { // 原始头部字节（含上限检查用）
        return m_header_parser.headers_raw();
    }
    std::string &headline() {
        return m_header_parser.headline();
    }
    string_map &headers() {
        return m_header_parser.headers();
    }

    std::string _headline_first() const {
        auto const &line = m_header_parser.m_headline;
        size_t space = line.find(' ');
        return space == std::string::npos ? "" : line.substr(0, space);
    }
    std::string _headline_second() const {
        auto const &line = m_header_parser.m_headline;
        size_t space1 = line.find(' ');
        if (space1 == std::string::npos) return "";
        size_t space2 = line.find(' ', space1 + 1);
        if (space2 == std::string::npos) return "";
        return line.substr(space1 + 1, space2 - space1 - 1);
    }
    std::string _headline_third() const {
        auto const &line = m_header_parser.m_headline;
        size_t space1 = line.find(' ');
        if (space1 == std::string::npos) return "";
        size_t space2 = line.find(' ', space1 + 1);
        if (space2 == std::string::npos) return "";
        size_t begin = space2 + 1;
        while (begin < line.size() && line[begin] == ' ') ++begin;
        return line.substr(begin);
    }

    std::string &body() {
        return m_header_parser.extra_body();
    }

    // 解析 Content-Length（严格：全部数字、非空、不溢出）。
    void _parse_content_length() {
        auto &headers = m_header_parser.headers();
        auto it = headers.find("content-length");
        if (it == headers.end()) {
            m_cl_present = false;
            m_content_length = 0;
            return;
        }
        m_cl_present = true;
        std::string_view v = it->second;
        if (v.empty()) {
            m_cl_valid = false;
            return;
        }
        size_t value = 0;
        for (char c : v) {
            if (c < '0' || c > '9') {
                m_cl_valid = false;
                return;
            }
            if (value > (SIZE_MAX - (size_t)(c - '0')) / 10) {
                m_cl_valid = false;
                return;
            }
            value = value * 10 + (size_t)(c - '0');
        }
        m_content_length = value;
    }

    // 收尾：按 Content-Length 截断正文，多出的字节进入 m_leftover。
    void _finish_body() {
        m_body_finished = true;
        if (body().size() > m_content_length) {
            m_leftover = body().substr(m_content_length);
            body().resize(m_content_length);
        }
    }

    void push_chunk(bytes_const_view chunk) {
        assert(!m_body_finished);
        if (!m_header_parser.header_finished()) {
            m_header_parser.push_chunk(chunk);
            if (m_header_parser.header_finished()) {
                _parse_content_length();
                m_body_accumulated_size = body().size();
                if (!m_cl_valid || m_content_length == 0 ||
                    m_body_accumulated_size >= m_content_length) {
                    _finish_body();
                }
            }
        } else {
            body().append(chunk);
            m_body_accumulated_size += chunk.size();
            if (m_body_accumulated_size >= m_content_length) {
                _finish_body();
            }
        }
    }

    std::string take_leftover() {
        return std::move(m_leftover);
    }

    std::string read_some_body() {
        return std::move(body());
    }
};

// ---------------- 请求 / 响应解析器 ----------------

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

inline std::string_view http_method_name(http_method m) {
    switch (m) {
    case http_method::GET: return "GET";
    case http_method::POST: return "POST";
    case http_method::PUT: return "PUT";
    case http_method::DELETE: return "DELETE";
    case http_method::HEAD: return "HEAD";
    case http_method::OPTIONS: return "OPTIONS";
    case http_method::PATCH: return "PATCH";
    case http_method::TRACE: return "TRACE";
    case http_method::CONNECT: return "CONNECT";
    default: return "UNKNOWN";
    }
}

template <class HeaderParser = http11_header_parser>
struct http_request_parser : _http_base_parser<HeaderParser> {
    http_method method() const {
        return parse_enum<http_method>(this->_headline_first());
    }

    // 请求目标（未拆分）
    std::string url() const {
        return this->_headline_second();
    }

    // 路径部分（"?" 之前）
    std::string path() const {
        auto target = url();
        size_t q = target.find('?');
        return q == std::string::npos ? target : target.substr(0, q);
    }

    // 查询串（"?" 之后，不含 "?"；没有则为空）
    std::string query() const {
        auto target = url();
        size_t q = target.find('?');
        return q == std::string::npos ? "" : target.substr(q + 1);
    }

    // "HTTP/1.1" / "HTTP/1.0"
    std::string version() const {
        return this->_headline_third();
    }
};

template <class HeaderParser = http11_header_parser>
struct http_response_parser : _http_base_parser<HeaderParser> {
    int status() const {
        auto s = this->_headline_second();
        try {
            return std::stoi(s);
        } catch (std::logic_error const &) {
            return -1;
        }
    }
};

// ---------------- 写入器 ----------------

struct http11_header_writer {
    bytes_buffer m_buffer;

    void reset_state() {
        m_buffer.clear();
    }
    bytes_buffer &buffer() {
        return m_buffer;
    }

    void begin_header(std::string_view first, std::string_view second,
                      std::string_view third) {
        m_buffer.append(first);
        m_buffer.append_literial(" ");
        m_buffer.append(second);
        m_buffer.append_literial(" ");
        m_buffer.append(third);
    }

    void write_header(std::string_view key, std::string_view value) {
        m_buffer.append_literial("\r\n");
        m_buffer.append(key);
        m_buffer.append_literial(": ");
        m_buffer.append(value);
    }

    void end_header() {
        m_buffer.append_literial("\r\n\r\n");
    }
};

template <class HeaderWriter = http11_header_writer>
struct _http_base_writer {
    HeaderWriter m_header_writer;

    void _begin_header(std::string_view first, std::string_view second,
                       std::string_view third) {
        m_header_writer.begin_header(first, second, third);
    }
    void reset_state() {
        m_header_writer.reset_state();
    }
    bytes_buffer &buffer() {
        return m_header_writer.buffer();
    }
    void write_header(std::string_view key, std::string_view value) {
        m_header_writer.write_header(key, value);
    }
    void end_header() {
        m_header_writer.end_header();
    }
    void write_body(std::string_view body) {
        m_header_writer.buffer().append(body);
    }
};

template <class HeaderWriter = http11_header_writer>
struct http_request_writer : _http_base_writer<HeaderWriter> {
    void begin_header(std::string_view method, std::string_view url) {
        this->_begin_header(method, url, "HTTP/1.1");
    }
};

template <class HeaderWriter = http11_header_writer>
struct http_response_writer : _http_base_writer<HeaderWriter> {
    // 标准状态行：HTTP/1.1 <code> <reason>
    void begin_header(int status, std::string_view reason = {}) {
        if (reason.empty()) {
            reason = http_status_reason(status);
        }
        this->_begin_header("HTTP/1.1", std::to_string(status), reason);
    }
};
