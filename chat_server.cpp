// 聊天服务器业务入口：注册静态页面、/send 和 /recv 三条路由，驱动长轮询。
// 全局消息列表和取消源都只在事件循环线程访问，不需要加锁。
#include "io_context.hpp"
#include "http_server.hpp"
#include "file_utils.hpp"
#include "reflect.hpp"
#include <unistd.h>

using namespace std::chrono_literals;

// C++ 全栈，聊天服务器
// 1. AJAX，轮询与长轮询 (OK)
// 2. WebSocket，JSON 消息
//file_utils把内容加载到浏览器里

struct Message {
    std::string user;
    std::string content;

    REFLECT(user, content); // 让 json_encode/json_decode 认识这两个字段
};

struct RecvParams {
    uint32_t first; // 客户端已经收到的消息数量

    REFLECT(first);
};

std::vector<Message> messages; // 全局消息列表，进程内单线程访问
stop_source recv_timeout_stop = stop_source::make(); // 当前长轮询的取消源

// 初始化服务器并注册路由，然后进入事件循环。
void server() {
    io_context ctx;
    chdir("../static"); // 相对 build/ 定位静态文件目录
    messages.push_back({"系统", "你好，欢迎来到聊天室"});
    auto server = http_server::make();
    // 首页：读取并返回 index.html。
    server->get_router().route("/", [](http_server::http_request &request) {
        std::string response = file_get_content("index.html");
        request.write_response(200, response, "text/html");
    });
    // 站点图标：读取并返回 favicon.ico。
    server->get_router().route("/favicon.ico", [](http_server::http_request &request) {
        std::string response = file_get_content("favicon.ico");
        request.write_response(200, response, "image/x-icon");
    });
    // 发消息：解码 JSON、追加到消息列表并唤醒挂起的长轮询。
    server->get_router().route("/send", [](http_server::http_request &request) {
        // fmt::println("/send 收到了 {}", request.body);
        messages.push_back(reflect::json_decode<Message>(request.body));
        recv_timeout_stop.request_stop(); // 唤醒当前等待者
        recv_timeout_stop = stop_source::make(); // 为下一轮长轮询换新令牌
        request.write_response(200, "OK");
    });
    // 拉消息：有新消息立即返回，没有则挂起最多 3 秒。
    server->get_router().route("/recv", [](http_server::http_request &request) {
        auto params = reflect::json_decode<RecvParams>(request.body);
        if (messages.size() > params.first) {
            std::vector<Message> submessages(messages.begin() + params.first,
                                             messages.end());
            std::string response = reflect::json_encode(submessages);
            // fmt::println("/recv 立即返回 {}", response);
            request.write_response(200, response);
        } else {
            // 引用 request：连接处理器由 m_resume 自引用保活
            io_context::get().set_timeout(3s, [&request, params] {
                std::vector<Message> submessages;
                if (messages.size() > params.first) {
                    submessages.assign(messages.begin() + params.first,
                                       messages.end());
                }
                std::string response = reflect::json_encode(submessages);
                // fmt::println("/recv 延迟返回 {}", response);
                request.write_response(200, response);
            }, recv_timeout_stop);
        }
    });
    // fmt::println("正在监听：http://0.0.0.0:8080");
    server->do_start("0.0.0.0", "8080");

    ctx.join();
}

// 程序入口：启动服务器并阻塞在事件循环。
int main() {
    // try {
        server();
    // } catch (std::system_error const &e) {
    //     fmt::println("{} ({}/{})", e.what(), e.code().category().name(),
    //                  e.code().value());
    // }
    return 0;
}
