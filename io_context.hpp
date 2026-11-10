// 单线程、基于 epoll 的极简异步 I/O 事件循环，思路 asio io_context：把 read / write / accept / connect 这些阻塞调用，
//统一包装成"注册回调 → 事件到达 → 执行回调"的形式
// 单线程运行；回调地址直接存入 epoll 的 data.ptr，事件到达时还原并执行。

#pragma once

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <system_error>
#include "timer_context.hpp"
#include "bytes_buffer.hpp"
#include "expected.hpp"
#include <cassert>
#include <array>

//timer_context 是时间维度的事件源:到期时刻 → 回调”的表，并回答一个问题——“距离下一个定时器到期还有多久
struct io_context : timer_context {
    int m_epfd; // epoll 实例
    size_t m_epcount = 0; // 已注册、尚未触发的回调数量

    static inline thread_local io_context *g_instance = nullptr; // 单线程模型的核心约束当前线程的事件循环

    // 创建 epoll 实例并登记为当前线程的事件循环。 构造函数
    io_context()
        : m_epfd(convert_error(epoll_create1(0)).expect("epoll_create")) {
        g_instance = this;
    }

    // 事件循环主体：同时驱动 epoll 事件和定时器，直到两者都为空。;算超时 → 睡 → 收事件 → 执行回调 → 计数减一，
    void join() {
        std::array<struct epoll_event, 128> events;
        while (!is_empty()) {  //定时器空 且 无在途回调才退出。回调不会因循环退出而丢失。
            std::chrono::nanoseconds dt = duration_to_next_timer(); // 最近的超时时间
#if HAS_epoll_pwait2
            struct timespec timeout, *timeoutp = nullptr;//计算和返回的进度不同：只是把毫秒精度换成 timespec 纳秒精度，纳秒精度
            if (dt.count() >= 0) {
                timeout.tv_sec = dt.count() / 1'000'000'000;
                timeout.tv_nsec = dt.count() % 1'000'000'000;
                timeoutp = &timeout;
            }
            int ret =
                convert_error(epoll_pwait2(m_epfd, events.data(), events.size(),//man看返回
                                           timeoutp, nullptr))
                    .expect("epoll_pwait2");
#else
            int timeout_ms = -1;
            if (dt.count() >= 0) {
                timeout_ms = dt.count() / 1'000'000; // 毫秒精度
            }
            int ret =
                convert_error(epoll_pwait(m_epfd, events.data(), events.size(),
                                           timeout_ms, nullptr))
                    .expect("epoll_pwait");
#endif
            for (int i = 0; i < ret; ++i) {  //回调还原与执行：
                auto cb = callback<>::from_address(events[i].data.ptr); // 接管回调所有权
                cb(); // 回调只能执行一次
                --m_epcount; // 在途回调少一个
            }
        }
    }

    //析构 关闭 epoll 实例并清理线程单例。
    ~io_context() {
        close(m_epfd);
        g_instance = nullptr;
    }

    // 取当前线程的事件循环，只能在 join 所在的线程使用。
    [[gnu::const]] static io_context &get() {//GCC 该函数只依赖全局常量状态、无副作用，可被优化
        assert(g_instance);
        return *g_instance;
    }

    // 没有定时器且没有在途回调时，事件循环可以退出。
    bool is_empty() const {//is_empty() 是 join() 退出的唯一判据。
        return timer_context::is_empty() && m_epcount == 0;
    }
};

struct file_descriptor { //fd 的 RAII 封装
    int m_fd = -1;
 
    file_descriptor() = default;// 默认构造空 fd。

    // 没有拷贝构造/赋值（被移动操作隐式删除），防止 fd 被双重关闭
    explicit file_descriptor(int fd) : m_fd(fd) {}//接管一个已打开的 fd。

    // 移动构造：转移 fd 所有权
    file_descriptor(file_descriptor &&that) noexcept : m_fd(that.m_fd) {
        that.m_fd = -1;
    }

    // 移动赋值：交换 fd，旧 fd 随临时对象析构关闭。
    file_descriptor &operator=(file_descriptor &&that) noexcept {
        std::swap(m_fd, that.m_fd);
        return *this;
    }

