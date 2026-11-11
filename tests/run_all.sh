#!/bin/bash
# 全套本地测试（回调版 + 协程版 + 多进程实验）。
# 集成入口仍是 `python3 tools/ci.py ci`（含构建/ASan/压测）。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

fail=0
run_suite() {
    local name="$1"
    shift
    local out rc
    out=$("$@" 2>&1)
    rc=$?
    echo "== ${name} rc=${rc}"
    echo "$out" | tail -3
    if [ "$rc" -ne 0 ]; then
        echo "$out" | tail -40
        fail=1
    fi
}

run_suite "P0-callback"        python3 tests/test_p0_stability.py
run_suite "P1-callback"        python3 tests/test_p1_protocol.py
run_suite "P3-callback"        python3 tests/test_p3_delivery.py
run_suite "P4-persistence"     python3 tests/test_p4_persistence.py
run_suite "P4-realtime"         python3 tests/test_p4_realtime.py
run_suite "P5-frontend"         python3 tests/test_p5_frontend.py
run_suite "P5-tls"             python3 tests/test_p5_tls.py
run_suite "P0-coroutine"       env CHAT_SERVER_BIN="$ROOT/build/chat_server_co" python3 tests/test_p0_stability.py
run_suite "P1-coroutine"       env CHAT_SERVER_BIN="$ROOT/build/chat_server_co" python3 tests/test_p1_protocol.py
run_suite "P3-coroutine"       env CHAT_SERVER_BIN="$ROOT/build/chat_server_co" python3 tests/test_p3_delivery.py
run_suite "P4-persistence-co"  env CHAT_SERVER_BIN="$ROOT/build/chat_server_co" python3 tests/test_p4_persistence.py
run_suite "P4-realtime-co"      env CHAT_SERVER_BIN="$ROOT/build/chat_server_co" python3 tests/test_p4_realtime.py
run_suite "P5-frontend-co"      env CHAT_SERVER_BIN="$ROOT/build/chat_server_co" python3 tests/test_p5_frontend.py
run_suite "P5-tls-co"          env CHAT_SERVER_BIN="$ROOT/build/chat_server_co" python3 tests/test_p5_tls.py
run_suite "P2-selftest"        ./build/chat_server_co --selftest
run_suite "P4-multiprocess"    python3 tests/test_p4_multiprocess.py

echo "=================================="
if [ "$fail" -eq 0 ]; then
    echo "ALL SUITES PASSED"
    exit 0
fi
echo "SUITE FAILURES PRESENT"
exit 1
