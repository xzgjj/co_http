"""P3 交付质量测试（静态缓存 / 304 / gzip / 健康检查 / 指标）。

运行（默认回调版）：python3 tests/test_p3_delivery.py -v
协程版：CHAT_SERVER_BIN=$PWD/build/chat_server_co python3 tests/test_p3_delivery.py -v
"""
import gzip
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402


class DeliveryBase(unittest.TestCase):
    def setUp(self):
        self.srv = harness.Server()
        self.srv.wait_ready()
        self.addCleanup(lambda: self.srv.alive() and self.srv.stop())

    def raw(self, request_bytes, timeout=3.0, expect_body=True):
        s = harness.connect(self.srv.port, timeout)
        try:
            s.sendall(request_bytes)
            return harness.read_response_ex(s, timeout, expect_body)
        finally:
            s.close()

    def get(self, path=b"/", extra=b"", expect_body=True):
        return self.raw(
            b"GET " + path + b" HTTP/1.1\r\nHost: x\r\n" + extra + b"\r\n",
            expect_body=expect_body,
        )


class TestCache(DeliveryBase):
    def test_etag_and_304(self):
        status, headers, body, _ = self.get()
        self.assertEqual(status, 200)
        self.assertIn(b"etag", headers)
        self.assertIn(b"cache-control", headers)
        etag = headers[b"etag"]

        status2, headers2, body2, _ = self.get(extra=b"If-None-Match: " + etag + b"\r\n")
        self.assertEqual(status2, 304)
        self.assertEqual(body2, b"")
        self.assertEqual(headers2[b"etag"], etag)

    def test_if_none_match_wildcard(self):
        status, _, _, _ = self.get(extra=b"If-None-Match: *\r\n")
        self.assertEqual(status, 304)

    def test_last_modified_present(self):
        _, headers, _, _ = self.get(expect_body=False)
        self.assertIn(b"last-modified", headers)
        self.assertTrue(headers[b"last-modified"].endswith(b"GMT"))


class TestCompression(DeliveryBase):
    def test_gzip_when_accepted(self):
        _, plain_headers, plain_body, _ = self.get()
        status, headers, body, _ = self.get(extra=b"Accept-Encoding: gzip\r\n")
        self.assertEqual(status, 200)
        self.assertEqual(headers[b"content-encoding"], b"gzip")
        self.assertEqual(int(headers[b"content-length"]), len(body))
        self.assertEqual(gzip.decompress(body), plain_body)
        self.assertLess(len(body), len(plain_body))

    def test_no_gzip_when_not_accepted(self):
        _, headers, _, _ = self.get()
        self.assertNotIn(b"content-encoding", headers)

    def test_304_before_compression(self):
        _, headers, _, _ = self.get(expect_body=False)
        etag = headers[b"etag"]
        status, headers2, body2, _ = self.get(
            extra=b"Accept-Encoding: gzip\r\nIf-None-Match: " + etag + b"\r\n",
            expect_body=False,
        )
        self.assertEqual(status, 304)
        self.assertEqual(body2, b"")


class TestObservability(DeliveryBase):
    def test_healthz(self):
        status, _, body, _ = self.get(b"/healthz")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"ok")

    def test_metrics_json_and_counters(self):
        before = json.loads(self.get(b"/metrics")[2])
        for _ in range(3):
            self.get(b"/healthz")
        after = json.loads(self.get(b"/metrics")[2])
        self.assertGreaterEqual(after["requests_total"], before["requests_total"] + 3)
        self.assertIn("active_connections", after)
        self.assertIn("uptime_s", after)

    def test_access_static_404_has_cache_headers_absent(self):
        status, headers, _, _ = self.get(b"/no-such-file")
        self.assertEqual(status, 404)
        self.assertNotIn(b"etag", headers)


class TestSyscallFastPaths(DeliveryBase):
    """sendfile 静态直传 + writev 响应头/正文合并（两版共用实现）。"""

    def test_sendfile_serves_exact_file_bytes(self):
        status, headers, body, _ = self.get(b"/favicon.ico")
        self.assertEqual(status, 200)
        expected_path = os.path.join(harness.ROOT, "static", "favicon.ico")
        with open(expected_path, "rb") as handle:
            expected = handle.read()
        self.assertEqual(len(body), len(expected))
        self.assertEqual(body, expected)

    def test_sendfile_and_writev_counters(self):
        before = json.loads(self.get(b"/metrics")[2])
        self.get(b"/favicon.ico")  # 未压缩静态 → sendfile
        self.get(b"/healthz")      # 动态响应 → writev
        after = json.loads(self.get(b"/metrics")[2])
        self.assertGreater(after["sendfile_total"], before["sendfile_total"])
        self.assertGreater(after["writev_total"], before["writev_total"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