    // 析构时关闭仍持有的 fd。
    ~file_descriptor() {
        if (m_fd == -1) {
            return;
        }
        close(m_fd);
    }
};

// getaddrinfo 专用的错误分类，让错误信息走 gai_strerror。；原有错误分类不够支持
inline std::error_category const &gai_category() {
    static struct final : std::error_category {//局部静态匿名派生类实例 ,单例
        // 错误分类名。
        char const *name() const noexcept override {
            return "getaddrinfo";
        }

        // 把 getaddrinfo 错误码转成可读文本。
        std::string message(int err) const override {
            return gai_strerror(err);
        }
    } instance;

    return instance;
}

struct address_resolver { //网络库底层地址解析组件:  C 风格的"链表 + 手动 free"变成自动管理生命周期的对象
    struct address_ref { //不拥有内存的"地址指针 + 长度"视图,用于把 sockaddr* 传给 syscall
        struct sockaddr *m_addr;
        socklen_t m_addrlen;
    };

    struct address { //实际存储。
        union {
            struct sockaddr m_addr;
            struct sockaddr_storage m_addr_storage; // 保证足够容纳IPv4/IPv6/Unix 任意地址
        };

        socklen_t m_addrlen = sizeof(struct sockaddr_storage);//内核写回时会把 m_addrlen 更新成实际地址长度，

        // 隐式转成“地址 + 长度”的引用。
        operator address_ref() {
            return {&m_addr, m_addrlen};
        }
    };

    struct address_info {  //链表游标，代表"当前地址项";指针视图
        struct addrinfo *m_curr = nullptr;

        // 取当前地址项的地址引用。
        address_ref get_address() const {
            return {m_curr->ai_addr, m_curr->ai_addrlen};
        }

        // 按当前地址项创建 socket。
        int create_socket() const {
            return convert_error(socket(m_curr->ai_family, m_curr->ai_socktype,
                                        m_curr->ai_protocol))
                .expect("socket");
        }

        // 移动到下一个地址项；没有更多项时返回 false。; 防止忽略返回值造成死循环
        [[nodiscard]] bool next_entry() {
            m_curr = m_curr->ai_next;
            if (m_curr == nullptr) {
                return false;
            }
            return true;
        }
    };

    struct addrinfo *m_head = nullptr; // getaddrinfo 返回的链表头;hints 传 NULL，即默认行为

    // 解析域名和服务名，失败时抛 system_error。;
    //生命周期：resolve 返回 address_info m_curr指向 m_head链表内部——address_resolver 必须活得比所有 address_info 久
    address_info resolve(std::string const &name, std::string const &service) {
        int err = getaddrinfo(name.c_str(), service.c_str(), NULL, &m_head);//getaddrinfo 返回 EAI_* 错误码，
        if (err != 0) {
            auto ec = std::error_code(err, gai_category());
            throw std::system_error(ec, name + ":" + service);
        }
        return {m_head};
    }

    // 默认构造空解析器。
    address_resolver() = default;

    // 移动构造：转移链表所有权。
    address_resolver(address_resolver &&that) : m_head(that.m_head) {
        that.m_head = nullptr;
    }

    // 释放 getaddrinfo 返回的链表。
    ~address_resolver() {
        if (m_head) {
            freeaddrinfo(m_head);
        }
    }
};

//注册进 epoll 后边沿触发
struct async_file : file_descriptor {//一个“fd 的异步句柄，每个对象绑定一个非阻塞 fd 并注册进 epoll。
    // 默认构造空文件描述符。
    async_file() = default;

    // 接管已打开的 fd：设置非阻塞并加入 epoll。;接管 fd + 非阻塞 + 注册 epoll
    explicit async_file(int fd) : file_descriptor(fd) {
        int flags = convert_error(fcntl(m_fd, F_GETFL)).expect("F_GETFL");//先读默认后加O_NONBLOCK
        flags |= O_NONBLOCK; // 所有异步 I/O 都要求设置非阻塞
        convert_error(fcntl(m_fd, F_SETFL, flags)).expect("F_SETFL");//抛 std::system_error

        struct epoll_event event;//注册 epoll 时 events = EPOLLET（边沿触发），data.ptr = nullptr（此刻无在途回调）。
        event.events = EPOLLET;
        event.data.ptr = nullptr; // 此刻还没有待执行回调
        convert_error(
            epoll_ctl(io_context::get().m_epfd, EPOLL_CTL_ADD, m_fd, &event))
            .expect("EPOLL_CTL_ADD");
    }

