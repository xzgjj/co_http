// 单线程、基于 epoll 的异步 I/O 事件循环（P0 稳定性重构版）。
//
// 与旧版的关键区别：
//   1. epoll 的 data 只放 fd（不再把裸回调指针塞进 data.ptr）——
//      事件到达时按 fd 查"在途操作槽"，查不到就交给 fd 的所有者收尾，
//      因此"对端 RST / 无在途操作"绝不会再解引用空指针。
//   2. 在途操作有明确生命周期：注册 → 触发/取消 → 恰好恢复一次；
//      取消会解除 epoll 武装（EPOLL_CTL_MOD 0）并投递到延迟队列，不重入。
//   3. 事件先整批收集、再统一执行，避免回调里关闭后续事件的 fd。
//   4. 信号 / 跨线程唤醒走 eventfd；epoll_wait 的 EINTR 直接重试。
#pragma once

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "timer_context.hpp"
#include "bytes_buffer.hpp"
#include "expected.hpp"
#include "tls.hpp"
#include <cassert>
#include <array>

// 进程级退出标志：信号处理器只做 async-signal-safe 的 write(eventfd)。
inline volatile std::sig_atomic_t g_quit = 0;
inline int g_signal_wake_fd = -1;

// fd 的所有者：当该 fd 收到事件、但没有任何在途操作时被通知（典型是 RST/HUP）。
struct fd_watcher {
    virtual void on_stray_events(uint32_t events) = 0;

protected:
    ~fd_watcher() = default;
};

// 事件循环：epoll + 定时器 + 延迟队列（定时器能力来自 timer_context）。
struct io_context : timer_context {
    // 单线程模型：当前线程的事件循环实例。
    static inline thread_local io_context *g_instance = nullptr;

    int m_epfd = -1;   // epoll 实例
    int m_wake_fd = -1; // eventfd：信号 / 跨线程 post 唤醒
    std::unordered_map<int, callback<>> m_ops;        // fd -> 在途操作续体
    std::unordered_map<int, fd_watcher *> m_watchers; // fd -> 所有者
    std::deque<callback<>> m_posted;                  // 延迟执行队列（禁止在回调里同步重入）

    io_context() {
        m_epfd = epoll_create1(EPOLL_CLOEXEC);
        if (m_epfd < 0) {
            throw std::system_error(errno, std::generic_category(), "epoll_create1");
        }
        m_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (m_wake_fd < 0) {
            close(m_epfd);
            throw std::system_error(errno, std::generic_category(), "eventfd");
        }
        g_signal_wake_fd = m_wake_fd;
        _epoll_ctl(EPOLL_CTL_ADD, m_wake_fd, EPOLLIN);
        g_instance = this;
    }

    ~io_context() {
        // 先"失联"：析构期间销毁的回调（可能持有连接/服务器的最后一个引用）
        // 收尾时调用 async_file 析构 → try_get() 返回 nullptr，不再回访本对象，
        // 从而避免"析构中重入 m_ops.erase"这类 use-after-free。
        g_instance = nullptr;
        if (g_signal_wake_fd == m_wake_fd) {
            g_signal_wake_fd = -1;
        }
        m_posted.clear();
        m_ops.clear();
        m_watchers.clear();
        if (m_wake_fd >= 0) {
            close(m_wake_fd);
            m_wake_fd = -1;
        }
        if (m_epfd >= 0) {
            close(m_epfd);
            m_epfd = -1;
        }
    }

    io_context(io_context const &) = delete;
    io_context &operator=(io_context const &) = delete;

    [[nodiscard]] static io_context &get() {
        assert(g_instance);
        return *g_instance;
    }

    [[nodiscard]] static io_context *try_get() noexcept {
        return g_instance;
    }

    // ---------------- fd 注册 / 操作槽 ----------------

    // fd 创建时登记所有者；此时没有在途操作。
    void add_fd(int fd, fd_watcher *owner) {
        m_watchers[fd] = owner;
        _epoll_ctl(EPOLL_CTL_ADD, fd, 0);
    }

    // fd 所有权移动（async_file 移动）后重新绑定所有者。
    void rebind_fd(int fd, fd_watcher *owner) {
        m_watchers[fd] = owner;
    }

