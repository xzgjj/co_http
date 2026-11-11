"""TLS：设置 TLS_CERT / TLS_KEY 后，同一端口提供 HTTPS（两版服务器共用）。"""
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402

OPENSSL = "/usr/bin/openssl" if os.path.exists("/usr/bin/openssl") else shutil.which("openssl")


def make_cert(directory):
    cert = os.path.join(directory, "cert.pem")
    key = os.path.join(directory, "key.pem")
    subprocess.run(
        [OPENSSL, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=localhost"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    return cert, key


@unittest.skipUnless(shutil.which("openssl") or os.path.exists("/usr/bin/openssl"),
                     "需要 openssl CLI 生成测试证书")
class TestTLS(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="co_http_tls_")
        self.cert, self.key = make_cert(self.tmp)
        self.env = {"TLS_CERT": self.cert, "TLS_KEY": self.key}
        self.srv = harness.Server(env=self.env)
        self.addCleanup(self._cleanup)
        self.srv.wait_ready()

    def _cleanup(self):
        if self.srv.alive():
            self.srv.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def tls_socket(self):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        raw = harness.connect(self.srv.port, timeout=3)
        return ctx.wrap_socket(raw, server_hostname="localhost")

    def request(self, raw_request):
        sock = self.tls_socket()
        try:
            sock.sendall(raw_request)
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk
            head, rest = data.split(b"\r\n\r\n", 1)
            status = int(head.split(b"\r\n")[0].split()[1])
            headers = {}
            for line in head.split(b"\r\n")[1:]:
                if b":" in line:
                    k, v = line.split(b":", 1)
                    headers[k.strip().lower()] = v.strip()
            length = int(headers.get(b"content-length", b"0"))
            body = rest
            while len(body) < length:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                body += chunk
            return status, headers, body
        finally:
            sock.close()

    def test_https_healthz(self):
        status, headers, body = self.request(
            b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        self.assertEqual(status, 200)
        self.assertEqual(body, b"ok")

    def test_https_index_page(self):
        status, _, body = self.request(
            b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        self.assertEqual(status, 200)
        self.assertTrue(body.lower().startswith(b"<!doctype html>"))

    def test_https_post_send(self):
        payload = b'{"user":"tls","content":"hello-tls"}'
        status, _, body = self.request(
            b"POST /send HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
            + str(len(payload)).encode() + b"\r\n\r\n" + payload
        )
        self.assertEqual(status, 200)
        self.assertEqual(body, b"OK")


if __name__ == "__main__":
    unittest.main(verbosity=2)
