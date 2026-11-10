// HTTP 头与 JSON 对象共用的字符串映射类型。
// 使用有序 map，保证遍历和序列化顺序稳定；头键在解析时统一转成小写。
#pragma once

#include <string>
#include <map>

using string_map = std::map<std::string, std::string>; // 键有序，输出可复现
