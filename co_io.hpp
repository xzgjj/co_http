// 协程侧 I/O 适配：把 async_file 的回调接口包装成 awaiter。
// 与回调版共用同一套 io_context 操作槽（"统一设计"的关键：只有控制流不同）。
#pragma once

#include <chrono>
#include <coroutine>

#include "expected.hpp"
#include "io_context.hpp"
#include "task.hpp"

namespace coio {

// co_await coio::tls_handshake{conn}
struct tls_handshake {
    async_file &file;
    stop_source stop;
    expected<int> result{0};

    bool await_ready() const noexcept {
        return !file.tls_enabled();
    }
    void await_suspend(std::coroutine_handle<> handle) {
        auto *self = this;
        file.async_tls_handshake(
            [self, handle](expected<int> ret) {
                self->result = ret;
                io_context::get().post([handle] {
                    if (!handle.done()) {
                        handle.resume();
                    }
                });
            },
            stop);
    }
    expected<int> await_resume() noexcept {
        return result;
    }
};

// co_await coio::read{conn, buffer}
struct read {
    async_file &file;
    bytes_view buffer;
    stop_source stop;
    expected<size_t> result;

    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> handle) {
        auto *self = this;
        file.async_read(
            buffer,
            [self, handle](expected<size_t> ret) {
                self->result = ret;
                // 统一投递到事件循环，避免在 I/O 回调里同步恢复协程（深栈/重入）。
                io_context::get().post([handle] {
                    if (!handle.done()) {
                        handle.resume();
                    }
                });
            },
            stop);
    }
    expected<size_t> await_resume() noexcept {
        return result;
    }
};

// co_await coio::write{conn, buffer}
struct write {
    async_file &file;
    bytes_const_view buffer;
    stop_source stop;
    expected<size_t> result;

    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> handle) {
        auto *self = this;
        file.async_write(
            buffer,
            [self, handle](expected<size_t> ret) {
                self->result = ret;
                io_context::get().post([handle] {
                    if (!handle.done()) {
                        handle.resume();
                    }
                });
            },
            stop);
    }
    expected<size_t> await_resume() noexcept {
        return result;
    }
};

// co_await coio::accept{listening, addr}
struct accept {
    async_file &file;
    address_resolver::address &addr;
    stop_source stop;
    expected<int> result;

    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> handle) {
        auto *self = this;
        file.async_accept(
            addr,
            [self, handle](expected<int> ret) {
                self->result = ret;
                io_context::get().post([handle] {
                    if (!handle.done()) {
                        handle.resume();
                    }
                });
            },
            stop);
    }
    expected<int> await_resume() noexcept {
        return result;
    }
};

// co_await coio::connect{conn, addr_info}
struct connect {
    async_file &file;
    address_resolver::address_info const &addr;
    stop_source stop;
    expected<int> result;

    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> handle) {
        auto *self = this;
        file.async_connect(
            addr,
            [self, handle](expected<int> ret) {
                self->result = ret;
                io_context::get().post([handle] {
                    if (!handle.done()) {
                        handle.resume();
                    }
                });
            },
            stop);
    }
    expected<int> await_resume() noexcept {
        return result;
    }
};

// co_await coio::sleep_for(50ms)
struct sleep_for {
    std::chrono::nanoseconds duration;
    stop_source stop = stop_source::make();

    explicit sleep_for(std::chrono::nanoseconds d) : duration(d) {}

    bool await_ready() const noexcept {
        return duration.count() <= 0;
    }
    void await_suspend(std::coroutine_handle<> handle) {
        io_context::get().set_timeout(
            duration,
            [handle] {
                io_context::get().post([handle] {
                    if (!handle.done()) {
                        handle.resume();
                    }
                });
            },
            stop);
    }
    void await_resume() noexcept {}
};

// co_await coio::sendfile{conn, in_fd, offset, count}
struct sendfile {
    async_file &file;
    int in_fd;
    off_t offset;
    size_t count;
    stop_source stop;
    expected<size_t> result;

    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> handle) {
        auto *self = this;
        file.async_sendfile(
            in_fd, offset, count,
            [self, handle](expected<size_t> ret) {
                self->result = ret;
                io_context::get().post([handle] {
                    if (!handle.done()) {
                        handle.resume();
                    }
                });
            },
            stop);
    }
    expected<size_t> await_resume() noexcept {
        return result;
    }
};

// co_await coio::writev{conn, header, body}
struct writev {
    async_file &file;
    bytes_const_view header;
    bytes_const_view body;
    stop_source stop;
    expected<size_t> result;

    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> handle) {
        auto *self = this;
        file.async_writev(
            header, body,
            [self, handle](expected<size_t> ret) {
                self->result = ret;
                io_context::get().post([handle] {
                    if (!handle.done()) {
                        handle.resume();
                    }
                });
            },
            stop);
    }
    expected<size_t> await_resume() noexcept {
        return result;
    }
};

// 单次事件：用于"长轮询等待者 → 协程恢复"的桥接。
struct async_event {
    bool ready = false;
    std::coroutine_handle<> waiter{};

    // 恰好生效一次；可安全地从发布方/定时器回调调用。
    void notify() {
        if (ready) {
            return;
        }
        ready = true;
        if (waiter) {
            auto handle = waiter;
            waiter = {};
            io_context::get().post([handle] {
                if (!handle.done()) {
                    handle.resume();
                }
            });
        }
    }

    bool await_ready() const noexcept {
        return ready;
    }
    void await_suspend(std::coroutine_handle<> handle) noexcept {
        waiter = handle;
    }
    void await_resume() noexcept {}
};

} // namespace coio
