"""前端接线检查：/ 返回的新页面必须走 WebSocket/SSE，且不依赖第三方 CDN。"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402


class TestFrontend(unittest.TestCase):
    def setUp(self):
        self.srv = harness.Server()
        self.srv.wait_ready()
        self.addCleanup(lambda: self.srv.alive() and self.srv.stop())

    def page(self):
        status, headers, body = harness.get(self.srv.port, b"/")
        self.assertEqual(status, 200)
        self.assertIn(b"text/html", headers.get(b"content-type", b""))
        return body.decode("utf-8", "replace")

    def test_uses_websocket_and_sse(self):
        text = self.page()
        self.assertIn("new WebSocket", text)
        self.assertIn("new EventSource", text)
        self.assertIn("/events?first=", text)
        self.assertIn("/ws", text)

    def test_no_third_party_cdn(self):
        text = self.page()
        self.assertNotIn("unpkg", text)
        self.assertNotIn("cdn.", text)
        self.assertNotIn("https://", text)

    def test_history_loaded_before_transport(self):
        text = self.page()
        self.assertIn('fetch("/recv"', text)
        self.assertIn("loadHistory", text)

    def test_saved_nicknames_and_no_footer_hint(self):
        text = self.page()
        # 页脚说明已删除
        self.assertNotIn("纯原生 HTML", text)
        self.assertNotIn("无第三方依赖", text)
        # 昵称保存 / 下拉切换
        self.assertIn("nick-toggle", text)
        self.assertIn("nick-menu", text)
        self.assertIn("chat-nicks", text)
        self.assertIn("localStorage", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
