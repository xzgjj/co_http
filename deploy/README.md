# 部署说明（P4）

## 二进制

| 目标 | 说明 |
|---|---|
| `build/chat_server` | 回调版（P0/P1 基线；默认） |
| `build/chat_server_co` | C++20 协程版（P2；功能等价，含 `--selftest`） |

两版共用配置（环境变量）：`PORT`、`STATIC_ROOT`、`MAX_HEADER`、`MAX_URL`、
`MAX_BODY`、`MAX_CONN`、`REQUEST_TIMEOUT_MS`、`WRITE_TIMEOUT_MS`、`POLL_TIMEOUT_MS`。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
PORT=8080 STATIC_ROOT=./static ./build/chat_server_co
```

## Docker

```bash
docker build -f deploy/Dockerfile -t co_http:latest .
docker run --rm -p 8080:8080 co_http:latest
```

镜像内健康检查访问 `/healthz`；指标在 `/metrics`。

## systemd

```bash
sudo install -d /opt/co_http
sudo cp build/chat_server_co /opt/co_http/chat_server
sudo cp -r static /opt/co_http/static
sudo cp deploy/co_http.service /etc/systemd/system/co_http.service
sudo systemctl daemon-reload && sudo systemctl enable --now co_http
curl -s localhost:8080/healthz   # ok
```

## 反向代理

TLS / gzip / 连接上限建议交给 Caddy 或 Nginx；程序自身已支持 ETag/304 与 gzip
（`Accept-Encoding: gzip`，>1KB 才压）。
