// realtime（SSE / WebSocket）单元测试：SHA1、Base64、握手 accept、帧编解码、SSE 格式。
#include "realtime.hpp"

#include <cstdio>
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

static std::string hex(std::string const &raw) {
    static char const *digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

static std::string make_client_frame(int opcode, std::string_view payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(0x80 | len));
    } else {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    }
    unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
    frame.append(reinterpret_cast<char *>(mask), 4);
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    }
    return frame;
}

static void test_sha1_base64() {
    CHECK(hex(realtime::sha1("abc")) ==
          "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(realtime::sha1("")) ==
          "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(realtime::base64_encode("hello") == "aGVsbG8=");
    CHECK(realtime::base64_encode("") == "");
    CHECK(realtime::base64_encode("ab") == "YWI=");
}

static void test_ws_accept() {
    // RFC 6455 示例
    CHECK(realtime::ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==") ==
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

static void test_ws_frames() {
    // 服务端帧：无掩码
    std::string server = realtime::ws_encode_frame(realtime::ws_text, "hi");
    CHECK(server.size() == 4);
    CHECK(static_cast<unsigned char>(server[0]) == 0x81);
    CHECK(static_cast<unsigned char>(server[1]) == 2);
    CHECK(server.substr(2) == "hi");

    // 客户端帧解析（含掩码）
    std::string buffer = make_client_frame(realtime::ws_text, "hello");
    realtime::ws_frame frame;
    CHECK(realtime::ws_parse_frame(buffer, frame) == 1);
    CHECK(frame.opcode == realtime::ws_text);
    CHECK(frame.payload == "hello");
    CHECK(buffer.empty());

    // 不完整帧：返回 0 且不消费
    buffer = make_client_frame(realtime::ws_text, "abc").substr(0, 3);
    size_t before = buffer.size();
    CHECK(realtime::ws_parse_frame(buffer, frame) == 0);
    CHECK(buffer.size() == before);

    // 未掩码的客户端帧：协议错误
    std::string unmasked = realtime::ws_encode_frame(realtime::ws_text, "x");
    CHECK(realtime::ws_parse_frame(unmasked, frame) == -1);

    // 长载荷（126 扩展长度）
    std::string long_payload(300, 'x');
    buffer = make_client_frame(realtime::ws_text, long_payload);
    CHECK(realtime::ws_parse_frame(buffer, frame) == 1);
    CHECK(frame.payload == long_payload);
}

static void test_sse_format() {
    CHECK(realtime::sse_event("{\"a\":1}") == "data: {\"a\":1}\n\n");
    CHECK(realtime::sse_comment("ok") == ": ok\n\n");
    std::string head = realtime::sse_response_head();
    CHECK(head.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(head.find("text/event-stream") != std::string::npos);
}

int main() {
    test_sha1_base64();
    test_ws_accept();
    test_ws_frames();
    test_sse_format();
    if (g_failures == 0) {
        std::printf("realtime_tests: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "realtime_tests: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
