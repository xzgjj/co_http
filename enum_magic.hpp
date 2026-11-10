// 编译期枚举名工具：从 __PRETTY_FUNCTION__ 里提取枚举类型名和枚举值名。
// 供 HTTP 方法解析使用；假定枚举值从 0 连续、最大值小于 256，仅支持 GCC/Clang。
#pragma once

#include <cstddef>
#include <string_view>
#include <utility>

// 从 pretty function 字符串里截取 "CrItMaGiC = " 之后、';' 或 ']' 之前的部分。
constexpr std::string_view _try_extract_value(std::string_view str) {
    auto critpos = str.find("CrItMaGiC = ") + 12;
    auto endpos = str.find_first_of(";]");
    auto slice = str.substr(critpos, endpos - critpos);
    return slice;
}

// 去掉 "类型名::" 前缀；以 '(' 开头说明该整数不是有效枚举值。
constexpr std::string_view _try_remove_prefix(std::string_view str, std::string_view prefix) {
    if (!str.empty()) {
        if (str.front() == '(') {
            return {}; // GCC 对越界枚举会打印成 (类型)数值
        }
        if (str.find(prefix) == 0 && str.find("::", prefix.size(), 2) == prefix.size()) {
            return str.substr(prefix.size() + 2);
        }
    }
    return str;
}

// 提取枚举类型名。
template <class CrItMaGiC>
constexpr std::string_view _enum_type_name() {
    constexpr std::string_view name = _try_extract_value(__PRETTY_FUNCTION__);
    return name;
}

// 提取某个枚举值的名字；无效值返回空串。
template <auto CrItMaGiC>
constexpr std::string_view _enum_value_name() {
    constexpr auto type = _enum_type_name<decltype(CrItMaGiC)>();
    constexpr std::string_view name = _try_remove_prefix(_try_extract_value(__PRETTY_FUNCTION__), type);
    return name;
}

#if __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wenum-constexpr-conversion"
#endif
// 二分查找有效枚举值的上界，结果作为 index_sequence 的长度。
template <class E, size_t I0, size_t I1>
constexpr size_t _enum_range_impl() {
    if constexpr (I0 + 1 == I1) {
        return I1;
    } else {
        constexpr size_t I = (I0 + I1) >> 1;
        if constexpr (!_enum_value_name<static_cast<E>(I)>().empty()) {
            return _enum_range_impl<E, I, I1>();
        } else {
            return _enum_range_impl<E, I0, I>();
        }
    }
}

// 计算枚举值范围，默认按 0..255 搜索。
template <class E>
constexpr size_t _enum_range() {
    return _enum_range_impl<E, 0, 256>();
}
#if __clang__
#pragma clang diagnostic pop
#endif

// 遍历 0..N-1，把数值匹配到枚举名。
template <class E, size_t ...Is>
constexpr std::string_view _dump_enum_impl(E value, std::index_sequence<Is...>) {
    std::string_view ret;
    (void)((value == static_cast<E>(Is) && ((ret = _enum_value_name<static_cast<E>(Is)>()), false)) || ...);
    return ret;
}

// 枚举值转名字；找不到时返回空串。
template <class E>
constexpr std::string_view dump_enum(E value) {
    return _dump_enum_impl(value, std::make_index_sequence<_enum_range<E>()>());
}

// 遍历 0..N-1，把枚举名匹配回数值。
template <class E, size_t ...Is>
constexpr E _parse_enum_impl(std::string_view name, std::index_sequence<Is...>) {
    size_t ret = static_cast<size_t>(-1);
    (void)((name == _enum_value_name<static_cast<E>(Is)>() && (ret = Is)) || ...);
    return static_cast<E>(ret);
}

// 名字转枚举值；找不到时返回 static_cast<E>(-1)。
template <class E>
constexpr E parse_enum(std::string_view name) {
    return _parse_enum_impl<E>(name, std::make_index_sequence<_enum_range<E>()>());
}
