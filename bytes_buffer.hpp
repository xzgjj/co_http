// 字节视图与缓冲区：在协议解析和异步 I/O 之间传递“指针 + 长度”，避免无谓拷贝。spin
// bytes_buffer 拥有内存，bytes_view/bytes_const_view 只借用，使用期间必须保证底层存储有效。
#pragma once

#include <cstddef>
#include <string_view>
#include <stdexcept>
#include <vector>

struct bytes_const_view {//只读字节视图
    char const *m_data;
    size_t m_size;

    // 返回只读数据指针。
    char const *data() const noexcept {
        return m_data;
    }

    // 返回字节数。
    size_t size() const noexcept {
        return m_size;
    }

    // 返回只读区间起点。
    char const *begin() const noexcept {
        return data();
    }

    // 返回只读区间终点。
    char const *end() const noexcept {
        return data() + size();
    }

    // 取子区间：起点越界抛异常，长度超出剩余部分时自动截断。
    bytes_const_view subspan(size_t start,
                             size_t len = static_cast<size_t>(-1)) const {
        if (start > size()) {
            throw std::out_of_range("bytes_const_view::subspan"); // 起点不合法
        }
        if (len > size() - start) {
            len = size() - start; // 超长时截断到末尾
        }
        return {data() + start, len};
    }

    // 隐式转换为 std::string_view，方便和标准库接口对接。
    operator std::string_view() const noexcept {
        return std::string_view{data(), size()};
    }
};

struct bytes_view {//可写字节视图
    char *m_data;
    size_t m_size;

    // 返回可写数据指针。
    char *data() const noexcept {
        return m_data;
    }

    // 返回字节数。
    size_t size() const noexcept {
        return m_size;
    }

    // 返回可写区间起点。
    char *begin() const noexcept {
        return data();
    }

    // 返回可写区间终点。
    char *end() const noexcept {
        return data() + size();
    }

    // 取可写子区间：起点越界抛异常，长度超出剩余部分时自动截断。，逻辑与只读视图类似
    bytes_view subspan(size_t start, size_t len) const {
        if (start > size()) {
            throw std::out_of_range("bytes_view::subspan"); // 起点不合法
        }
        if (len > size() - start) {
            len = size() - start; // 超长时截断到末尾
        }
        return {data() + start, len};
    }

    // 可写视图隐式转为安全退化为只读视图。
    operator bytes_const_view() const noexcept {
        return bytes_const_view{data(), size()};
    }

    // 隐式转换为 std::string_view。
    operator std::string_view() const noexcept {
        return std::string_view{data(), size()};
    }
};

struct bytes_buffer {//动态拥有型缓冲区
    std::vector<char> m_data;

    
    bytes_buffer() = default;// 默认构造空缓冲区。
    bytes_buffer(bytes_buffer &&) = default;/// 移动构造：转移底层 vector。
    bytes_buffer &operator=(bytes_buffer &&) = default;// 移动赋值：转移底层 vector。

   
    explicit bytes_buffer(bytes_buffer const &) = default; // 拷贝构造要求显式调用，避免在异步链路中误拷贝大缓冲区。

    // 分配 n 个零字节；这是分配，不是 reserve。
    explicit bytes_buffer(size_t n) : m_data(n) {}

    // 返回只读数据指针。
    char const *data() const noexcept {
        return m_data.data();
    }

    // 返回可写数据指针。
    char *data() noexcept {
        return m_data.data();
    }

    // 返回当前字节数。
    size_t size() const noexcept {
        return m_data.size();
    }

    // 返回只读区间起点。
    char const *begin() const noexcept {
        return data();
    }

    // 返回可写区间起点。
    char *begin() noexcept {
        return data();
    }

    // 返回只读区间终点。
    char const *end() const noexcept {
        return data() + size();
    }

    // 返回可写区间终点。
    char *end() noexcept {
        return data() + size();
    }

    // 只读子区间，转发给 bytes_const_view。
    bytes_const_view subspan(size_t start, size_t len) const {
        return operator bytes_const_view().subspan(start, len);
    }

    // 可写子区间，转发给 bytes_view。
    bytes_view subspan(size_t start, size_t len) {
        return operator bytes_view().subspan(start, len);
    }

    // 转为只读视图。
    operator bytes_const_view() const noexcept {
        return bytes_const_view{m_data.data(), m_data.size()};
    }

    // 转为可写视图。
    operator bytes_view() noexcept {
        return bytes_view{m_data.data(), m_data.size()};
    }

    // 转为 std::string_view。
    operator std::string_view() const noexcept {
        return std::string_view{m_data.data(), m_data.size()};
    }

    // 追加一段字节；insert 可能扩容，之前的视图和指针全部失效。
    void append(bytes_const_view chunk) {
        m_data.insert(m_data.end(), chunk.begin(), chunk.end());
    }

    // 追加一段字符串字节；同样可能扩容并使旧视图失效。
    void append(std::string_view chunk) {
        m_data.insert(m_data.end(), chunk.begin(), chunk.end());
    }

    // 追加字符串字面量，N-1 用于去掉末尾的 '\0'。固定容量缓冲区
    template <size_t N>
    void append_literial(char const (&literial)[N]) {
        append(std::string_view{literial, N - 1});
    }

    // 清空内容但保留容量。
    void clear() {
        m_data.clear();
    }

    // 调整大小；缩小时保留容量，扩大时补零。
    void resize(size_t n) {
        m_data.resize(n);
    }

    // 预留容量，不改变当前大小。
    void reserve(size_t n) {
        m_data.reserve(n);
    }
};

template <size_t N>
struct static_bytes_buffer {
    std::array<char, N> m_data;

    // 返回只读数据指针。
    char const *data() const noexcept {
        return m_data.data();
    }

    // 返回可写数据指针。
    char *data() noexcept {
        return m_data.data();
    }

    // 返回固定容量。
    static constexpr size_t size() noexcept {
        return N;
    }

    // 转为只读视图。
    operator bytes_const_view() const noexcept {
        return bytes_const_view{m_data.data(), N};
    }

    // 转为可写视图。
    operator bytes_view() noexcept {
        return bytes_view{m_data.data(), N};
    }

    // 转为 std::string_view。
    operator std::string_view() const noexcept {
        return std::string_view{m_data.data(), m_data.size()};
    }
};