    // fd 销毁：注销 epoll、清掉在途操作与所有者。
    void remove_fd(int fd, fd_watcher *owner = nullptr) {
        auto wit = m_watchers.find(fd);
        if (wit != m_watchers.end() && (owner == nullptr || wit->second == owner)) {
            m_watchers.erase(wit);
        }
        m_ops.erase(fd);
        _epoll_ctl(EPOLL_CTL_DEL, fd, 0);
    }

    // 注册一次在途操作：fd 上同一时刻只允许一个操作（epoll + ONESHOT 的约束）。
    void arm(int fd, uint32_t events, callback<> resume, stop_source stop) {
        m_ops[fd] = std::move(resume);
        _epoll_ctl(EPOLL_CTL_MOD, fd, events | EPOLLET | EPOLLONESHOT);
        if (stop) {
            // 取消：解除武装 + 投递（不在这里直接执行续体，避免同步重入）。
            stop.set_stop_callback([fd] {
                if (auto *ctx = io_context::try_get()) {
                    ctx->cancel_fd(fd);
                }
            });
        }
    }

    // 取消某个 fd 上的在途操作；续体会在事件循环里收到 -ECANCELED。
    void cancel_fd(int fd) {
        auto it = m_ops.find(fd);
        if (it == m_ops.end()) {
            return;
        }
        callback<> resume = std::move(it->second);
        m_ops.erase(it);
        _epoll_ctl(EPOLL_CTL_MOD, fd, 0);
        post(std::move(resume));
    }

    // 延迟执行（也用于信号/跨线程唤醒）。
    void post(callback<> cb) {
        m_posted.push_back(std::move(cb));
        _wake();
    }

    // 请求退出事件循环（信号处理、测试用）。
    void request_stop() {
        g_quit = 1;
        _wake();
    }

    // 定时器与在途操作都清空、且没有延迟任务时，事件循环可以退出。
    [[nodiscard]] bool is_empty() const {
        return timer_context::is_empty() && m_ops.empty() && m_posted.empty();
    }

    // ---------------- 主循环 ----------------

    void join() {
        std::array<struct epoll_event, 128> events;
        std::vector<std::pair<int, uint32_t>> ready;
        ready.reserve(events.size());

        while (!g_quit) {
            if (is_empty()) {
                break;
            }
            _drain_posted();
            if (g_quit) {
                break;
            }

            std::chrono::nanoseconds dt = duration_to_next_timer();
            _drain_posted();
            if (g_quit) {
                break;
            }

            int timeout_ms = -1;
            if (dt.count() >= 0) {
                timeout_ms = (int)std::min<long long>(dt.count() / 1000000, 60000);
            }

            int n = epoll_wait(m_epfd, events.data(), (int)events.size(), timeout_ms);
            if (n < 0) {
                if (errno == EINTR) {
                    continue; // 信号打断：回到循环顶部检查 g_quit
                }
                continue; // 其他错误不终止进程（教学项目：记录后继续）
            }

            // 先整批收集，再执行；这样前面回调关闭 fd 不会影响后面的事件。
            ready.clear();
            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                uint32_t ev = events[i].events;
                ready.emplace_back(fd, ev);
            }
            for (auto const &[fd, ev] : ready) {
                if (fd == m_wake_fd) {
                    _drain_wake();
                    continue;
                }
                _on_event(fd, ev);
            }
            _drain_posted();
        }
    }

