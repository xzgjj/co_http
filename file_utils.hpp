// 静态文件读写工具，路径相对于进程当前工作目录。
// 打开失败时抛 system_error；聊天服务器用它读取 static/ 下的页面和图标。
#pragma once

#include <fstream>
#include <string>
#include <string_view>
#include <algorithm>

// 读取整个文件；打开失败抛 system_error。
inline std::string file_get_content(std::string const &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::system_error(errno, std::generic_category());
    }
    std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    return content;
}

// 写入整个文件；打开失败抛 system_error。
inline void file_put_content(std::string const &path, std::string_view content) {
    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::system_error(errno, std::generic_category());
    }
    std::copy(content.begin(), content.end(), std::ostreambuf_iterator<char>(file));
}
