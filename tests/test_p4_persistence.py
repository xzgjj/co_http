"""P4 持久化：SQLite 消息落库 + 重启后历史恢复（独立事实来源：直接用 sqlite3 查库）。"""
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402


class TestSQLitePersistence(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="co_http_p4_")
        self.db_path = os.path.join(self.tmp, "chat.db")
        self.env = {"CHAT_DB": self.db_path}
        self.srv = harness.Server(env=self.env)
        self.srv.wait_ready()
        self.addCleanup(self._cleanup)

    def _cleanup(self):
        if self.srv.alive():
            self.srv.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def restart(self):
        self.srv.stop()
        self.srv = harness.Server(env=self.env)
        self.srv.wait_ready()

    def recv_messages(self, first):
        status, _, body = harness.post(
            self.srv.port, "/recv", b'{"first":%d}' % first
        )
        self.assertEqual(status, 200)
        return json.loads(body)

    def test_send_is_persisted_and_survives_restart(self):
        status, _, body = harness.post(
            self.srv.port, "/send",
            b'{"user":"persist","content":"hello-db"}',
        )
        self.assertEqual(status, 200)
        self.assertEqual(body, b"OK")

        # 独立事实来源：直接查 SQLite（不经过服务器）
        conn = sqlite3.connect(self.db_path)
        try:
            rows = conn.execute("SELECT user, content FROM messages").fetchall()
        finally:
            conn.close()
        self.assertIn(("persist", "hello-db"), rows)

        self.restart()

        messages = self.recv_messages(0)
        self.assertTrue(
            any(m["user"] == "persist" and m["content"] == "hello-db"
                for m in messages),
            "persisted message missing after restart: %r" % (messages,),
        )

    def test_cursor_continues_after_restart(self):
        harness.post(self.srv.port, "/send", b'{"user":"u1","content":"first"}')
        self.restart()
        # 重启后：system(0) + first(1)，所以 first=2 应当挂起；发一条新消息后可立刻取到
        status, _, body = harness.post(
            self.srv.port, "/send", b'{"user":"u2","content":"second"}'
        )
        self.assertEqual(status, 200)
        messages = self.recv_messages(2)
        self.assertEqual(len(messages), 1)
        self.assertEqual(messages[0]["content"], "second")

    def test_bad_db_path_degrades_to_memory(self):
        self.srv.stop()
        self.srv = harness.Server(env={"CHAT_DB": "/nonexistent/dir/chat.db"})
        self.srv.wait_ready()
        status, _, body = harness.get(self.srv.port, "/healthz")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"ok")
        status, _, _ = harness.post(
            self.srv.port, "/send", b'{"user":"u","content":"mem"}'
        )
        self.assertEqual(status, 200)


if __name__ == "__main__":
    unittest.main(verbosity=2)
