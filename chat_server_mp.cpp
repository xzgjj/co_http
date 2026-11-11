// 多进程实验（P4）：N 个子进程各自 bind（SO_REUSEPORT）并服务同一端口。
//
// 定位：架构对比实验（设计文档 P4-7），不是默认部署形态。
//   - 子进程共用 chat_server_app 的路由/业务（统一设计）；
//   - 各进程内存中的聊天状态互相独立（演示 SO_REUSEPORT 分流与进程级并行）；
//   - 父进程只负责拉起与转发 SIGTERM/SIGINT，退出时不留下孤儿进程。
#include "chat_server_app.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

static pid_t g_children[64];
static int g_child_count = 0;

static void on_parent_signal(int) {
    g_quit = 1;
    for (int i = 0; i < g_child_count; ++i) {
        if (g_children[i] > 0) {
            kill(g_children[i], SIGTERM);
        }
    }
}

static int parse_workers(int argc, char **argv) {
    int workers = 2;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = std::atoi(argv[++i]);
        } else if (std::strncmp(argv[i], "--workers=", 10) == 0) {
            workers = std::atoi(argv[i] + 10);
        }
    }
    if (workers < 1) {
        workers = 1;
    }
    if (workers > 32) {
        workers = 32;
    }
    return workers;
}

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("co_http %s (multiprocess)\n", CO_HTTP_VERSION);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--help") == 0) {
        std::printf("usage: %s [--workers N] [--version] [--help]\n"
                    "  多进程实验：N 个子进程用 SO_REUSEPORT 共享端口（默认 2）\n",
                    argv[0]);
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);
    int workers = parse_workers(argc, argv);

    struct sigaction sa{};
    sa.sa_handler = on_parent_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    for (int i = 0; i < workers; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            // 子进程：换成普通的"置 g_quit"处理器，跑共享应用。
            chat_app::install_signal_handlers();
            int rc = 1;
            try {
                rc = chat_app::run();
            } catch (std::exception const &e) {
                std::fprintf(stderr, "[chat-mp] child fatal: %s\n", e.what());
            }
            _exit(rc);
        }
        if (pid < 0) {
            std::perror("fork");
            break;
        }
        g_children[g_child_count++] = pid;
    }
    if (g_child_count == 0) {
        std::fprintf(stderr, "[chat-mp] no worker started\n");
        return 1;
    }
    std::fprintf(stderr, "[chat-mp] started %d workers (SO_REUSEPORT)\n",
                 g_child_count);

    int alive = g_child_count;
    while (alive > 0) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        --alive;
        if (g_quit) {
            for (int i = 0; i < g_child_count; ++i) {
                if (g_children[i] > 0) {
                    kill(g_children[i], SIGTERM);
                }
            }
        }
    }
    return 0;
}