    // 把一次异步操作挂到 epoll：保存回调地址并登记在途计数。稍后要执行的回调”
    void _epoll_callback(callback<> &&resume, uint32_t events,
                         stop_source stop) {
        struct epoll_event event;
        event.events = events;
        event.data.ptr = resume.get_address(); //"稍后要执行的回调"的地址塞进 epoll 不转移所有权
        convert_error(
            epoll_ctl(io_context::get().m_epfd, EPOLL_CTL_MOD, m_fd, &event))
            .expect("EPOLL_CTL_MOD");// 装填关注事件 + 回调地址
        ++io_context::get().m_epcount; // 标记"有一个在途回调加一
        stop.set_stop_callback([resume_ptr = resume.leak_address()] {
            callback<>::from_address(resume_ptr)(); // 取消 = 提前把回调"恢复"执行
        });
    }

    // 异步读：先试读一次，遇到 EAGAIN 才注册 EPOLLIN。是优化；数据已在缓冲时不进内核等待；同步会阻塞
    void async_read(bytes_view buf, callback<expected<size_t>> cb,//边沿模式下真正执行读的仍是 async_read
                    stop_source stop = {}) {
#if USE_LEVEL_TRIGGER
        // 如果 read 可以读了，请操作系统，调用，我这个回调
        return _epoll_callback(
            [this, buf, cb = std::move(cb), stop]() mutable {
                if (stop.stop_requested()) {
                    stop.clear_stop_callback();
                    return cb(-ECANCELED); // 注册前检查取消：若已取消，走 cb(-ECANCELED)
                }
                auto ret = convert_error<size_t>(read(m_fd, buf.data(), buf.size()));
                stop.clear_stop_callback();
                return cb(ret);
            },
            EPOLLIN | EPOLLERR | EPOLLONESHOT, stop);
#else
        if (stop.stop_requested()) {  //只有 EAGAIN 才注册：真正需要等待才挂 epoll。
            stop.clear_stop_callback();
            return cb(-ECANCELED); // 注册前就已被取消
        }
        auto ret = convert_error<size_t>(read(m_fd, buf.data(), buf.size()));
        if (!ret.is_error(EAGAIN)) {
            stop.clear_stop_callback(); // 数据已就绪，无需再取消
            return cb(ret);
        }

        // 如果 read 可以读了，请操作系统，调用，我这个回调
        return _epoll_callback(
            [this, buf, cb = std::move(cb), stop]() mutable {
                return async_read(buf, std::move(cb), stop); // 事件到达后重试读取
            },
            EPOLLIN | EPOLLERR | EPOLLET | EPOLLONESHOT, stop);
#endif
    }

    // 异步写：先试写一次，遇到 EAGAIN 才注册 EPOLLOUT。
    void async_write(bytes_const_view buf, callback<expected<size_t>> cb,
                     stop_source stop = {}) {
#if USE_LEVEL_TRIGGER
        // 如果 write 可以写了，请操作系统，调用，我这个回调
        return _epoll_callback(
            [this, buf, cb = std::move(cb), stop]() mutable {
                if (stop.stop_requested()) {
                    stop.clear_stop_callback();
                    return cb(-ECANCELED); // 已被取消
                }
                auto ret = convert_error<size_t>(write(m_fd, buf.data(), buf.size()));
                return cb(ret);
            },
            EPOLLOUT | EPOLLERR | EPOLLONESHOT, stop);
#else
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return cb(-ECANCELED); // 注册前就已被取消
        }
        auto ret = convert_error<size_t>(write(m_fd, buf.data(), buf.size()));
        if (!ret.is_error(EAGAIN)) {
            stop.clear_stop_callback(); // 已写出部分或全部数据
            return cb(ret);
        }

