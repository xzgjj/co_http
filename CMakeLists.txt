cmake_minimum_required(VERSION 3.12)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 17)

project(co_http VERSION 4.0 LANGUAGES CXX)

add_compile_options(-Wall -Wextra -Werror=return-type)

include(CheckSymbolExists)
check_symbol_exists(epoll_pwait2 "sys/epoll.h" HAS_epoll_pwait2)
if (HAS_epoll_pwait2)
    add_definitions(-DHAS_epoll_pwait2)
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

link_libraries(Threads::Threads ZLIB::ZLIB SQLite::SQLite3 OpenSSL::SSL OpenSSL::Crypto)
add_compile_definitions(CO_HTTP_VERSION="${PROJECT_VERSION}")
include_directories(include)

file(GLOB sources CONFIGURE_DEPENDS *.cpp)
foreach (source IN ITEMS ${sources})
    get_filename_component(name "${source}" NAME_WLE)
    if (name STREQUAL "chat_server_co")
        continue() # 协程版单独用 C++20 构建（见下）
    endif()
    add_executable(${name} ${name}.cpp)
endforeach()

# ---- 单元测试（快速、无网络，ctest 驱动） ----
enable_testing()

add_executable(codec_tests tests/unit/test_http_codec.cpp)
target_include_directories(codec_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME codec_tests COMMAND codec_tests)

add_executable(timer_tests tests/unit/test_timer_wheel.cpp)
target_include_directories(timer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME timer_tests COMMAND timer_tests)

add_executable(realtime_tests tests/unit/test_realtime.cpp)
target_include_directories(realtime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME realtime_tests COMMAND realtime_tests)

# ---- P2 协程版（C++20，仅该 target 开启，不改全局标准） ----
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/chat_server_co.cpp)
    add_executable(chat_server_co chat_server_co.cpp)
    set_target_properties(chat_server_co PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    add_test(NAME co_smoke COMMAND chat_server_co --selftest)
endif()