private:
    void _on_event(int fd, uint32_t events) {
        auto it = m_ops.find(fd);
        if (it != m_ops.end()) {
            callback<> resume = std::move(it->second);
            m_ops.erase(it);
            _epoll_ctl(EPOLL_CTL_MOD, fd, 0); // ONESHOT 已消费，解除武装
            post(std::move(resume));
            return;
        }
        // 没有在途操作：该 fd 的所有者负责收尾（RST/HUP 等）。
        auto wit = m_watchers.find(fd);
        if (wit != m_watchers.end() && wit->second) {
            wit->second->on_stray_events(events);
        }
    }

    void _drain_posted() {
        while (!m_posted.empty()) {
            callback<> cb = std::move(m_posted.front());
            m_posted.pop_front();
            cb();
        }
    }

    void _wake() {
        if (m_wake_fd < 0) {
            return;
        }
        uint64_t one = 1;
        ssize_t r = write(m_wake_fd, &one, sizeof(one));
        (void)r;
    }

    void _drain_wake() {
        if (m_wake_fd < 0) {
            return;
        }
        uint64_t value = 0;
        while (read(m_wake_fd, &value, sizeof(value)) > 0) {
        }
    }

    // 注册/修改/删除 epoll 关注；失败不抛异常（事件循环里不允许异常逃逸）。
    void _epoll_ctl(int op, int fd, uint32_t events) {
        struct epoll_event event{};
        event.events = events;
        event.data.fd = fd;
        if (epoll_ctl(m_epfd, op, fd, &event) == 0) {
            return;
        }
        if (errno == ENOENT && op != EPOLL_CTL_ADD) {
            return; // 已注销
        }
        if (errno == EEXIST && op == EPOLL_CTL_ADD) {
            event.events = events;
            event.data.fd = fd;
            epoll_ctl(m_epfd, EPOLL_CTL_MOD, fd, &event);
        }
    }
};

// ---------------- fd 的 RAII 封装 ----------------

struct file_descriptor {
    int m_fd = -1;

    file_descriptor() = default;
    explicit file_descriptor(int fd) : m_fd(fd) {}
    file_descriptor(file_descriptor &&that) noexcept : m_fd(that.m_fd) {
        that.m_fd = -1;
    }
    file_descriptor &operator=(file_descriptor &&that) noexcept {
        std::swap(m_fd, that.m_fd);
        return *this;
    }
    file_descriptor(file_descriptor const &) = delete;
    file_descriptor &operator=(file_descriptor const &) = delete;

    ~file_descriptor() {
        if (m_fd != -1) {
            close(m_fd);
        }
    }
};

// getaddrinfo 专用的错误分类。
inline std::error_category const &gai_category() {
    static struct final : std::error_category {
        char const *name() const noexcept override {
            return "getaddrinfo";
        }

        std::string message(int err) const override {
            return gai_strerror(err);
        }
    } instance;

    return instance;
}

struct address_resolver {
    struct address_ref {
        struct sockaddr *m_addr;
        socklen_t m_addrlen;
    };

    struct address {
        union {
            struct sockaddr m_addr;
            struct sockaddr_storage m_addr_storage;
        };

        socklen_t m_addrlen = sizeof(struct sockaddr_storage);

        operator address_ref() {
            return {&m_addr, m_addrlen};
        }
    };

    struct address_info {
        struct addrinfo *m_curr = nullptr;

        address_ref get_address() const {
            return {m_curr->ai_addr, m_curr->ai_addrlen};
        }

        int create_socket() const {
            return convert_error(socket(m_curr->ai_family, m_curr->ai_socktype,
                                        m_curr->ai_protocol))
                .expect("socket");
        }

        [[nodiscard]] bool next_entry() {
            m_curr = m_curr->ai_next;
            return m_curr != nullptr;
        }
    };

    struct addrinfo *m_head = nullptr;

    address_info resolve(std::string const &name, std::string const &service) {
        int err = getaddrinfo(name.c_str(), service.c_str(), nullptr, &m_head);
        if (err != 0) {
            throw std::system_error(std::error_code(err, gai_category()),
                                    name + ":" + service);
        }
        return {m_head};
    }

    address_resolver() = default;
    address_resolver(address_resolver &&that) : m_head(that.m_head) {
        that.m_head = nullptr;
    }
    address_resolver(address_resolver const &) = delete;
    address_resolver &operator=(address_resolver const &) = delete;

    ~address_resolver() {
        if (m_head) {
            freeaddrinfo(m_head);
        }
    }
};

// ---------------- 异步 fd ----------------

// 一个绑定非阻塞 fd 的异步句柄：支持"回调风格"接口（保留给对照组），
// 内部统一走 io_context 的操作槽。
struct async_file : file_descriptor, fd_watcher {
    SSL *m_ssl = nullptr; // 设置后本句柄的读写走 TLS
    async_file() = default;

    explicit async_file(int fd) : file_descriptor(fd) {
        _set_nonblocking();
        if (auto *ctx = io_context::try_get()) {
            ctx->add_fd(m_fd, this);
        }
    }

