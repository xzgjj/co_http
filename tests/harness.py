"""测试夹具：启动/停止 chat_server，提供原始 socket 的 HTTP 收发工具。

黑盒测试：只通过 TCP/HTTP 外部行为断言，不触碰服务器内部实现。
运行环境：WSL Ubuntu（与项目构建环境一致）。
"""
import os
import signal
import socket
import struct
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
# 允许用环境变量指定别的二进制（例如 ASan/UBSan 构建），默认用 build/chat_server。
BIN = os.environ.get("CHAT_SERVER_BIN", os.path.join(BUILD, "chat_server"))
DEFAULT_PORT = 8080


class Server:
    """一个 chat_server 子进程。cwd=build 以满足 ../static 的相对路径。"""

    def __init__(self, env=None, port=DEFAULT_PORT, nofile=None, logfile=None):
        self.port = port
        self.env = dict(os.environ)
        self.env.update(env or {})
        self.logfile = logfile
        preexec = None
        if nofile is not None:
            import resource

            def preexec():  # noqa: E306
                resource.setrlimit(resource.RLIMIT_NOFILE, (nofile, nofile))

        kwargs = {}
        if logfile:
            self._log_handle = open(logfile, "wb")
            kwargs["stdout"] = self._log_handle
        else:
            self._log_handle = None
            kwargs["stdout"] = subprocess.PIPE
        self.proc = subprocess.Popen(
            [BIN], cwd=BUILD, stderr=subprocess.STDOUT,
            env=self.env, preexec_fn=preexec, **kwargs,
        )

    def wait_ready(self, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    "server exited before ready rc=%r: %s"
                    % (self.proc.returncode, self._read_output())
                )
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=0.2)
                s.close()
                return
            except OSError:
                time.sleep(0.02)
        raise TimeoutError("server not ready on port %d" % self.port)

    def alive(self):
        return self.proc.poll() is None

    def returncode(self):
        return self.proc.poll()

    def _read_output(self):
        if self._log_handle is not None:
            self._log_handle.flush()
            try:
                with open(self.logfile, "rb") as f:
                    return f.read().decode("utf-8", "replace")
            except OSError:
                return ""
        try:
            out, _ = self.proc.communicate(timeout=1)
            return out.decode("utf-8", "replace")
        except subprocess.TimeoutExpired:
            self.proc.kill()
            out, _ = self.proc.communicate()
            return out.decode("utf-8", "replace")

    def stop(self, sig=signal.SIGTERM, timeout=3.0):
        """发信号并等待退出；返回退出码（被杀为负值）。"""
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        if self._log_handle is not None:
            self._log_handle.close()
            self._log_handle = None
        return self.proc.returncode


def connect(port, timeout=3.0):
    return socket.create_connection(("127.0.0.1", port), timeout=timeout)


def rst_close(sock):
    """设置 SO_LINGER=0 后关闭，让内核发 RST 而不是 FIN。"""
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    sock.close()


def read_response_ex(sock, timeout=3.0, expect_body=True):
    """读取一个完整 HTTP 响应，额外返回原始头部字节。
    返回 (status, headers, body, head_bytes)；连接提前关闭时 status=None。
    HEAD 响应没有正文：传 expect_body=False（Content-Length 仍会出现在头里）。"""
    sock.settimeout(timeout)
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            return None, None, data, data
        data += chunk
    head, rest = data.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    try:
        status = int(lines[0].split()[1])
    except (IndexError, ValueError):
        return None, None, data, head
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.strip().lower()] = v.strip()
    length = int(headers.get(b"content-length", b"0"))
    body = rest
    while expect_body and len(body) < length:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        body += chunk
    return status, headers, body, head


def read_response_buffered(sock, buffer=b"", timeout=3.0, expect_body=True):
    """流水线专用：调用方持有 buffer，返回 (status, headers, body, 剩余 buffer)。

    响应可能被 TCP 合并到同一次 recv：这里只消费属于当前响应的字节，
    多读的字节留在 buffer 里供下一个响应解析，避免"第二个响应已被吃掉"的假超时。
    """
    sock.settimeout(timeout)
    data = buffer
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            return None, None, data, b""
        data += chunk
    head, rest = data.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    try:
        status = int(lines[0].split()[1])
    except (IndexError, ValueError):
        return None, None, data, b""
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.strip().lower()] = v.strip()
    if b"content-length" not in headers:
        # 无 CL（本函数只用于带 CL 的流水线响应）：body 到连接关闭为止。
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            rest += chunk
        return status, headers, rest, b""
    length = int(headers[b"content-length"])
    while len(rest) < length:
        chunk = sock.recv(4096)
        if not chunk:
            break
        rest += chunk
    body = rest[:length]
    return status, headers, body, rest[length:]


def read_response(sock, timeout=3.0):
    status, headers, body, _ = read_response_ex(sock, timeout)
    return status, headers, body


def _b(x):
    return x.encode() if isinstance(x, str) else x


def request(port, raw, timeout=3.0):
    """发送原始请求字节并读一个响应。"""
    s = connect(port, timeout)
    try:
        s.sendall(raw)
        return read_response(s, timeout)
    finally:
        s.close()


def get(port, path=b"/", timeout=3.0):
    return request(port, b"GET " + _b(path) + b" HTTP/1.1\r\nHost: x\r\n\r\n", timeout)


def post(port, path, body, timeout=3.0):
    path = _b(path)
    body = _b(body)
    return request(
        port,
        b"POST " + path + b" HTTP/1.1\r\nHost: x\r\nContent-Length: "
        + str(len(body)).encode() + b"\r\n\r\n" + body,
        timeout,
    )
