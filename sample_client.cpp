// 最小 HTTP 客户端示例：访问 142857.red，1 秒后再请求一次。
// 用来验证 http_client 的连接池和异步请求链路，需要外网。
#include "io_context.hpp"
#include "http_client.hpp"

// 发起第一次请求，成功后延迟 1 秒再发一次，然后进入事件循环。
void client() {
    io_context ctx;
    auto client = http_client::make();

    client->do_request(
        {"GET", "http://142857.red"},
        [client](expected<int> ret,
                 http_client::http_response const &response) {
            ret.expect("http://142857.red"); // 连接或读取失败会抛出
            // fmt::println("{}", response.body);

            // 第一次请求完成后，1 秒后再请求一次验证连接复用。
            io_context::get().set_timeout(std::chrono::seconds(1), [client] {
                client->do_request(
                    {"GET", "http://142857.red"},
                    [client](expected<int> ret,
                             http_client::http_response const &response) {
                        ret.expect("http://142857.red");
                        // fmt::println("{}", response.body);
                    });
            });
        });

    ctx.join();
}

// 程序入口：启动示例客户端并阻塞在事件循环。
int main() {
    // try {
        client();
    // } catch (std::system_error const &e) {
    //     fmt::println("{} ({}/{})", e.what(), e.code().category().name(),
    //                  e.code().value());
    // }
    return 0;
}