    async_file(async_file &&that) noexcept : file_descriptor(std::move(that)) {
        m_ssl = std::exchange(that.m_ssl, nullptr);
        if (m_fd >= 0) {
            if (auto *ctx = io_context::try_get()) {
                ctx->rebind_fd(m_fd, this);
            }
        }
    }

    async_file &operator=(async_file &&that) noexcept {
        if (this != &that) {
            if (m_ssl) {
                SSL_free(m_ssl);
                m_ssl = nullptr;
            }
            if (m_fd >= 0) {
                if (auto *ctx = io_context::try_get()) {
                    ctx->remove_fd(m_fd, this);
                }
            }
            file_descriptor::operator=(std::move(that));
            m_ssl = std::exchange(that.m_ssl, nullptr);
            if (m_fd >= 0) {
                if (auto *ctx = io_context::try_get()) {
                    ctx->rebind_fd(m_fd, this);
                }
            }
        }
        return *this;
    }

    async_file(async_file const &) = delete;
    async_file &operator=(async_file const &) = delete;

    ~async_file() {
        if (m_ssl) {
            SSL_free(m_ssl);
            m_ssl = nullptr;
        }
        if (m_fd >= 0) {
            if (auto *ctx = io_context::try_get()) {
                ctx->remove_fd(m_fd, this);
            }
        }
    }

    // 无在途操作却收到事件（RST/HUP/对端关闭）：主动关闭，避免 fd 泄漏。
    void on_stray_events(uint32_t events) override {
        (void)events;
        close_now();
    }

    // 立刻关闭（可从事件循环内部安全调用）。
    void close_now() {
        if (m_fd < 0) {
            return;
        }
        if (auto *ctx = io_context::try_get()) {
            ctx->remove_fd(m_fd, this);
        }
        int fd = m_fd;
        m_fd = -1;
        close(fd);
    }

    explicit operator bool() const noexcept {
        return m_fd != -1;
    }

    // ---------------- TLS（可选） ----------------

    void adopt_tls(SSL *ssl) {
        m_ssl = ssl;
    }
    [[nodiscard]] bool tls_enabled() const noexcept {
        return m_ssl != nullptr;
    }

