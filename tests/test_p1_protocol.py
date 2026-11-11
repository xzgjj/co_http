"""P1 协议正确性集成测试（黑盒，WSL 下运行）。

覆盖：HEAD、method+path 路由、405/OPTIONS、query 拆分、标准 reason/Date、
Connection 协商、HTTP/1.0、pipelining、Host、Content-Length 严格化、chunked 501。

运行：cd <repo> && python3 tests/test_p1_protocol.py -v
"""
import email.utils
import os
import socket
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402


class P1Base(unittest.TestCase):
    def setUp(self):
        self.srv = harness.Server()
        self.srv.wait_ready()
        self.addCleanup(lambda: self.srv.alive() and self.srv.stop())

    def raw(self, data, timeout=3.0, expect_body=True):
        s = harness.connect(self.srv.port, timeout)
        try:
            s.sendall(data)
            return harness.read_response_ex(s, timeout, expect_body)
        finally:
            s.close()

    def assert_eof(self, sock, timeout=2.0):
        sock.settimeout(timeout)
        try:
            data = sock.recv(1)
        except socket.timeout:
            self.fail("connection stayed open (expected EOF)")
        self.assertEqual(data, b"", "expected EOF, got %r" % data)


class TestHead(P1Base):
    def test_head_returns_headers_without_body(self):
        status, headers, body, _ = self.raw(
            b"HEAD / HTTP/1.1\r\nHost: x\r\n\r\n", expect_body=False
        )
        self.assertEqual(status, 200)
        self.assertEqual(body, b"", "HEAD response must not carry a body")
        self.assertIn(b"content-length", headers)

    def test_head_content_length_matches_get(self):
        _, _, get_body, _ = self.raw(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        _, head_headers, _, _ = self.raw(
            b"HEAD / HTTP/1.1\r\nHost: x\r\n\r\n", expect_body=False
        )
        self.assertEqual(int(head_headers[b"content-length"]), len(get_body))

    def test_head_404_has_no_body(self):
        status, headers, body, _ = self.raw(
            b"HEAD /no-such-path HTTP/1.1\r\nHost: x\r\n\r\n", expect_body=False
        )
        self.assertEqual(status, 404)
        self.assertEqual(body, b"")


class TestRouting(P1Base):
    def test_get_on_post_endpoint_is_405(self):
        status, headers, body, _ = self.raw(
            b"GET /send HTTP/1.1\r\nHost: x\r\n\r\n"
        )
        self.assertEqual(status, 405)
        self.assertIn(b"allow", headers)
        self.assertIn(b"POST", headers[b"allow"])

    def test_post_on_index_is_405(self):
        status, headers, _, _ = self.raw(
            b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"
        )
        self.assertEqual(status, 405)
        self.assertIn(b"GET", headers[b"allow"])

    def test_options_lists_allow(self):
        status, headers, _, _ = self.raw(
            b"OPTIONS /send HTTP/1.1\r\nHost: x\r\n\r\n"
        )
        self.assertEqual(status, 200)
        allow = headers[b"allow"]
        self.assertIn(b"POST", allow)
        self.assertIn(b"OPTIONS", allow)

    def test_query_is_split_from_path(self):
        status, _, body, _ = self.raw(b"GET /?x=1&y=2 HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assertEqual(status, 200)
        self.assertTrue(body.lower().startswith(b"<!doctype html>"))  # index.html

    def test_404_reason_phrase(self):
        status, headers, _, head = self.raw(
            b"GET /nope HTTP/1.1\r\nHost: x\r\n\r\n"
        )
        self.assertEqual(status, 404)
        self.assertTrue(head.startswith(b"HTTP/1.1 404 Not Found"), head)


class TestHeadersAndConnections(P1Base):
    def test_date_header_present(self):
        _, headers, _, _ = self.raw(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assertIn(b"date", headers)
        parsed = email.utils.parsedate_to_datetime(headers[b"date"].decode())
        self.assertIsNotNone(parsed)

    def test_http10_default_close(self):
        s = harness.connect(self.srv.port)
        try:
            s.sendall(b"GET / HTTP/1.0\r\n\r\n")
            status, headers, _, _ = harness.read_response_ex(s)
            self.assertEqual(status, 200)
            self.assertEqual(headers[b"connection"].lower(), b"close")
            self.assert_eof(s)
        finally:
            s.close()

    def test_connection_close_honored(self):
        s = harness.connect(self.srv.port)
        try:
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            status, headers, _, _ = harness.read_response_ex(s)
            self.assertEqual(status, 200)
            self.assertEqual(headers[b"connection"].lower(), b"close")
            self.assert_eof(s)
        finally:
            s.close()

    def test_keep_alive_two_requests(self):
        s = harness.connect(self.srv.port)
        try:
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            status1, _, _, _ = harness.read_response_ex(s)
            s.sendall(b"GET /favicon.ico HTTP/1.1\r\nHost: x\r\n\r\n")
            status2, _, body2, _ = harness.read_response_ex(s)
            self.assertEqual(status1, 200)
            self.assertEqual(status2, 200)
            self.assertGreater(len(body2), 0)
        finally:
            s.close()


class TestPipelining(P1Base):
    def test_two_pipelined_gets(self):
        s = harness.connect(self.srv.port)
        try:
            s.sendall(
                b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"
                b"GET /favicon.ico HTTP/1.1\r\nHost: x\r\n\r\n"
            )
            buffer = b""
            status1, _, body1, buffer = harness.read_response_buffered(s, buffer)
            status2, _, body2, buffer = harness.read_response_buffered(s, buffer)
            self.assertEqual((status1, status2), (200, 200))
            self.assertTrue(body1.lower().startswith(b"<!doctype html>"))
            self.assertGreater(len(body2), 0)
        finally:
            s.close()

    def test_pipelined_post_then_get(self):
        s = harness.connect(self.srv.port)
        payload = b'{"user":"u","content":"hello"}'
        try:
            s.sendall(
                b"POST /send HTTP/1.1\r\nHost: x\r\nContent-Length: "
                + str(len(payload)).encode() + b"\r\n\r\n" + payload
                + b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"
            )
            buffer = b""
            status1, _, body1, buffer = harness.read_response_buffered(s, buffer)
            status2, _, body2, buffer = harness.read_response_buffered(s, buffer)
            self.assertEqual(status1, 200)
            self.assertEqual(body1, b"OK")
            self.assertEqual(status2, 200)
            self.assertTrue(body2.lower().startswith(b"<!doctype html>"))
        finally:
            s.close()


class TestStrictness(P1Base):
    def test_missing_host_is_400(self):
        status, _, _, _ = self.raw(b"GET / HTTP/1.1\r\n\r\n")
        self.assertEqual(status, 400)

    def test_duplicate_content_length_is_400(self):
        status, _, _, _ = self.raw(
            b"POST /send HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n"
            b"Content-Length: 3\r\n\r\n{}"
        )
        self.assertEqual(status, 400)

    def test_invalid_content_length_is_400(self):
        status, _, _, _ = self.raw(
            b"POST /send HTTP/1.1\r\nHost: x\r\nContent-Length: 12a\r\n\r\n"
        )
        self.assertEqual(status, 400)

    def test_missing_content_length_post_is_411(self):
        status, _, _, _ = self.raw(
            b"POST /send HTTP/1.1\r\nHost: x\r\n\r\n"
        )
        self.assertEqual(status, 411)

    def test_chunked_is_501(self):
        s = harness.connect(self.srv.port)
        try:
            s.sendall(
                b"POST /send HTTP/1.1\r\nHost: x\r\n"
                b"Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n"
            )
            status, headers, _, _ = harness.read_response_ex(s)
            self.assertEqual(status, 501)
            self.assert_eof(s)
        finally:
            s.close()

    def test_unsupported_version_is_505(self):
        status, _, _, _ = self.raw(b"GET / HTTP/2.0\r\nHost: x\r\n\r\n")
        self.assertEqual(status, 505)


if __name__ == "__main__":
    unittest.main(verbosity=2)
