"""P0 稳定性回归测试（黑盒，WSL 下运行）。

每个用例只通过 TCP/HTTP 外部行为断言。运行：
    cd <repo> && python3 tests/test_p0_stability.py -v
"""
import os
import socket
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402


def start(**kw):
    srv = harness.Server(**kw)
    srv.wait_ready()
    return srv


class P0TestBase(unittest.TestCase):
    env = None

    def setUp(self):
        self.srv = start(env=self.env)
        self.addCleanup(self._cleanup)

    def _cleanup(self):
        if self.srv.alive():
            self.srv.stop()

    def assert_alive(self):
        if not self.srv.alive():
            self.fail(
                "server died (rc=%r):\n%s"
                % (self.srv.returncode(), self.srv._read_output()[-3000:])
            )

    def assert_serves(self):
        status, _, _ = harness.get(self.srv.port, timeout=3.0)
        self.assertEqual(status, 200, "server no longer serves GET /")


class TestResetCrash(P0TestBase):
    """P0-1：取消/未挂操作的 fd 收到 RST/HUP 时不得崩溃。"""

    def test_rst_during_long_poll_keeps_server_alive(self):
        for i in range(10):
            if not self.srv.alive():
                self.fail(
                    "server died after %d RST(s) (rc=%r):\n%s"
                    % (i, self.srv.returncode(), self.srv._read_output()[-3000:])
                )
            try:
                s = harness.connect(self.srv.port)
            except OSError as exc:
                self.fail("connect failed after %d RST(s): %r; alive=%r" % (i, exc, self.srv.alive()))
            body = b'{"first":1}'  # 1 = 只等新消息，走长轮询挂起
            s.sendall(
                b"POST /recv HTTP/1.1\r\nHost: x\r\nContent-Length: "
                + str(len(body)).encode() + b"\r\n\r\n" + body
            )
            time.sleep(0.05)  # 让服务器读完请求、进入长轮询
            harness.rst_close(s)  # 对端 RST
            time.sleep(0.01)
        time.sleep(0.3)
        self.assert_alive()
        self.assert_serves()


class TestExceptionBoundary(P0TestBase):
    """P0-3：业务异常不得逃出事件循环。"""

    def test_bad_json_send_returns_400(self):
        status, _, body = harness.post(self.srv.port, "/send", b"not-json")
        self.assertIsNotNone(status, "connection closed without response")
        self.assertEqual(status, 400)
        self.assert_alive()

    def test_bad_json_recv_returns_400(self):
        status, _, _ = harness.post(self.srv.port, "/recv", b"{oops")
        self.assertIsNotNone(status, "connection closed without response")
        self.assertEqual(status, 400)
        self.assert_alive()

    def test_bad_long_poll_cursor_returns_400(self):
        status, _, _ = harness.post(self.srv.port, "/recv", b'{"first":999999}')
        self.assertIsNotNone(status, "connection closed without response")
        self.assertEqual(status, 400)
        self.assert_alive()


class TestLimits(P0TestBase):
    """P0-5：头部/URL/正文上限返回正确状态码。"""

    def test_big_header_returns_431(self):
        big = b"GET / HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"A" * (20 * 1024) + b"\r\n\r\n"
        status, _, _ = harness.request(self.srv.port, big)
        self.assertIsNotNone(status, "connection closed without response")
        self.assertEqual(status, 431)
        self.assert_alive()

    def test_big_url_returns_414(self):
        big = b"GET /" + b"a" * (12 * 1024) + b" HTTP/1.1\r\nHost: x\r\n\r\n"
        status, _, _ = harness.request(self.srv.port, big)
        self.assertIsNotNone(status, "connection closed without response")
        self.assertEqual(status, 414)
        self.assert_alive()

    def test_big_declared_body_returns_413(self):
        s = harness.connect(self.srv.port)
        try:
            s.sendall(
                b"POST /send HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n"
                + b"{" + b"a" * 100
            )
            status, _, _ = harness.read_response(s, timeout=3.0)
        except socket.timeout:
            self.fail("server waited for a 2MB body instead of rejecting with 413")
        finally:
            s.close()
        self.assertIsNotNone(status, "connection closed without response")
        self.assertEqual(status, 413)
        self.assert_alive()


