// 取消令牌：用共享控制块在“定时器超时”和“I/O 完成”之间做二选一。
// 只有 make()/in_place 构造的实例才真正可取消；拷贝共享同一个控制块。
#pragma once

#include "callback.hpp"
#include <memory>

struct stop_source {
    struct _control_block {//_control_block 是共享控制块，保存：
        bool m_stop = false;//m_stop：是否已经请求停止
        callback<> m_cb; // m_cb：单槽停止回调
    };

    std::shared_ptr<_control_block> m_control; //变为指针的操作；指南指针 切可以随意拷贝

    // 默认构造为空令牌，request_stop/set_stop_callback 都是 no-op。
    stop_source() = default;

    // in_place 构造:分配控制块，得到可真正取消的令牌。
    explicit stop_source(std::in_place_t)
        : m_control(std::make_shared<_control_block>()) {}

    // 创建可取消令牌的工厂函数。
    static stop_source make() {
        return stop_source(std::in_place);
    }

    //查询接口
    // 查询是否已经请求停止；空令牌永远返回 false。
    bool stop_requested() const noexcept {
        return m_control && m_control->m_stop;
    }

    // 是否具备取消能力，即是否持有控制块。
    bool stop_possible() const noexcept {
        return m_control != nullptr;
    }

    // 便于 if(token) 判断：持有控制块即为真。
    explicit operator bool() const noexcept {
        return m_control != nullptr;
    }

    //  修改接口:置位停止标志并执行已注册的回调，然后清空回调。
    void request_stop() const {
        if (!m_control) {
            return;
        }
        m_control->m_stop = true;//通过 m_control-> 修改其指向的控制块
        if (m_control->m_cb) {  //检查回调是否为空
            m_control->m_cb();  //同步执行回调
            m_control->m_cb = nullptr; //执行后清空
        }
    }

    // 注册停止回调；这是单槽操作，会覆盖之前注册的回调。
    void set_stop_callback(callback<> cb) const noexcept { //参数按值传 callback<> cb，函数内 std::move 移入控制块，避免多余拷贝。
        if (!m_control) {
            return;
        }
        m_control->m_cb = std::move(cb);
    }

    // 清除停止回调，通常在 I/O 正常完成后调用，避免重复触发。
    void clear_stop_callback() const noexcept {
        if (!m_control) {
            return;
        }
        m_control->m_cb = nullptr;
    }
};