    // 异步完成 TLS 握手（非阻塞；WANT_READ/WANT_WRITE 映射到 epoll 关注）。
    void async_tls_handshake(callback<expected<int>> cb, stop_source stop = {}) {
        if (!m_ssl) {
            stop.clear_stop_callback();
            return cb(0);
        }
        if (m_fd < 0) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return cb(-ECANCELED);
        }
        ERR_clear_error();
        int ret = SSL_do_handshake(m_ssl);
        if (ret == 1) {
            stop.clear_stop_callback();
            return cb(0);
        }
        int err = SSL_get_error(m_ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            auto *ctx = io_context::try_get();
            if (!ctx) {
                stop.clear_stop_callback();
                return cb(-EBADF);
            }
            uint32_t events = (err == SSL_ERROR_WANT_READ) ? EPOLLIN : EPOLLOUT;
            ctx->arm(m_fd, events,
                     [this, cb = std::move(cb), stop]() mutable {
                         return async_tls_handshake(std::move(cb), stop);
                     },
                     stop);
            return;
        }
        stop.clear_stop_callback();
        return cb(-EIO);
    }

    // 异步读：先试读一次，EAGAIN 才注册 EPOLLIN。
    void async_read(bytes_view buf, callback<expected<size_t>> cb,
                    stop_source stop = {}) {
        if (m_fd < 0) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return cb(-ECANCELED);
        }
        if (m_ssl) {
            size_t space = buf.size();
            if (space > (1u << 20)) {
                space = 1u << 20;
            }
            int n = SSL_read(m_ssl, buf.data(), static_cast<int>(space));
            if (n > 0) {
                stop.clear_stop_callback();
                return cb(static_cast<size_t>(n));
            }
            int err = SSL_get_error(m_ssl, n);
            if (err == SSL_ERROR_ZERO_RETURN) {
                stop.clear_stop_callback();
                return cb(static_cast<size_t>(0));
            }
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                auto *ctx = io_context::try_get();
                if (!ctx) {
                    stop.clear_stop_callback();
                    return cb(-EBADF);
                }
                uint32_t events = (err == SSL_ERROR_WANT_READ) ? EPOLLIN : EPOLLOUT;
                ctx->arm(m_fd, events,
                         [this, buf, cb = std::move(cb), stop]() mutable {
                             return async_read(buf, std::move(cb), stop);
                         },
                         stop);
                return;
            }
            stop.clear_stop_callback();
            return cb(-EIO);
        }
        auto ret = convert_error<size_t>(read(m_fd, buf.data(), buf.size()));
        if (!ret.is_error(EAGAIN)) {
            stop.clear_stop_callback();
            return cb(ret);
        }
        auto *ctx = io_context::try_get();
        if (!ctx) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        ctx->arm(m_fd, EPOLLIN, [this, buf, cb = std::move(cb), stop]() mutable {
            return async_read(buf, std::move(cb), stop); // 事件到达后重试
        }, stop);
    }

    // 异步写：先试写一次，EAGAIN 才注册 EPOLLOUT。
    void async_write(bytes_const_view buf, callback<expected<size_t>> cb,
                     stop_source stop = {}) {
        if (m_fd < 0) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return cb(-ECANCELED);
        }
        if (m_ssl) {
            size_t space = buf.size();
            if (space > (1u << 20)) {
                space = 1u << 20;
            }
            int n = SSL_write(m_ssl, buf.data(), static_cast<int>(space));
            if (n > 0) {
                stop.clear_stop_callback();
                return cb(static_cast<size_t>(n));
            }
            int err = SSL_get_error(m_ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                auto *ctx = io_context::try_get();
                if (!ctx) {
                    stop.clear_stop_callback();
                    return cb(-EBADF);
                }
                uint32_t events = (err == SSL_ERROR_WANT_READ) ? EPOLLIN : EPOLLOUT;
                ctx->arm(m_fd, events,
                         [this, buf, cb = std::move(cb), stop]() mutable {
                             return async_write(buf, std::move(cb), stop);
                         },
                         stop);
                return;
            }
            stop.clear_stop_callback();
            return cb(-EIO);
        }
        auto ret = convert_error<size_t>(write(m_fd, buf.data(), buf.size()));
        if (!ret.is_error(EAGAIN)) {
            stop.clear_stop_callback();
            return cb(ret);
        }
        auto *ctx = io_context::try_get();
        if (!ctx) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        ctx->arm(m_fd, EPOLLOUT, [this, buf, cb = std::move(cb), stop]() mutable {
            return async_write(buf, std::move(cb), stop);
        }, stop);
    }

    // 异步 accept：先试一次，EAGAIN 才注册 EPOLLIN。
    void async_accept(address_resolver::address &addr,
                      callback<expected<int>> cb, stop_source stop = {}) {
        if (m_fd < 0) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return cb(-ECANCELED);
        }
        auto ret = convert_error<int>(accept(m_fd, &addr.m_addr, &addr.m_addrlen));
        if (!ret.is_error(EAGAIN)) {
            stop.clear_stop_callback();
            return cb(ret);
        }
        auto *ctx = io_context::try_get();
        if (!ctx) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        ctx->arm(m_fd, EPOLLIN, [this, &addr, cb = std::move(cb), stop]() mutable {
            return async_accept(addr, std::move(cb), stop);
        }, stop);
    }

    // 异步 sendfile：静态文件零拷贝（内核态直传，不经过用户缓冲）。
    void async_sendfile(int in_fd, off_t offset, size_t count,
                        callback<expected<size_t>> cb, stop_source stop = {}) {
        if (m_ssl) { // TLS 下没有零拷贝路径
            stop.clear_stop_callback();
            return cb(-ENOTSUP);
        }
        if (m_fd < 0 || in_fd < 0) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return cb(-ECANCELED);
        }
        off_t off = offset;
        ssize_t ret = ::sendfile(m_fd, in_fd, &off, count);
        if (ret >= 0) {
            stop.clear_stop_callback();
            return cb(static_cast<expected<size_t>>(static_cast<size_t>(ret)));
        }
        if (errno != EAGAIN && errno != EINTR) {
            stop.clear_stop_callback();
            return cb(-errno);
        }
        auto *ctx = io_context::try_get();
        if (!ctx) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        ctx->arm(m_fd, EPOLLOUT,
                 [this, in_fd, offset, count, cb = std::move(cb), stop]() mutable {
                     return async_sendfile(in_fd, offset, count, std::move(cb), stop);
                 },
                 stop);
    }

    // 异步 writev：把"响应头 + 正文"一次写出（减少一次用户态拷贝/系统调用）。
    void async_writev(bytes_const_view a, bytes_const_view b,
                      callback<expected<size_t>> cb, stop_source stop = {}) {
        if (m_fd < 0) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return cb(-ECANCELED);
        }
        if (m_ssl) {
            // TLS 没有 writev：拼接后按普通写发出（仍是异步非阻塞）。
            auto combined = std::make_shared<std::string>();
            combined->reserve(a.size() + b.size());
            combined->append(a.data(), a.size());
            combined->append(b.data(), b.size());
            async_write(
                bytes_const_view{combined->data(), combined->size()},
                [combined, cb = std::move(cb)](expected<size_t> ret) mutable {
                    cb(ret);
                },
                stop);
            return;
        }
        struct iovec iov[2];
        int iovcnt = 0;
        if (a.size() != 0) {
            iov[iovcnt++] = {const_cast<char *>(a.data()), a.size()};
        }
        if (b.size() != 0) {
            iov[iovcnt++] = {const_cast<char *>(b.data()), b.size()};
        }
        if (iovcnt == 0) {
            stop.clear_stop_callback();
            return cb(static_cast<expected<size_t>>(static_cast<size_t>(0)));
        }
        ssize_t ret = ::writev(m_fd, iov, iovcnt);
        if (ret >= 0) {
            stop.clear_stop_callback();
            return cb(static_cast<expected<size_t>>(static_cast<size_t>(ret)));
        }
        if (errno != EAGAIN && errno != EINTR) {
            stop.clear_stop_callback();
            return cb(-errno);
        }
        auto *ctx = io_context::try_get();
        if (!ctx) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        ctx->arm(m_fd, EPOLLOUT,
                 [this, a, b, cb = std::move(cb), stop]() mutable {
                     return async_writev(a, b, std::move(cb), stop);
                 },
                 stop);
    }
    // 异步 connect：EINPROGRESS 时等 EPOLLOUT，再用 SO_ERROR 取结果。
    void async_connect(address_resolver::address_info const &addr,
                       callback<expected<int>> cb, stop_source stop = {}) {
        if (m_fd < 0) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return cb(-ECANCELED);
        }
        auto addr_ptr = addr.get_address();
        auto ret = convert_error<int>(connect(m_fd, addr_ptr.m_addr, addr_ptr.m_addrlen));
        if (!ret.is_error(EINPROGRESS)) {
            stop.clear_stop_callback();
            return cb(ret);
        }
        auto *ctx = io_context::try_get();
        if (!ctx) {
            stop.clear_stop_callback();
            return cb(-EBADF);
        }
        ctx->arm(m_fd, EPOLLOUT, [this, cb = std::move(cb), stop]() mutable {
            if (stop.stop_requested()) {
                stop.clear_stop_callback();
                return cb(-ECANCELED);
            }
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
                err = errno;
            }
            stop.clear_stop_callback();
            return cb(err > 0 ? -err : 0);
        }, stop);
    }

    // 创建监听 socket：地址复用、bind、listen，返回非阻塞的 async_file。
    static async_file async_bind(address_resolver::address_info const &addr) {
        auto sock = async_file{addr.create_socket()};
        auto serve_addr = addr.get_address();
        int on = 1;
        setsockopt(sock.m_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(sock.m_fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
        convert_error(bind(sock.m_fd, serve_addr.m_addr, serve_addr.m_addrlen))
            .expect("bind");
        convert_error(listen(sock.m_fd, SOMAXCONN)).expect("listen");
        return sock;
    }

private:
    void _set_nonblocking() {
        int flags = convert_error(fcntl(m_fd, F_GETFL)).expect("F_GETFL");
        flags |= O_NONBLOCK;
        convert_error(fcntl(m_fd, F_SETFL, flags)).expect("F_SETFL");
    }
};