class TestSlowloris(P0TestBase):
    """P0-5：整请求 deadline（不随每次 read 重置）。"""

    env = {"REQUEST_TIMEOUT_MS": "800"}

    def test_slow_header_connection_is_closed(self):
        s = harness.connect(self.srv.port)
        try:
            s.sendall(b"GET / HTT")  # 只发半个请求头，不再继续
            s.settimeout(3.0)
            closed = False
            deadline = time.time() + 2.5
            while time.time() < deadline:
                try:
                    data = s.recv(4096)
                except socket.timeout:
                    continue
                if not data:
                    closed = True
                    break
                if b"HTTP/1.1" in data:
                    closed = True  # 服务器回了 408/错误响应后关闭
                    break
        finally:
            s.close()
        self.assertTrue(closed, "slowloris connection stayed open past request deadline")
        self.assert_alive()


class TestLongPollBroadcast(P0TestBase):
    """P0-6：一条新消息必须唤醒所有挂起的长轮询，而不是只唤醒最后一个。"""

    def test_send_wakes_all_waiters_quickly(self):
        socks = []
        try:
            for _ in range(3):
                s = harness.connect(self.srv.port)
                body = b'{"first":1}'
                s.sendall(
                    b"POST /recv HTTP/1.1\r\nHost: x\r\nContent-Length: "
                    + str(len(body)).encode() + b"\r\n\r\n" + body
                )
                socks.append(s)
            time.sleep(0.1)  # 三个 /recv 都进入长轮询
            t0 = time.time()
            harness.post(self.srv.port, "/send", b'{"user":"u","content":"hello"}')
            for i, s in enumerate(socks):
                status, _, _ = harness.read_response(s, timeout=1.0)
                elapsed = time.time() - t0
                self.assertEqual(status, 200, "waiter %d got no response" % i)
                self.assertLess(elapsed, 1.0, "waiter %d woke only after %.2fs" % (i, elapsed))
        finally:
            for s in socks:
                s.close()
        self.assert_alive()


class TestSignals(P0TestBase):
    """P0-4/P0-7：SIGPIPE 不杀进程；SIGTERM 优雅退出（退出码 0）。"""

    def test_write_after_peer_reset_does_not_kill_process(self):
        for _ in range(20):
            s = harness.connect(self.srv.port)
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            harness.rst_close(s)  # 服务器写响应时对端已 RST
            time.sleep(0.005)
        time.sleep(0.3)
        self.assert_alive()

    def test_sigterm_exits_cleanly(self):
        rc = self.srv.stop(timeout=3.0)
        self.assertEqual(rc, 0, "SIGTERM should exit 0, got %r" % (rc,))


class TestResourceExhaustion(unittest.TestCase):
    """P0-3/P0-5：fd 耗尽（EMFILE）时不得抛异常退出。"""

    def test_emfile_keeps_server_alive(self):
        srv = harness.Server(nofile=64)
        srv.wait_ready()
        socks = []
        try:
            for _ in range(120):
                try:
                    socks.append(harness.connect(srv.port, timeout=1.0))
                except OSError:
                    break
            time.sleep(0.3)
            # 注意：先判断再读日志——_read_output 在进程仍存活时会 kill 它。
            if not srv.alive():
                self.fail(
                    "server died under EMFILE (rc=%r):\n%s"
                    % (srv.returncode(), srv._read_output()[-3000:])
                )
        finally:
            for s in socks:
                s.close()
            time.sleep(0.2)
            alive_after = srv.alive()
            detail = ""
            if not alive_after:
                detail = "rc=%r\n%s" % (srv.returncode(), srv._read_output()[-3000:])
            srv.stop()
        self.assertTrue(alive_after, "server died after EMFILE pressure\n%s" % detail)


class TestKeepAliveSanity(P0TestBase):
    """回归护栏：正常 keep-alive 两次请求都应成功。"""

    def test_two_requests_on_one_connection(self):
        s = harness.connect(self.srv.port)
        try:
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            status1, _, _ = harness.read_response(s)
            s.sendall(b"GET /favicon.ico HTTP/1.1\r\nHost: x\r\n\r\n")
            status2, _, _ = harness.read_response(s)
        finally:
            s.close()
        self.assertEqual(status1, 200)
        self.assertEqual(status2, 200)


if __name__ == "__main__":
    unittest.main(verbosity=2)
