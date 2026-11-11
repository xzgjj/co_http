// http_codec 单元测试（快速、无网络）。
// 覆盖：请求行拆分、头部语法、Content-Length 严格解析、chunked 识别、
// 正文/流水线剩余字节切分、状态行 reason phrase。
#include "http_codec.hpp"

#include <cstdio>
#include <cstring>
#include <string>

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

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!((a) == (b))) {                                                 \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__,         \
                         __LINE__, #a, #b);                                  \
        }                                                                    \
    } while (0)

static void test_request_line_and_query() {
    http_request_parser<> p;
    std::string raw =
        "GET /chat/room?x=1&y=2 HTTP/1.1\r\nHost: example.com\r\n\r\n";
    p.push_chunk(bytes_const_view{raw.data(), raw.size()});
    CHECK(p.header_finished());
    CHECK(p.request_finished());
    CHECK_EQ((int)p.method(), (int)http_method::GET);
    CHECK_EQ(p.path(), std::string("/chat/room"));
    CHECK_EQ(p.query(), std::string("x=1&y=2"));
    CHECK_EQ(p.version(), std::string("HTTP/1.1"));
    CHECK_EQ(p.url(), std::string("/chat/room?x=1&y=2"));
}

static void test_query_optional() {
    http_request_parser<> p;
    std::string raw = "POST /recv HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n\r\n{}";
    p.push_chunk(bytes_const_view{raw.data(), raw.size()});
    CHECK_EQ(p.path(), std::string("/recv"));
    CHECK_EQ(p.query(), std::string(""));
    CHECK_EQ(p.body(), std::string("{}"));
    CHECK(p.cl_present());
    CHECK(p.cl_valid());
    CHECK_EQ(p.content_length(), (size_t)2);
}

static void test_header_syntax_variants() {
    http_request_parser<> p;
    // 冒号后无空格 + 值首尾空格 + 大小写键
    std::string raw =
        "GET / HTTP/1.1\r\nHost:h\r\nX-Test:value\r\nX-Space:  padded  \r\n\r\n";
    p.push_chunk(bytes_const_view{raw.data(), raw.size()});
    CHECK(!p.bad_header_line());
    auto &h = p.headers();
    CHECK(h.find("x-test") != h.end());
    CHECK_EQ(h["x-test"], std::string("value"));
    CHECK_EQ(h["x-space"], std::string("padded"));
    CHECK_EQ(h["host"], std::string("h"));
}

static void test_duplicate_content_length() {
    http_request_parser<> p;
    std::string raw =
        "POST /send HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabc";
    p.push_chunk(bytes_const_view{raw.data(), raw.size()});
    CHECK(p.duplicate_content_length());
}

static void test_invalid_content_length() {
    http_request_parser<> p;
    std::string raw = "POST /send HTTP/1.1\r\nHost: h\r\nContent-Length: 12a\r\n\r\n";
    p.push_chunk(bytes_const_view{raw.data(), raw.size()});
    CHECK(p.cl_present());
    CHECK(!p.cl_valid());
}

static void test_chunked_detected() {
    http_request_parser<> p;
    std::string raw =
        "POST /send HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
    p.push_chunk(bytes_const_view{raw.data(), raw.size()});
    CHECK(p.chunked());
}

static void test_body_and_pipeline_leftover() {
    http_request_parser<> p;
    std::string raw =
        "POST /send HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhelloEXTRA";
    p.push_chunk(bytes_const_view{raw.data(), raw.size()});
    CHECK(p.request_finished());
    CHECK_EQ(p.body(), std::string("hello"));
    CHECK_EQ(p.take_leftover(), std::string("EXTRA"));
}

static void test_body_split_across_chunks() {
    http_request_parser<> p;
    std::string head = "POST /send HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\n";
    p.push_chunk(bytes_const_view{head.data(), head.size()});
    CHECK(!p.request_finished());
    std::string part1 = "abcd";
    p.push_chunk(bytes_const_view{part1.data(), part1.size()});
    CHECK(!p.request_finished());
    std::string part2 = "012345NEXT"; // 6 字节正文 + 4 字节流水线
    p.push_chunk(bytes_const_view{part2.data(), part2.size()});
    CHECK(p.request_finished());
    CHECK_EQ(p.body(), std::string("abcd012345"));
    CHECK_EQ(p.take_leftover(), std::string("NEXT"));
}

static void test_header_split_across_chunks() {
    http_request_parser<> p;
    std::string part1 = "GET /abc HTTP/1.1\r\nHo";
    p.push_chunk(bytes_const_view{part1.data(), part1.size()});
    CHECK(!p.header_finished());
    std::string part2 = "st: h\r\n\r\n";
    p.push_chunk(bytes_const_view{part2.data(), part2.size()});
    CHECK(p.header_finished());
    CHECK(p.request_finished());
    CHECK_EQ(p.path(), std::string("/abc"));
}

static void test_bad_header_line() {
    http_request_parser<> p;
    std::string raw = "GET / HTTP/1.1\r\nthis-is-not-a-header\r\nHost: h\r\n\r\n";
    p.push_chunk(bytes_const_view{raw.data(), raw.size()});
    CHECK(p.bad_header_line());
}

static void test_status_reason_and_writer() {
    CHECK_EQ(std::string(http_status_reason(404)), std::string("Not Found"));
    CHECK_EQ(std::string(http_status_reason(431)),
             std::string("Request Header Fields Too Large"));

    http_response_writer<> w;
    w.begin_header(404);
    w.write_header("Content-length", "0");
    w.end_header();
    std::string text(w.buffer().data(), w.buffer().size());
    CHECK(text.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0);
}

static void test_date_string_format() {
    std::string date = http_date_string();
    CHECK_EQ(date.size(), (size_t)29);
    CHECK(date.find("GMT") == 26);
    CHECK_EQ(date[3], ',');
}

int main() {
    test_request_line_and_query();
    test_query_optional();
    test_header_syntax_variants();
    test_duplicate_content_length();
    test_invalid_content_length();
    test_chunked_detected();
    test_body_and_pipeline_leftover();
    test_body_split_across_chunks();
    test_header_split_across_chunks();
    test_bad_header_line();
    test_status_reason_and_writer();
    test_date_string_format();

    if (g_failures == 0) {
        std::printf("codec_tests: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "codec_tests: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
