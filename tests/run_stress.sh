#!/bin/bash
# 压测两版并打印 JSON（CI 之外手工复现用）。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

echo "=== callback ==="
python3 tests/stress.py --conns 20 --duration 3
rc1=$?

echo "=== coroutine ==="
CHAT_SERVER_BIN="$ROOT/build/chat_server_co" python3 tests/stress.py --conns 20 --duration 3
rc2=$?

if [ "$rc1" -eq 0 ] && [ "$rc2" -eq 0 ]; then
    echo "stress: OK"
    exit 0
fi
echo "stress: FAILED (callback=$rc1 coroutine=$rc2)"
exit 1
