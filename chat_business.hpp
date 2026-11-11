// 聊天业务核心（回调版与协程版共用，保证"统一设计"）：
//   - 消息环形缓冲 + 绝对序号；
//   - chat_hub：长轮询等待者订阅/广播（回调与协程都用 callback<> 唤醒）；
//   - 静态文件启动加载；
//   - /send、/recv 的纯函数式校验与切片，不依赖具体服务器实现。
#pragma once

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <map>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include "io_context.hpp"
#include "reflect.hpp"
#include "file_utils.hpp"
#include "http_delivery.hpp"

namespace chat {

struct Message {
    std::string user;
    std::string content;
    REFLECT(user, content);
};

struct RecvParams {
    uint32_t first;
    REFLECT(first);
};

// 长轮询等待者：respond 恰好执行一次（publish 或超时）。
struct waiter {
    callback<> respond;
    stop_source timer_stop;
    bool done = false;
};

// 可选持久化（SQLite）：消息写入/读取的具体实现放在 hub 之后。
inline bool storage_enabled();
inline void persist_message(Message const &msg);
inline void init_storage(std::string const &path);
inline void load_storage_into_hub(class hub &h);

struct hub {
    std::deque<Message> messages;
    size_t base = 0; // messages[0] 的绝对序号
    size_t capacity = 1000;
    std::vector<std::weak_ptr<waiter>> waiters;

    [[nodiscard]] size_t next_index() const {
        return base + messages.size();
    }

    void append(Message msg) {
        messages.push_back(std::move(msg));
        while (messages.size() > capacity) {
            messages.pop_front();
            ++base;
        }
    }

    std::string slice_json(size_t since) const {
        if (since < base) {
            since = base;
        }
        std::vector<Message> out;
        for (size_t i = since; i < next_index(); ++i) {
            out.push_back(messages[i - base]);
        }
        return reflect::json_encode(out);
    }

    // 订阅一次长轮询：返回 waiter，由调用方设置 respond 与超时。
    std::shared_ptr<waiter> subscribe(callback<> respond) {
        auto state = std::make_shared<waiter>();
        state->respond = std::move(respond);
        waiters.push_back(state);
        return state;
    }

    // 发布一条消息并唤醒所有等待者（延迟到事件循环执行，避免重入）。
    void publish(Message msg) {
        persist_message(msg); // 可选 SQLite 持久化（无库时是空操作）
        append(std::move(msg));
        auto old = std::move(waiters);
        waiters.clear();
        for (auto &weak : old) {
            auto state = weak.lock();
            if (!state || state->done) {
                continue;
            }
            state->done = true;
            state->timer_stop.request_stop();
            io_context::get().post([state] { state->respond(); });
        }
    }
};

inline hub &get_hub() {
    static hub instance;
    return instance;
}

// ---------------- 可选持久化（SQLite）实现 ----------------
// 设置 CHAT_DB=<path> 时启用：启动加载最近 1000 条，/send 的消息写入。
// 未设置时保持纯内存行为（与旧版本一致）。

struct store {
    sqlite3 *db = nullptr;

    [[nodiscard]] bool is_open() const {
        return db != nullptr;
    }

    bool open(std::string const &path) {
        if (sqlite3_open_v2(path.c_str(), &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
            SQLITE_OK) {
            std::fprintf(stderr, "[chat] sqlite open failed: %s\n", sqlite3_errmsg(db));
            if (db) {
                sqlite3_close(db);
                db = nullptr;
            }
            return false;
        }
        char *err = nullptr;
        const char *sql =
            "PRAGMA journal_mode=WAL;"
            "CREATE TABLE IF NOT EXISTS messages ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  user TEXT NOT NULL,"
            "  content TEXT NOT NULL,"
            "  ts INTEGER NOT NULL"
            ");";
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::fprintf(stderr, "[chat] sqlite init failed: %s\n", err ? err : "?");
            sqlite3_free(err);
            sqlite3_close(db);
            db = nullptr;
            return false;
        }
        return true;
    }

    void append(Message const &msg) {
        if (!db) {
            return;
        }
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                               "INSERT INTO messages(user, content, ts) VALUES(?,?,?);",
                               -1, &stmt, nullptr) != SQLITE_OK) {
            std::fprintf(stderr, "[chat] sqlite prepare failed: %s\n", sqlite3_errmsg(db));
            return;
        }
        sqlite3_bind_text(stmt, 1, msg.user.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, msg.content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(std::time(nullptr)));
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::fprintf(stderr, "[chat] sqlite insert failed: %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
    }

