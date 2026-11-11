#!/usr/bin/env python3
"""连接与压测（P3 验收）：keep-alive 并发吞吐 + 长轮询广播。

用法：
    python3 tests/stress.py [--conns 20] [--duration 3] [--min-rps 100]
默认二进制 build/chat_server，可用 CHAT_SERVER_BIN 指向协程版。
失败（错误>0 / 低于阈值 / 服务掉线）返回非 0。
"""
import argparse
import asyncio
import json
import os
import socket
import statistics
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402

REQUEST = b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"


async def load_worker(port, duration, counters):
    try:
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
    except OSError:
        counters["errors"] += 1
        return
    end = time.perf_counter() + duration
    latencies = []
    try:
        while time.perf_counter() < end:
            started = time.perf_counter()
            writer.write(REQUEST)
            await writer.drain()
            head = await reader.readuntil(b"\r\n\r\n")
            try:
                status = int(head.split(b" ")[1])
            except (IndexError, ValueError):
                status = 0
            length = 0
            for line in head.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":", 1)[1])
            if length:
                await reader.readexactly(length)
            latencies.append(time.perf_counter() - started)
            counters["ok"] += 1
            if status != 200:
                counters["errors"] += 1
    except (OSError, asyncio.IncompleteReadError):
        counters["errors"] += 1
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except OSError:
            pass
        counters["latencies"].extend(latencies)


async def load_test(port, conns, duration):
    counters = {"ok": 0, "errors": 0, "latencies": []}
    tasks = [
        asyncio.create_task(load_worker(port, duration, counters))
        for _ in range(conns)
    ]
    await asyncio.gather(*tasks)
    return counters


def long_poll_test(port, clients=10):
    """clients 个 /recv 长轮询挂起 → 一条 /send → 统计唤醒延迟。"""
    results = []
    lock = threading.Lock()

    def waiter():
        try:
            body = b'{"first":1}'
            s = socket.create_connection(("127.0.0.1", port), timeout=3)
            s.sendall(
                b"POST /recv HTTP/1.1\r\nHost: x\r\nContent-Length: "
                + str(len(body)).encode()
                + b"\r\n\r\n"
                + body
            )
            started = time.perf_counter()
            s.settimeout(4)
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            with lock:
                results.append(time.perf_counter() - started)
            s.close()
        except OSError:
            with lock:
                results.append(None)

    threads = [threading.Thread(target=waiter) for _ in range(clients)]
    for t in threads:
        t.start()
    time.sleep(0.2)

    body = b'{"user":"stress","content":"wake"}'
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
    s.sendall(
        b"POST /send HTTP/1.1\r\nHost: x\r\nContent-Length: "
        + str(len(body)).encode()
        + b"\r\n\r\n"
        + body
    )
    s.settimeout(2)
    try:
        s.recv(4096)
    except OSError:
        pass
    s.close()

    for t in threads:
        t.join(timeout=4)
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--conns", type=int, default=20)
    parser.add_argument("--duration", type=float, default=3.0)
    parser.add_argument("--min-rps", type=float, default=100.0)
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    import shutil
    import tempfile

    # WSL 的 /mnt/d 是 DrvFs：sendfile 在该文件系统上明显更慢（实测 4.5k vs ext4 26k rps）。
    # 本地压测把静态文件复制到原生文件系统，避免把文件系统差异误判为服务端性能。
    static_root = os.environ.get("STRESS_STATIC_ROOT")
    temp_static = None
    if not static_root and harness.ROOT.startswith("/mnt/"):
        temp_static = tempfile.mkdtemp(prefix="co_http_static_")
        shutil.copytree(os.path.join(harness.ROOT, "static"),
                        os.path.join(temp_static, "static"))
        static_root = os.path.join(temp_static, "static")

    env = {}
    if static_root:
        env["STATIC_ROOT"] = static_root

    logfile = tempfile.NamedTemporaryFile(
        prefix="co_http_stress_", suffix=".log", delete=False
    ).name
    srv = harness.Server(port=args.port, logfile=logfile, env=env)
    srv.wait_ready()
    summary = {}
    ok = True
    try:
        counters = asyncio.run(load_test(args.port, args.conns, args.duration))
        lat = counters["latencies"]
        rps = counters["ok"] / args.duration
        p99 = (
            statistics.quantiles(lat, n=100)[98]
            if len(lat) >= 100
            else (max(lat) if lat else 0.0)
        )
        summary["load"] = {
            "conns": args.conns,
            "duration_s": args.duration,
            "requests": counters["ok"],
            "errors": counters["errors"],
            "rps": round(rps, 1),
            "p50_ms": round(statistics.median(lat) * 1000, 2) if lat else None,
            "p99_ms": round(p99 * 1000, 2),
        }
        ok &= counters["errors"] == 0 and counters["ok"] > 0
        ok &= rps >= args.min_rps
        ok &= p99 < 1.0

        wake = long_poll_test(args.port, clients=10)
        wake_valid = [w for w in wake if w is not None]
        summary["long_poll"] = {
            "clients": len(wake),
            "woken": len(wake_valid),
            "max_wake_ms": round(max(wake_valid) * 1000, 2) if wake_valid else None,
        }
        ok &= len(wake_valid) == len(wake) and len(wake_valid) > 0
        ok &= max(wake_valid or [99]) < 1.0

        summary["server_alive"] = srv.alive()
        ok &= srv.alive()
    finally:
        srv.stop()
        if temp_static:
            shutil.rmtree(temp_static, ignore_errors=True)
        if not ok:
            try:
                with open(logfile, "r", errors="replace") as handle:
                    text = handle.read()
                print("--- server log tail ---")
                print(text[-3000:])
            except OSError:
                pass

    summary["ok"] = bool(ok)
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
