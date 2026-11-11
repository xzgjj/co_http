// WebSocket 广播注册表（两版服务器共用）：
// 每个 WS 连接注册一个 push(text)；文本消息通过 broadcast 推给其他所有连接。
// 单线程事件循环下无锁；订阅者用 weak_ptr 管理生命周期。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ws_subscriber {
    std::function<void(std::string)> push;
    std::function<void()> closed;
};

struct ws_hub {
    std::vector<std::weak_ptr<ws_subscriber>> clients;

    std::shared_ptr<ws_subscriber> add(std::function<void(std::string)> push,
                                       std::function<void()> closed = {}) {
        auto sub = std::make_shared<ws_subscriber>();
        sub->push = std::move(push);
        sub->closed = std::move(closed);
        clients.push_back(sub);
        return sub;
    }

    // 广播给除 except 外的所有在线 WS 客户端；顺带清理失效订阅。
    void broadcast(std::string const &text, ws_subscriber *except = nullptr) {
        auto old = std::move(clients);
        clients.clear();
        for (auto &weak : old) {
            auto sub = weak.lock();
            if (!sub) {
                continue;
            }
            clients.push_back(sub);
            if (sub.get() != except) {
                sub->push(text);
            }
        }
    }

    [[nodiscard]] size_t size() const {
        size_t count = 0;
        for (auto &weak : clients) {
            if (!weak.expired()) {
                ++count;
            }
        }
        return count;
    }
};

inline ws_hub &ws_clients() {
    static ws_hub instance;
    return instance;
}