        // 如果 write 可以写了，请操作系统，调用，我这个回调
        return _epoll_callback(
            [this, buf, cb = std::move(cb), stop]() mutable {
                return async_write(buf, std::move(cb), stop); // 事件到达后重试写入
            },
            EPOLLOUT | EPOLLERR | EPOLLET | EPOLLONESHOT, stop);
#endif
    }

    // 异步 accept：先试一次，没有新连接时等待 EPOLLIN。
    void async_accept(address_resolver::address &addr,
                      callback<expected<int>> cb, stop_source stop = {}) {
#if USE_LEVEL_TRIGGER
        // 如果 accept 到请求了，请操作系统，调用，我这个回调
        return _epoll_callback(
            [this, &addr, cb = std::move(cb), stop]() mutable {
                if (stop.stop_requested()) {
                    stop.clear_stop_callback();
                    return cb(-ECANCELED); // 已被取消
                }
                auto ret = convert_error<int>(accept(m_fd, &addr.m_addr, &addr.m_addrlen));
                return cb(ret);
            },
            EPOLLIN | EPOLLERR | EPOLLONESHOT, stop);
#else
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return cb(-ECANCELED); // 注册前就已被取消
        }
        auto ret =
            convert_error<int>(accept(m_fd, &addr.m_addr, &addr.m_addrlen));
        if (!ret.is_error(EAGAIN)) {
            stop.clear_stop_callback(); // 已接到连接
            return cb(ret);
        }

        // 如果 accept 到请求了，请操作系统，调用，我这个回调
        return _epoll_callback(
            [this, &addr, cb = std::move(cb), stop]() mutable {
                return async_accept(addr, std::move(cb), stop); // 事件到达后重试
            },
            EPOLLIN | EPOLLERR | EPOLLET | EPOLLONESHOT, stop);
#endif
    }

    // 异步 connect：非阻塞连接，EINPROGRESS 时等待 EPOLLOUT 并用 SO_ERROR 取结果。；
    void async_connect(address_resolver::address_info const &addr,   //在已构造好、已置非阻塞的 async_file 上发起一次非阻塞 connect
                       callback<expected<int>> cb, stop_source stop = {}) {
        if (stop.stop_requested()) {
            stop.clear_stop_callback();
            return cb(-ECANCELED); // 与 read/write/accept 一致：注册前就被取消 注册前就已被取消
        }
        auto addr_ptr = addr.get_address();//发起 connect:连接结果通过回调给出——0 = 成功，负值 = -errno，-ECANCELED = 被取消。
        auto ret =
            convert_error(connect(m_fd, addr_ptr.m_addr, addr_ptr.m_addrlen));
        if (!ret.is_error(EINPROGRESS)) {
            stop.clear_stop_callback(); // 立即成功或立即失败
            return cb(ret);
        }
        return _epoll_callback( //挂起等待
            [this, cb = std::move(cb), stop]() mutable { //关注 EPOLLOUT：非阻塞 connect 的三次握手在后台进行，完成（成功或失败）后 socket 变为可写
                if (stop.stop_requested()) {   //事件到达后的收尾
                    stop.clear_stop_callback();
                    return cb(-ECANCELED); // 等待期间已被取消
                }
                int ret;
                socklen_t ret_len = sizeof(ret);
                convert_error(
                    getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &ret, &ret_len))
                    .expect("getsockopt");
                if (ret > 0) {
                    ret = -ret; // SO_ERROR 是正 errno，统一转成负值
                }
                stop.clear_stop_callback();
                return cb(ret);   // 0=连上，负=失败原因
            },
            EPOLLOUT | EPOLLERR | EPOLLONESHOT, stop);
    }

    // 创建监听 socket：设置地址复用、bind、listen，返回非阻塞的 async_file。
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

    // 移动构造：转移 fd 所有权。
    async_file(async_file &&) = default;

    // 移动赋值：转移 fd 所有权。
    async_file &operator=(async_file &&) = default;

    // 析构时先从 epoll 注销，再关闭 fd。
    ~async_file() {
        if (m_fd == -1) {
            return;
        }
        epoll_ctl(io_context::get().m_epfd, EPOLL_CTL_DEL, m_fd, nullptr);
    }

    // 判断是否持有一个有效的 fd。
    explicit operator bool() const noexcept {
        return m_fd != -1;
    }
};
