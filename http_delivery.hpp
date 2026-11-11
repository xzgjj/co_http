// HTTP 交付层公共件（回调版与协程版共用）：
//   - 静态资源元数据（ETag / Last-Modified）；
//   - 条件请求 304；
//   - gzip 压缩（zlib）；
//   - 统一的静态响应构造，保证两版行为一致。
#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zlib.h>

struct static_asset {
    std::string content;
    std::string content_type;
    std::string etag;
    std::string last_modified;
    int fd = -1; // 供 sendfile 使用（启动时打开，进程生命周期内保留）
};

// FNV-1a 64 位 → 十六进制 ETag。
inline std::string make_etag(std::string_view content) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : content) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\"%016llx\"",
                  static_cast<unsigned long long>(hash));
    return buf;
}

inline std::string http_date_from_time(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

inline bool accepts_gzip(std::string_view accept_encoding) {
    std::string lower(accept_encoding);
    for (char &c : lower) {
        if ('A' <= c && c <= 'Z') {
            c += 'a' - 'A';
        }
    }
    return lower.find("gzip") != std::string::npos;
}

inline bool etag_matches(std::string_view if_none_match, std::string const &etag) {
    if (if_none_match.empty()) {
        return false;
    }
    std::string value(if_none_match);
    if (value.find('*') != std::string::npos) {
        return true;
    }
    return value.find(etag) != std::string::npos;
}

// gzip 压缩（失败返回空串）。
inline std::string gzip_compress(std::string_view input) {
    z_stream stream{};
    if (deflateInit2(&stream, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    std::string out;
    out.resize(deflateBound(&stream, static_cast<uLong>(input.size())) + 16);
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    int ret = deflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&stream);
        return {};
    }
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

struct static_response {
    int status = 200;
    std::string body;
    std::string content_type;
    std::vector<std::pair<std::string, std::string>> headers;
};

// 条件请求 + 协商压缩，回调版与协程版都调用它，保证两版逐字节一致。
inline static_response build_static_response(static_asset const &asset,
                                             std::string_view if_none_match,
                                             std::string_view accept_encoding) {
    static_response response;
    response.content_type = asset.content_type;
    response.headers.emplace_back("Cache-Control", "no-cache");
    response.headers.emplace_back("ETag", asset.etag);
    response.headers.emplace_back("Last-Modified", asset.last_modified);

    if (etag_matches(if_none_match, asset.etag)) {
        response.status = 304;
        return response;
    }

    bool gzipped = false;
    if (asset.content.size() > 1024 && accepts_gzip(accept_encoding)) {
        std::string compressed = gzip_compress(asset.content);
        if (!compressed.empty() && compressed.size() < asset.content.size()) {
            response.body = std::move(compressed);
            gzipped = true;
        }
    }
    if (!gzipped) {
        response.body = asset.content;
    } else {
        response.headers.emplace_back("Content-Encoding", "gzip");
        response.headers.emplace_back("Vary", "Accept-Encoding");
    }
    response.status = 200;
    return response;
}
