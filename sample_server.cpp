// 最小 HTTP 服务端示例：监听 127.0.0.1:8080，把请求正文原样回显。
// 用来单独验证 http_server 和事件循环，不涉及聊天业务。
#include "io_context.hpp"
#include "http_server.hpp"

// 注册一条回显路由并进入事件循环。
void server() {
    io_context ctx;
    auto server = http_server::make();
    // 根路径：空正文时返回提示，否则回显正文和字节数。；避免tcp粘包
    server->get_router().route("/", [](http_server::http_request &request) {
        std::string response;
        if (request.body.empty()) {
            response = "你好，你的请求正文为空哦";
        } else {
            response = "你好，你的请求是: [" + request.body
                + "]，共 " + std::to_string(request.body.size())
                + " 字节";
        }
        request.write_response(200, response);
    });
    // fmt::println("正在监听：http://127.0.0.1:8080");
    server->do_start("127.0.0.1", "8080");

    ctx.join();
}

// 程序入口：启动示例服务器并阻塞在事件循环。
int main() {
    // try {
        server();
    // } catch (std::system_error const &e) {
    //     fmt::println("{} ({}/{})", e.what(), e.code().category().name(),
    //                  e.code().value());
    // }
    return 0;
}
