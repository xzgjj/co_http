#!/usr/bin/env python3
"""统一协调：构建 / 单元测试 / 集成测试 / ASan / 压测 / CI。

用法：
    python3 tools/ci.py build      # Release 构建
    python3 tools/ci.py test       # ctest + P0/P1/P3（回调版 + 协程版）
    python3 tools/ci.py asan       # ASan/UBSan 构建并跑同一套黑盒用例
    python3 tools/ci.py stress     # 连接与压测（两版）
    python3 tools/ci.py ci         # 以上全部（CI 入口）

任何一步失败则返回非 0；输出末尾给出步骤汇总。
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
ASAN_BUILD = "/tmp/co_http_asan"

PY_TESTS = [
    "tests/test_p0_stability.py",
    "tests/test_p1_protocol.py",
    "tests/test_p3_delivery.py",
    "tests/test_p4_persistence.py",
    "tests/test_p4_realtime.py",
    "tests/test_p5_frontend.py",
    "tests/test_p5_tls.py",
]


def run(cmd, env=None, tail=2500):
    printable = " ".join(cmd)
    print("+ " + printable, flush=True)
    result = subprocess.run(
        cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    output = result.stdout or ""
    if result.returncode != 0:
        print(output[-tail:], flush=True)
    return result.returncode


def cmake_build(build_dir, targets=None, extra_args=None, env=None):
    configure = ["cmake", "-B", build_dir, "-S", ROOT, "-DCMAKE_BUILD_TYPE=Release"]
    if extra_args:
        configure += extra_args
    if run(configure, env=env) != 0:
        return False
    build = ["cmake", "--build", build_dir, "-j"]
    if targets:
        build += ["--target"] + targets
    return run(build, env=env) == 0


def step_build():
    return cmake_build(BUILD)


def step_unit():
    return run(["ctest", "--test-dir", BUILD, "--output-on-failure"]) == 0


def step_pytests(binary=None, env_extra=None):
    env = dict(os.environ)
    if binary:
        env["CHAT_SERVER_BIN"] = binary
    if env_extra:
        env.update(env_extra)
    ok = True
    for test in PY_TESTS:
        ok = run([sys.executable, test, "-v"], env=env) == 0 and ok
    return ok


def step_selftest(binary):
    return run([binary, "--selftest"]) == 0


def _tail_only(env):
    return env


def step_test():
    ok = step_unit()
    callback = os.path.join(BUILD, "chat_server")
    co = os.path.join(BUILD, "chat_server_co")
    ok = step_pytests(callback) and ok
    ok = step_selftest(co) and ok
    ok = step_pytests(co) and ok
    ok = run([sys.executable, "tests/test_p4_multiprocess.py", "-v"]) == 0 and ok
    return ok


def step_asan():
    if not cmake_build(
        ASAN_BUILD,
        targets=["chat_server", "chat_server_co", "chat_server_mp"],
        extra_args=[
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g",
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined",
        ],
    ):
        return False
    env = {"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1"}
    ok = step_pytests(os.path.join(ASAN_BUILD, "chat_server"), env)
    ok = run([os.path.join(ASAN_BUILD, "chat_server_co"), "--selftest"],
             env={**os.environ, **env}) == 0 and ok
    ok = step_pytests(os.path.join(ASAN_BUILD, "chat_server_co"), env) and ok
    return ok


def step_stress():
    ok = run([sys.executable, "tests/stress.py", "--conns", "20", "--duration", "3"]) == 0
    env = dict(os.environ)
    env["CHAT_SERVER_BIN"] = os.path.join(BUILD, "chat_server_co")
    ok = run([sys.executable, "tests/stress.py", "--conns", "20", "--duration", "3"],
             env=env) == 0 and ok
    return ok


STEPS = {
    "build": step_build,
    "test": step_test,
    "asan": step_asan,
    "stress": step_stress,
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["build", "test", "asan", "stress", "ci"],
                        nargs="?", default="ci")
    args = parser.parse_args()

    if args.command == "ci":
        order = ["build", "test", "asan", "stress"]
    else:
        order = [args.command]

    results = {}
    for name in order:
        print("\n===== %s =====" % name, flush=True)
        results[name] = STEPS[name]()

    print("\n===== summary =====")
    for name in order:
        print("%-8s %s" % (name, "OK" if results[name] else "FAILED"))
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
