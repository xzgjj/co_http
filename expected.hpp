// 轻量错误返回：用有符号整数同时表示结果和 -errno 错误码。 异步回调统一用它传递返回值，负值即失败，0 或正值即成功。
//于异步回调、系统调用封装等场景，避免额外堆分配和复杂类型，同时提供 value()/expect() 抛异常
#pragma once

#include <type_traits>
#include <system_error>

//错误分类 错误变成字符串的方法不一样
template <class T>
struct [[nodiscard]] expected {
    std::make_signed_t<T> m_res;

    // 默认构造，结果为 0。
    expected() = default;

    // 用有符号结果构造，负数表示 -errno。
    expected(std::make_signed_t<T> res) noexcept : m_res(res) {}

    // 返回错误码：失败时为负的 errno，成功时为 0。
    int error() const noexcept {
        if (m_res < 0) {
            return m_res;
        }
        return 0;
    }

    // 判断是否为指定的错误码。
    bool is_error(int err) const noexcept {
        return m_res == -err;
    }

    // 转换为 std::error_code：负值取相反数作为 errno。
    std::error_code error_code() const noexcept {
        if (m_res < 0) {
            return std::error_code(-m_res, std::system_category());
        }
        return std::error_code();
    }

    // 成功时返回结果，失败时抛出带上下文的 system_error。
    T expect(char const *what) const {
        if (m_res < 0) {
            auto ec = error_code();
            // fmt::println(stderr, "{}: {}", what, ec.message());
            throw std::system_error(ec, what);
        }
        return m_res;
    }

    // 成功时返回结果，失败时只抛错误码。
    T value() const {
        if (m_res < 0) {
            auto ec = error_code();
            // fmt::println(stderr, "{}", ec.message());
            throw std::system_error(ec);
        }
        // assert(m_res >= 0);
        return m_res;
    }

    // 直接返回底层值，不检查错误。
    T raw_value() const {
        return m_res;
    }
};

// 把系统调用返回值 -1 转成 -errno，其余值原样返回。
template <class U, class T>
expected<U> convert_error(T res) {
    if (res == -1) {
        return -errno;
    }
    return res;
}

// 显式指定返回类型的版本，供需要 size_t/int 等具体类型的调用点使用。
template <int = 0, class T>
expected<T> convert_error(T res) {
    if (res == -1) {
        return -errno;
    }
    return res;
}
