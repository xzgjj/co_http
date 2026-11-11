// 最小 C++20 协程任务：Task<T>（惰性启动、对称转移、异常暂存）。
//
// 设计要点（参考 co_async 的结构，自行实现）：
//   - initial_suspend = suspend_always：协程被 co_spawn/co_await 时才运行；
//   - final_suspend 对称转移到 continuation；顶层任务没有 continuation 时自毁；
//   - 异常不 terminate：unhandled_exception 暂存 exception_ptr，由 await_resume
//     或顶层包装统一处理；
//   - 零运行队列、零虚函数：所有权只经过 coroutine_handle。
#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

template <class T = void>
struct Task {
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type {
        std::optional<T> value;
        std::exception_ptr error;
        std::coroutine_handle<> continuation{};

        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        struct final_awaiter {
            bool await_ready() noexcept {
                return false;
            }
            std::coroutine_handle<> await_suspend(handle_type h) noexcept {
                auto &promise = h.promise();
                if (promise.continuation) {
                    return promise.continuation; // 对称转移回调用方
                }
                h.destroy(); // 顶层任务：自毁（detached）
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        final_awaiter final_suspend() noexcept {
            return {};
        }

        template <class U>
        void return_value(U &&v) {
            value.emplace(std::forward<U>(v));
        }
        void unhandled_exception() noexcept {
            error = std::current_exception();
        }
    };

    struct awaiter {
        handle_type handle;

        bool await_ready() const noexcept {
            return false;
        }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            handle.promise().continuation = caller;
            return handle; // 对称转移到被调协程
        }
        T await_resume() {
            auto &promise = handle.promise();
            if (promise.error) {
                std::rethrow_exception(promise.error);
            }
            return std::move(*promise.value);
        }
        ~awaiter() {
            if (handle) {
                handle.destroy();
            }
        }
    };

    handle_type m_handle{};

    Task() = default;
    explicit Task(handle_type h) : m_handle(h) {}
    Task(Task &&other) noexcept : m_handle(std::exchange(other.m_handle, {})) {}
    Task &operator=(Task &&other) noexcept {
        if (this != &other) {
            if (m_handle) {
                m_handle.destroy();
            }
            m_handle = std::exchange(other.m_handle, {});
        }
        return *this;
    }
    Task(Task const &) = delete;
    Task &operator=(Task const &) = delete;

    ~Task() {
        if (m_handle) {
            m_handle.destroy();
        }
    }

    handle_type release() noexcept {
        return std::exchange(m_handle, {});
    }
    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(m_handle);
    }

    awaiter operator co_await() && {
        return awaiter{release()};
    }
};

// Task<void> 特化：无返回值。
template <>
struct Task<void> {
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type {
        std::exception_ptr error;
        std::coroutine_handle<> continuation{};

        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept {
            return {};
        }
        struct final_awaiter {
            bool await_ready() noexcept {
                return false;
            }
            std::coroutine_handle<> await_suspend(handle_type h) noexcept {
                auto &promise = h.promise();
                if (promise.continuation) {
                    return promise.continuation;
                }
                h.destroy();
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        final_awaiter final_suspend() noexcept {
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {
            error = std::current_exception();
        }
    };

    struct awaiter {
        handle_type handle;

        bool await_ready() const noexcept {
            return false;
        }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            handle.promise().continuation = caller;
            return handle;
        }
        void await_resume() {
            if (handle.promise().error) {
                std::rethrow_exception(handle.promise().error);
            }
        }
        ~awaiter() {
            if (handle) {
                handle.destroy();
            }
        }
    };

    handle_type m_handle{};

    Task() = default;
    explicit Task(handle_type h) : m_handle(h) {}
    Task(Task &&other) noexcept : m_handle(std::exchange(other.m_handle, {})) {}
    Task &operator=(Task &&other) noexcept {
        if (this != &other) {
            if (m_handle) {
                m_handle.destroy();
            }
            m_handle = std::exchange(other.m_handle, {});
        }
        return *this;
    }
    Task(Task const &) = delete;
    Task &operator=(Task const &) = delete;

    ~Task() {
        if (m_handle) {
            m_handle.destroy();
        }
    }

    handle_type release() noexcept {
        return std::exchange(m_handle, {});
    }
    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(m_handle);
    }

    awaiter operator co_await() && {
        return awaiter{release()};
    }
};

// 启动一个 detached 任务（顶层任务完成时自毁）。
inline void co_spawn(Task<void> task) {
    auto handle = task.release();
    handle.resume();
}
