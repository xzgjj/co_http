#!/bin/bash
# P0 验收脚本：默认构建跑一遍；再用 ASan/UBSan 构建跑一遍（同一套黑盒用例）。
# 用法：bash tests/run_p0.sh [--no-asan]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

echo "== [1/3] Release 构建 =="
cmake -B build -DCMAKE_BUILD_TYPE=Release > /tmp/p0-cmake.log 2>&1 || { tail -20 /tmp/p0-cmake.log; exit 1; }
cmake --build build --target chat_server -j > /tmp/p0-build.log 2>&1 || { tail -40 /tmp/p0-build.log; exit 1; }

echo "== [2/3] 黑盒用例（Release）=="
python3 tests/test_p0_stability.py -v 2>&1 | tail -20
rc=${PIPESTATUS[0]}

if [ "${1:-}" = "--no-asan" ]; then
    echo "== 跳过 ASan =="
    exit "$rc"
fi

echo "== [3/3] 黑盒用例（ASan + UBSan）=="
ASAN_DIR=/tmp/co_http-asan
cmake -B "$ASAN_DIR" -S . -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" > /tmp/p0-cmake-asan.log 2>&1 || { tail -20 /tmp/p0-cmake-asan.log; exit 1; }
cmake --build "$ASAN_DIR" --target chat_server -j > /tmp/p0-build-asan.log 2>&1 || { tail -40 /tmp/p0-build-asan.log; exit 1; }
CHAT_SERVER_BIN="$ASAN_DIR/chat_server" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
    python3 tests/test_p0_stability.py -v 2>&1 | tail -20
rc2=${PIPESTATUS[0]}

if [ "$rc" -eq 0 ] && [ "$rc2" -eq 0 ]; then
    echo "P0 验收通过"
    exit 0
fi
echo "P0 验收失败（release=$rc asan=$rc2）"
exit 1