    std::vector<Message> load_recent(size_t limit) {
        std::vector<Message> out;
        if (!db) {
            return out;
        }
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(
                db, "SELECT user, content FROM messages ORDER BY id DESC LIMIT ?;", -1,
                &stmt, nullptr) != SQLITE_OK) {
            std::fprintf(stderr, "[chat] sqlite load failed: %s\n", sqlite3_errmsg(db));
            return out;
        }
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(limit));
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto text = [](sqlite3_stmt *s, int col) {
                auto *p = reinterpret_cast<char const *>(sqlite3_column_text(s, col));
                return std::string(p ? p : "");
            };
            out.push_back({text(stmt, 0), text(stmt, 1)});
        }
        sqlite3_finalize(stmt);
        std::reverse(out.begin(), out.end());
        return out;
    }

    ~store() {
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
    }
};

inline store &get_store() {
    static store instance;
    return instance;
}

inline bool storage_enabled() {
    return get_store().is_open();
}

inline void persist_message(Message const &msg) {
    if (storage_enabled()) {
        get_store().append(msg);
    }
}

inline void init_storage(std::string const &path) {
    if (!path.empty()) {
        get_store().open(path);
    }
}

inline void load_storage_into_hub(hub &h) {
    if (!storage_enabled()) {
        return;
    }
    for (auto &msg : get_store().load_recent(1000)) {
        h.append(std::move(msg));
    }
}

// ---------------- 静态文件 ----------------

// 静态资源（含 ETag / Last-Modified，供条件请求与压缩协商使用）
using static_file = static_asset;

inline std::map<std::string, static_file> &static_files() {
    static std::map<std::string, static_file> files;
    return files;
}

inline void load_static_file(std::string const &path, std::string const &url,
                             std::string const &content_type) {
    try {
        std::string content = file_get_content(path);
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        static_files()[url] = {content, content_type, make_etag(content),
                               http_date_from_time(std::time(nullptr)), fd};
    } catch (std::exception const &e) {
        std::fprintf(stderr, "[chat] load '%s' failed: %s\n", path.c_str(), e.what());
    }
}

inline static_file const *find_static(std::string_view url) {
    auto it = static_files().find(std::string(url));
    if (it == static_files().end()) {
        return nullptr;
    }
    return &it->second;
}

// ---------------- /send、/recv 的纯逻辑 ----------------

inline bool send_message(std::string_view json_body) {
    Message msg;
    std::error_code ec;
    if (!reflect::json_decode(json_body, msg, ec)) {
        return false;
    }
    if (msg.user.empty() || msg.user.size() > 64 || msg.content.empty() ||
        msg.content.size() > 4096) {
        return false;
    }
    get_hub().publish(std::move(msg));
    return true;
}

enum class recv_result { ok, invalid };

// 解析 /recv 请求；成功时 since 为游标。返回 ok 表示参数合法（不代表有增量）。
inline recv_result parse_recv(std::string_view json_body, size_t &since) {
    RecvParams params{};
    std::error_code ec;
    if (!reflect::json_decode(json_body, params, ec)) {
        return recv_result::invalid;
    }
    since = params.first;
    auto &h = get_hub();
    if (since > h.next_index() || since < h.base) {
        return recv_result::invalid;
    }
    return recv_result::ok;
}

inline std::string recv_slice(size_t since) {
    return get_hub().slice_json(since);
}

inline bool recv_immediate(size_t since) {
    return since < get_hub().next_index();
}

// 解析 query 里的游标参数（如 "first=12" / "since=3"），非法或缺失返回 0。
inline size_t parse_cursor_query(std::string_view query,
                                 std::string_view name = "first") {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t end = query.find('&', pos);
        if (end == std::string_view::npos) {
            end = query.size();
        }
        std::string_view pair = query.substr(pos, end - pos);
        size_t eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == name) {
            std::string_view value = pair.substr(eq + 1);
            size_t number = 0;
            bool digits = !value.empty();
            for (char c : value) {
                if (c < '0' || c > '9') {
                    digits = false;
                    break;
                }
                number = number * 10 + static_cast<size_t>(c - '0');
            }
            if (digits) {
                return number;
            }
            return 0;
        }
        pos = end + 1;
    }
    return 0;
}

// 注册一个长轮询等待者（respond 由调用方决定如何"完成响应"）。
inline std::shared_ptr<waiter> subscribe_long_poll(callback<> respond) {
    return get_hub().subscribe(std::move(respond));
}

} // namespace chat
