// 类型擦除的一次性回调，是整条异步链路的基本单元。异步链路中传递“某个事件发生后要执行的动作”。 
// 按签名 Args... 进行类型擦除的、独占所有权的、默认一次性调用的回调封装 默认调用一次后自动置空；需要重复调用时传 multishot_call。
#pragma once

#include <cassert>
#include <type_traits>
#include <utility>
#include <memory>

inline constexpr struct multishot_call_t {
    explicit multishot_call_t() = default;
} multishot_call; // 显式标记“这次调用不销毁回调”

template <class... Args>
struct callback {
    struct _callback_base {
        // 执行回调，参数类型由外层 callback 决定。
        virtual void _call(Args... args) = 0;

        // 虚析构，保证通过基类指针删除时能调用到实际类型。
        virtual ~_callback_base() = default;
    };

    template <class F>
    struct _callback_impl final : _callback_base {// 保存具体可调用对象 F
        F m_func;

        // 用任意可构造 F 的参数构造包装器。
        template <class... Ts,
                  class = std::enable_if_t<std::is_constructible_v<F, Ts...>>>//构造时完美转发构造
        _callback_impl(Ts &&...ts) : m_func(std::forward<Ts>(ts)...) {}

        // 把调用转发给保存的可调用对象。
        void _call(Args... args) override {
            m_func(std::forward<Args>(args)...);
        }
    };

    std::unique_ptr<_callback_base> m_base;//使用 unique_ptr 独占管理底层实现对象

    // 用任意满足签名的可调用对象构造回调。
    template <class F, class = std::enable_if_t<
                           std::is_invocable_v<F, Args...> &&
                           !std::is_same_v<std::decay_t<F>, callback>>>//decay_t 存储：lambda、函数对象、函数引用会被退化为可存储的值类型。
    callback(F &&f)
        : m_base(std::make_unique<_callback_impl<std::decay_t<F>>>( // 创建类型擦除包装
              std::forward<F>(f))) {}

    // 构造空回调。
    callback() = default;

    callback(std::nullptr_t) noexcept {}// 用 nullptr 构造空回调。
    callback(callback const &) = delete;// 回调独占所有权，禁止拷贝。

    callback &operator=(callback const &) = delete;// 回调独占所有权，禁止拷贝赋值。
 
    callback(callback &&) = default;// 移动构造：转移底层可调用对象。
    callback &operator=(callback &&) = default;// 移动赋值：转移底层可调用对象。

    // 执行回调并销毁，保证同一个回调不会被调用两次。
    void operator()(Args... args) {
        assert(m_base); // 空回调被调用属于使用错误
        m_base->_call(std::forward<Args>(args)...);
        m_base = nullptr; // 所有回调，只能调用一次
    }

    // 多次调用入口：执行后保留回调，供路由等场景复用。
    void operator()(multishot_call_t, Args... args) const {
        assert(m_base);
        m_base->_call(std::forward<Args>(args)...);
    }

    //---------- epoll 等事件循环的集成机制: 地址接口--------------
    // 返回底层对象地址，供 epoll 的 data.ptr 保存。
    void *get_address() const noexcept { //调用之后 callback 对象变成空壳故意漏”
        return static_cast<void *>(m_base.get());//返回底层 _callback_base* 的裸地址。不转移所有权
    }

    // 释放所有权并返回地址，接收方负责还原成 callback。构造新的 callback
    void *leak_address() noexcept {
        return static_cast<void *>(m_base.release());
    }

    // 从地址接管所有权，还原成 callback。
    static callback from_address(void *addr) noexcept {
        callback cb;
        cb.m_base = std::unique_ptr<_callback_base>(
            static_cast<_callback_base *>(addr));
        return cb;
    }

    // 判断是否持有可调用对象。
    explicit operator bool() const noexcept {
        return m_base != nullptr;
    }
};
