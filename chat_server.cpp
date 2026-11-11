// 聊天服务器（回调版，单进程）：应用逻辑在 chat_server_app.hpp，与多进程实验共用。
#include "chat_server_app.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("co_http %s (callback)\n", CO_HTTP_VERSION);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--help") == 0) {
        std::printf("usage: %s [--version] [--help]\n"
                    "  env: PORT STATIC_ROOT CHAT_DB MAX_HEADER MAX_URL MAX_BODY "
                    "MAX_CONN\n"
                    "       REQUEST_TIMEOUT_MS WRITE_TIMEOUT_MS POLL_TIMEOUT_MS\n",
                    argv[0]);
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);
    chat_app::install_signal_handlers();

    try {
        return chat_app::run();
    } catch (std::exception const &e) {
        std::fprintf(stderr, "[chat] fatal: %s\n", e.what());
        return 1;
    }
}
