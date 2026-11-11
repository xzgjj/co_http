// 实时推送的公共件（SSE + WebSocket）：两版服务器共用同一套协议逻辑。
//   - SHA1 / Base64（WebSocket 握手 accept key）；
//   - WebSocket 服务端帧编码 / 客户端帧解析（含掩码）；
//   - SSE 事件格式化。
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "string_map.hpp"

namespace realtime {

// ---------------- SHA1 ----------------

inline uint32_t rotl(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

inline std::string sha1(std::string_view message) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476,
             h4 = 0xC3D2E1F0;
    std::string data(message);
    uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56) {
        data.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        data.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xFF));
    }

    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint8_t>(data[chunk + i * 4]) << 24) |
                   (static_cast<uint8_t>(data[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint8_t>(data[chunk + i * 4 + 2]) << 8) |
                   (static_cast<uint8_t>(data[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::string out(20, '\0');
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<char>((hs[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<char>((hs[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<char>((hs[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<char>(hs[i] & 0xFF);
    }
    return out;
}

inline std::string base64_encode(std::string_view data) {
    static char const *kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t v = (static_cast<uint8_t>(data[i]) << 16) |
                     (static_cast<uint8_t>(data[i + 1]) << 8) |
                     static_cast<uint8_t>(data[i + 2]);
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(kTable[(v >> 6) & 63]);
        out.push_back(kTable[v & 63]);
        i += 3;
    }
    size_t remain = data.size() - i;
    if (remain == 1) {
        uint32_t v = static_cast<uint8_t>(data[i]) << 16;
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.append("==");
    } else if (remain == 2) {
        uint32_t v = (static_cast<uint8_t>(data[i]) << 16) |
                     (static_cast<uint8_t>(data[i + 1]) << 8);
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(kTable[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

// RFC 6455：Sec-WebSocket-Accept = base64(sha1(key + GUID))
inline std::string ws_accept_key(std::string_view client_key) {
    static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string data(client_key);
    data += kGuid;
    return base64_encode(sha1(data));
}

// ---------------- WebSocket 帧 ----------------

enum ws_opcode : int {
    ws_continuation = 0x0,
    ws_text = 0x1,
    ws_binary = 0x2,
    ws_close = 0x8,
    ws_ping = 0x9,
    ws_pong = 0xA,
};

struct ws_frame {
    bool fin = true;
    int opcode = 0;
    std::string payload;
};

// 服务端帧（不掩码）。
inline std::string ws_encode_frame(int opcode, std::string_view payload) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));
    size_t len = payload.size();
    if (len < 126) {
        out.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
        }
    }
    out.append(payload);
    return out;
}

// 解析一条客户端帧（要求掩码）；返回 1=成功 0=数据不完整 -1=协议错误。
inline int ws_parse_frame(std::string &buffer, ws_frame &out, size_t max_payload = 1 << 20) {
    if (buffer.size() < 2) {
        return 0;
    }
    auto *p = reinterpret_cast<unsigned char const *>(buffer.data());
    bool fin = (p[0] & 0x80) != 0;
    int opcode = p[0] & 0x0F;
    bool masked = (p[1] & 0x80) != 0;
    uint64_t len = p[1] & 0x7F;
    size_t pos = 2;
    if (len == 126) {
        if (buffer.size() < pos + 2) {
            return 0;
        }
        len = (static_cast<uint64_t>(p[2]) << 8) | p[3];
        pos += 2;
    } else if (len == 127) {
        if (buffer.size() < pos + 8) {
            return 0;
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | p[pos + i];
        }
        pos += 8;
    }
    if (len > max_payload) {
        return -1;
    }
    if (!masked) {
        return -1; // 客户端帧必须掩码
    }
    if (buffer.size() < pos + 4) {
        return 0;
    }
    unsigned char mask[4];
    std::memcpy(mask, p + pos, 4);
    pos += 4;
    if (buffer.size() < pos + len) {
        return 0;
    }
    out.fin = fin;
    out.opcode = opcode;
    out.payload.assign(reinterpret_cast<char const *>(p + pos), static_cast<size_t>(len));
    for (size_t i = 0; i < out.payload.size(); ++i) {
        out.payload[i] = static_cast<char>(static_cast<unsigned char>(out.payload[i]) ^ mask[i % 4]);
    }
    buffer.erase(0, pos + static_cast<size_t>(len));
    return 1;
}

inline bool ws_is_upgrade(string_map const &headers) {
    auto up = headers.find("upgrade");
    if (up == headers.end()) {
        return false;
    }
    std::string value = up->second;
    for (char &c : value) {
        if ('A' <= c && c <= 'Z') {
            c += 'a' - 'A';
        }
    }
    if (value.find("websocket") == std::string::npos) {
        return false;
    }
    auto conn = headers.find("connection");
    if (conn == headers.end()) {
        return false;
    }
    std::string cvalue = conn->second;
    for (char &c : cvalue) {
        if ('A' <= c && c <= 'Z') {
            c += 'a' - 'A';
        }
    }
    return cvalue.find("upgrade") != std::string::npos;
}

inline std::string ws_101_response(std::string_view accept) {
    std::string out = "HTTP/1.1 101 Switching Protocols\r\n";
    out += "Upgrade: websocket\r\n";
    out += "Connection: Upgrade\r\n";
    out += "Sec-WebSocket-Accept: ";
    out += accept;
    out += "\r\n\r\n";
    return out;
}

// ---------------- SSE ----------------

inline std::string sse_event(std::string_view data) {
    return std::string("data: ") + std::string(data) + "\n\n";
}

inline std::string sse_comment(std::string_view text) {
    return std::string(": ") + std::string(text) + "\n\n";
}

inline std::string sse_response_head() {
    return "HTTP/1.1 200 OK\r\n"
           "Server: co_http\r\n"
           "Content-Type: text/event-stream; charset=utf-8\r\n"
           "Cache-Control: no-store\r\n"
           "Connection: close\r\n"
           "X-Accel-Buffering: no\r\n"
           "\r\n";
}

} // namespace realtime
