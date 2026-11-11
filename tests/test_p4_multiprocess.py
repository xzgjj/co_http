"""P4 多进程实验：N 个子进程 SO_REUSEPORT 共享端口；父进程退出不遗留孤儿。"""
import os
import signal
import socket
import subprocess
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402

BIN = os.path.join(harness.BUILD, "chat_server_mp")
PORT = 8080


def kill_strays():
    subprocess.run(["pkill", "-f", "build/chat_server_mp"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class TestMultiProcess(unittest.TestCase):
    def setUp(self):
        kill_strays()
        time.sleep(0.1)
        self.proc = subprocess.Popen(
            [BIN, "--workers", "2"],
            cwd=harness.BUILD,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.addCleanup(self._cleanup)
        self._wait_ready()

    def _cleanup(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        kill_strays()

    def _wait_ready(self, timeout=6.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                out = self.proc.stdout.read() if self.proc.stdout else ""
                self.fail("mp parent exited early rc=%r: %s"
                          % (self.proc.returncode, out[-2000:]))
            try:
                s = socket.create_connection(("127.0.0.1", PORT), timeout=0.3)
                s.close()
                return
            except OSError:
                time.sleep(0.03)
        self.fail("mp server not ready")

    def healthz(self):
        s = harness.connect(PORT, timeout=3)
        try:
            s.sendall(b"GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n")
            status, headers, body, _ = harness.read_response_ex(s)
            return status, headers, body
        finally:
            s.close()

    def test_workers_share_port_and_parent_cleans_up(self):
        status, headers, body = self.healthz()
        self.assertEqual(status, 200)
        self.assertEqual(body, b"ok")
        self.assertIn(b"x-pid", headers)

        pids = set()
        for _ in range(40):
            status, headers, _ = self.healthz()
            self.assertEqual(status, 200)
            pids.add(headers[b"x-pid"])
        self.assertGreaterEqual(
            len(pids), 2, "SO_REUSEPORT 未分到多个进程: %r" % (pids,)
        )

        self.proc.send_signal(signal.SIGTERM)
        rc = self.proc.wait(timeout=5)
        self.assertEqual(rc, 0)
        time.sleep(0.3)
        out = subprocess.run(
            ["pgrep", "-f", "build/chat_server_mp"], capture_output=True, text=True
        )
        self.assertEqual(out.stdout.strip(), "",
                         "父进程退出后仍有子进程: %s" % out.stdout)

    def test_static_and_api_work_via_workers(self):
        status, headers, body = harness.get(PORT, b"/")
        self.assertEqual(status, 200)
        self.assertTrue(body.lower().startswith(b"<!doctype html>"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
