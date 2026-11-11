#!/bin/bash
# P2 验收：协程版功能等价（同一套 P0 + P1 黑盒用例跑在 chat_server_co 上）。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

echo "== 构建 =="
cmake -B build -DCMAKE_BUILD_TYPE=Release > /tmp/p2-cmake.log 2>&1 || { tail -20 /tmp/p2-cmake.log; exit 1; }
cmake --build build --target chat_server chat_server_co codec_tests -j > /tmp/p2-build.log 2>&1 || { tail -40 /tmp/p2-build.log; exit 1; }

echo "== 协程自检（Task / sleep / work-stealing）=="
./build/chat_server_co --selftest || exit 1

echo "== P0 用例（协程版）=="
CHAT_SERVER_BIN="$ROOT/build/chat_server_co" python3 tests/test_p0_stability.py 2>&1 | tail -4
p0=${PIPESTATUS[0]}

echo "== P1 用例（协程版）=="
CHAT_SERVER_BIN="$ROOT/build/chat_server_co" python3 tests/test_p1_protocol.py 2>&1 | tail -4
p1=${PIPESTATUS[0]}

echo "== P3 用例（协程版）=="
CHAT_SERVER_BIN="$ROOT/build/chat_server_co" python3 tests/test_p3_delivery.py 2>&1 | tail -4
p3=${PIPESTATUS[0]}

if [ "$p0" -eq 0 ] && [ "$p1" -eq 0 ] && [ "$p3" -eq 0 ]; then
    echo "P2 验收通过"
    exit 0
fi
echo "P2 验收失败（p0=$p0 p1=$p1 p3=$p3）"
exit 1
