"""P4 实时推送：SSE（text/event-stream）与 WebSocket（握手/回显/ping-pong/close）。

运行（默认回调版）：python3 tests/test_p4_realtime.py -v
协程版：CHAT_SERVER_BIN=$PWD/build/chat_server_co python3 tests/test_p4_realtime.py -v
"""
import base64
import hashlib
import json
import os
import socket
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 0x1, 0x2, 0x8, 0x9, 0xA


def client_frame(opcode, payload: bytes) -> bytes:
    frame = bytes([0x80 | opcode])
    mask = b"\x12\x34\x56\x78"
    length = len(payload)
    if length < 126:
        frame += bytes([0x80 | length])
    elif length <= 0xFFFF:
        frame += bytes([0x80 | 126]) + length.to_bytes(2, "big")
    else:
        frame += bytes([0x80 | 127]) + length.to_bytes(8, "big")
    frame += mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return frame


def parse_server_frame(buffer: bytes):
    """返回 (fin, opcode, payload, consumed) 或 None（不完整）。"""
    if len(buffer) < 2:
        return None
    b0, b1 = buffer[0], buffer[1]
    fin = bool(b0 & 0x80)
    opcode = b0 & 0x0F
    masked = bool(b1 & 0x80)
    length = b1 & 0x7F
    pos = 2
    if length == 126:
        if len(buffer) < pos + 2:
            return None
        length = int.from_bytes(buffer[pos:pos + 2], "big")
        pos += 2
    elif length == 127:
        if len(buffer) < pos + 8:
            return None
        length = int.from_bytes(buffer[pos:pos + 8], "big")
        pos += 8
    if masked:
        if len(buffer) < pos + 4:
            return None
        mask = buffer[pos:pos + 4]
        pos += 4
    if len(buffer) < pos + length:
        return None
    payload = buffer[pos:pos + length]
    if masked:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return fin, opcode, payload, pos + length


class WSConn:
    def __init__(self, sock):
        self.sock = sock
        self.buffer = b""

    def send(self, opcode, payload):
        self.sock.sendall(client_frame(opcode, payload))

    def recv(self, timeout=3.0):
        self.sock.settimeout(timeout)
        while True:
            parsed = parse_server_frame(self.buffer)
            if parsed:
                fin, opcode, payload, consumed = parsed
                self.buffer = self.buffer[consumed:]
                return fin, opcode, payload
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("connection closed")
            self.buffer += chunk


class RealtimeBase(unittest.TestCase):
    def setUp(self):
        self.srv = harness.Server(env={"SSE_HEARTBEAT_MS": "60000"})
        self.srv.wait_ready()
        self.addCleanup(lambda: self.srv.alive() and self.srv.stop())

    def open_sse(self):
        s = harness.connect(self.srv.port)
        s.sendall(
            b"GET /events HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n"
        )
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                self.fail("SSE closed before headers")
            data += chunk
        head, rest = data.split(b"\r\n\r\n", 1)
        lines = head.split(b"\r\n")
        self.assertIn(b" 200 ", lines[0])
        headers = {}
        for line in lines[1:]:
            if b":" in line:
                k, v = line.split(b":", 1)
                headers[k.strip().lower()] = v.strip()
        self.assertTrue(headers.get(b"content-type", b"").startswith(b"text/event-stream"))
        return s, rest

    def read_sse_data(self, sock, buffer=b"", timeout=3.0):
        """读取一个 SSE 事件，返回 (剩余缓冲, data 内容或 None)。"""
        sock.settimeout(timeout)
        data = buffer
        while b"\n\n" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
        if b"\n\n" not in data:
            return data, None
        event, rest = data.split(b"\n\n", 1)
        payload = None
        for line in event.split(b"\n"):
            if line.startswith(b"data:"):
                payload = line[5:].strip()
        return rest, payload

    def open_ws(self):
        key = base64.b64encode(os.urandom(16)).decode()
        s = harness.connect(self.srv.port)
        s.sendall(
            (
                "GET /ws HTTP/1.1\r\n"
                "Host: x\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Key: %s\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n" % key
            ).encode()
        )
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                self.fail("WS closed before handshake")
            data += chunk
        head, rest = data.split(b"\r\n\r\n", 1)
        lines = head.split(b"\r\n")
        self.assertIn(b" 101 ", lines[0])
        headers = {}
        for line in lines[1:]:
            if b":" in line:
                k, v = line.split(b":", 1)
                headers[k.strip().lower()] = v.strip()
        expected = base64.b64encode(
            hashlib.sha1((key + WS_GUID).encode()).digest()
        ).decode()
        self.assertEqual(headers.get(b"sec-websocket-accept", b"").decode(), expected)
        ws = WSConn(s)
        ws.buffer = rest
        return ws


class TestSSE(RealtimeBase):
    def test_replays_history_on_connect(self):
        harness.post(self.srv.port, "/send",
                     b'{"user":"early","content":"before-connect"}')
        sse, rest = self.open_sse()
        try:
            rest, payload = self.read_sse_data(sse, rest)
            self.assertIsNotNone(payload, "SSE 连接后应先重放历史")
            messages = json.loads(payload)
            self.assertTrue(any(m["content"] == "before-connect" for m in messages),
                            "历史消息缺失: %r" % (messages,))
        finally:
            sse.close()

    def test_stream_delivers_message(self):
        sse, rest = self.open_sse()
        try:
            # 先消费"历史重放"这一帧
            rest, history = self.read_sse_data(sse, rest)
            self.assertIsNotNone(history)
            harness.post(self.srv.port, "/send",
                         b'{"user":"sse","content":"hello-sse"}')
            rest, payload = self.read_sse_data(sse, rest)
            self.assertIsNotNone(payload)
            self.assertIn(b"hello-sse", payload)
        finally:
            sse.close()
        self.assertTrue(self.srv.alive())

    def test_metrics_count_sse(self):
        _, _ = self.open_sse()
        before = json.loads(harness.get(self.srv.port, b"/metrics")[2])
        self.assertGreaterEqual(before["sse_total"], 1)


class TestWebSocket(RealtimeBase):
    def test_handshake_echo_ping_close(self):
        ws = self.open_ws()
        try:
            ws.send(OP_TEXT, b"hello-ws")
            fin, opcode, payload = ws.recv()
            self.assertTrue(fin)
            self.assertEqual(opcode, OP_TEXT)
            self.assertEqual(payload, b"hello-ws")

            ws.send(OP_PING, b"p")
            fin, opcode, payload = ws.recv()
            self.assertEqual(opcode, OP_PONG)
            self.assertEqual(payload, b"p")

            ws.send(OP_CLOSE, b"")
            fin, opcode, payload = ws.recv()
            self.assertEqual(opcode, OP_CLOSE)
        finally:
            ws.sock.close()
        self.assertTrue(self.srv.alive())

    def test_broadcast_to_other_ws_client(self):
        ws1 = self.open_ws()
        ws2 = self.open_ws()
        try:
            ws1.send(OP_TEXT, b"broadcast-x")
            fin, opcode, payload = ws1.recv()
            self.assertEqual(opcode, OP_TEXT)
            self.assertEqual(payload, b"broadcast-x")
            # 另一个 WS 客户端应收到同一条消息
            fin, opcode, payload = ws2.recv(timeout=3.0)
            self.assertEqual(opcode, OP_TEXT)
            self.assertEqual(payload, b"broadcast-x")
        finally:
            ws1.sock.close()
            ws2.sock.close()
        self.assertTrue(self.srv.alive())

    def test_ws_message_reaches_long_poll(self):
        ws = self.open_ws()
        s = harness.connect(self.srv.port)
        try:
            body = b'{"first":1}'
            s.sendall(
                b"POST /recv HTTP/1.1\r\nHost: x\r\nContent-Length: "
                + str(len(body)).encode() + b"\r\n\r\n" + body
            )
            pending = b""
            ws.send(OP_TEXT, b'{"user":"wsu","content":"from-ws"}')
            # 回显
            _, opcode, payload = ws.recv()
            self.assertEqual(opcode, OP_TEXT)
            # 长轮询应被唤醒
            s.settimeout(3.0)
            while b"\r\n\r\n" not in pending:
                chunk = s.recv(4096)
                if not chunk:
                    break
                pending += chunk
            head, rest = pending.split(b"\r\n\r\n", 1)
            headers = {}
            for line in head.split(b"\r\n")[1:]:
                if b":" in line:
                    k, v = line.split(b":", 1)
                    headers[k.strip().lower()] = v.strip()
            length = int(headers.get(b"content-length", b"0"))
            body_out = rest
            while len(body_out) < length:
                body_out += s.recv(4096)
            messages = json.loads(body_out)
            self.assertTrue(any(m["content"] == "from-ws" for m in messages))
        finally:
            ws.sock.close()
            s.close()

    def test_bad_upgrade_is_400(self):
        status, _, _ = harness.get(self.srv.port, b"/ws")
        self.assertEqual(status, 400)


if __name__ == "__main__":
    unittest.main(verbosity=2)
