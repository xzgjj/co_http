# co_http

基于小彭老师 [archibate/co_http](https://github.com/archibate/co_http) 的 C++ 异步 HTTP 服务器与聊天室实现，用于学习 epoll、HTTP/1.1、回调式异步与 C++20 协程。

## 项目来源

原始项目是小彭老师的 C++17 回调式异步 HTTP 服务器。我在保留回调实现的基础上，补充HTTP/1.1 协议细节、C++20 协程版本、SQLite 持久化、SSE/WebSocket、TLS便于对照学习，更进一步靠近生产项目

最初没有直接使用 co_async 的协程实现，先用 C++17 回调把 HTTP 与异步 I/O 跑通，再以独立编译目标实现协程版本，两版共用协议与业务代码。

## 功能

| 模块 | 内容 |
|---|---|
| HTTP | HTTP/1.1 常用子集：method + path 路由、HEAD、query、keep-alive、pipelining、Content-Length 校验、请求上限与超时 |
| 聊天 | `/send` 发消息、`/recv` 拉历史、长轮询、SSE、WebSocket；消息环形缓冲与 SQLite 持久化 |
| 服务端 | 回调版、C++20 协程版、多进程 `SO_REUSEPORT` 实验版 |
| 静态资源 | 启动缓存、ETag/304、gzip、sendfile |
| 可观测 | `/healthz`、`/metrics`、访问日志 |
| 安全 | 请求上限、连接上限、整请求 deadline、TLS（OpenSSL） |
| 前端 | 原生 HTML/CSS/JS，无第三方 CDN；WebSocket 优先，SSE 回退；昵称本地保存；深色模式 |

## 架构

```
chat_server.cpp      聊天业务路由（回调版）
chat_server_co.cpp   聊天业务路由（协程版）
chat_server_mp.cpp   多进程实验入口
        │
        ├── http_server.hpp / co_http_server.hpp   连接与请求处理
        ├── http_codec.hpp                         HTTP 编解码
        ├── chat_business.hpp                      消息、长轮询广播、SQLite
        ├── http_delivery.hpp                      静态缓存、ETag、gzip
        ├── realtime.hpp / ws_hub.hpp              SSE / WebSocket
        └── tls.hpp                                TLS
                │
                ├── io_context.hpp    epoll 事件循环与异步操作
                ├── timer_context.hpp 定时器
                └── stop_source.hpp   取消令牌
```

回调版与协程版共用 `http_codec.hpp`、`chat_business.hpp`、`http_delivery.hpp`、`realtime.hpp` 与内核操作槽，只有连接状态机不同。

## 目录结构

| 路径 | 说明 |
|---|---|
| `chat_server.cpp` | 回调版入口与聊天路由 |
| `chat_server_co.cpp` | C++20 协程版入口 |
| `chat_server_mp.cpp` | 多进程实验入口 |
| `http_server.hpp` | 回调版连接处理、路由、流式响应 |
| `co_http_server.hpp` | 协程版连接处理 |
| `http_codec.hpp` | HTTP 请求解析与响应写出 |
| `io_context.hpp` | epoll 事件循环、异步读写、定时、取消 |
| `chat_business.hpp` | 消息缓冲、长轮询广播、SQLite |
| `realtime.hpp` / `ws_hub.hpp` | SSE 与 WebSocket 协议 |
| `http_delivery.hpp` / `metrics.hpp` | 静态资源交付与指标 |
| `static/` | 聊天前端 |
| `tests/` | 单元测试、协议测试、稳定性测试、压测脚本 |
| `tools/ci.py` | 构建、测试、ASan、压测的统一入口 |
| `deploy/` | Dockerfile、systemd 服务文件 |
| `docs/` | 机制分析、架构图、演进设计、阶段总结 |
| `include/bearssl/` | 第三方 TLS 库头文件 |

## 环境要求

- Linux（依赖 epoll；Windows 仅用于编辑代码）
- GCC 13 及以上（协程版需要 C++20）
- CMake 3.12 及以上
- zlib、SQLite3、OpenSSL 开发库
- Python 3（运行测试脚本）
- `wrk`（可选，压测用）

已在 GCC 13.3 / Ubuntu 24.04 与 GCC 14.3 / Ubuntu 22.04 下验证。

## 构建

```bash
git clone https://github.com/xzgjj/co_http.git
cd co_http

sudo apt update
sudo apt install -y build-essential cmake zlib1g-dev libsqlite3-dev libssl-dev python3 wrk

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

构建产物：

| 产物 | 说明 |
|---|---|
| `build/chat_server` | 回调版，默认 |
| `build/chat_server_co` | C++20 协程版，支持 `--selftest` |
| `build/chat_server_mp` | 多进程实验版，支持 `--workers N` |

## 运行

```bash
cd build
./chat_server
```

默认监听 `0.0.0.0:8080`，静态目录为相对路径 `../static`。浏览器访问：

```text
http://127.0.0.1:8080
```

常用启动方式：

```bash
# 指定端口与静态目录
PORT=9090 STATIC_ROOT=../static ./chat_server

# 协程版
./chat_server_co
./chat_server_co --selftest

# 多进程实验：N 个子进程通过 SO_REUSEPORT 共享端口
./chat_server_mp --workers 4
```

启用 TLS（需要证书与私钥）：

```bash
TLS_CERT=/path/to/cert.pem TLS_KEY=/path/to/key.pem ./chat_server
```

此时访问 `https://127.0.0.1:8080`。

## 配置

通过环境变量配置：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `PORT` | `8080` | 监听端口 |
| `STATIC_ROOT` | `../static` | 静态文件目录（相对于工作目录） |
| `CHAT_DB` | 空 | SQLite 数据库路径；为空时只保存在内存 |
| `TLS_CERT` / `TLS_KEY` | 空 | 证书与私钥路径；两者都设置时启用 HTTPS |
| `MAX_HEADER` | `16384` | 请求头最大字节数 |
| `MAX_URL` | `8192` | URL 最大字节数 |
| `MAX_BODY` | `1048576` | 请求体最大字节数 |
| `MAX_CONN` | `1024` | 最大并发连接数 |
| `REQUEST_TIMEOUT_MS` | `10000` | 单请求处理超时 |
| `WRITE_TIMEOUT_MS` | `10000` | 响应写超时 |
| `POLL_TIMEOUT_MS` | `3000` | 长轮询挂起时长 |
| `SSE_HEARTBEAT_MS` | `15000` | SSE 心跳间隔 |

## 测试

构建完成后运行单元测试：

```bash
ctest --test-dir build --output-on-failure
```

单元测试包括 HTTP 编解码、时间轮、SSE/WebSocket 协议与协程自检。

运行完整测试：

```bash
# 构建 + 单元/集成测试 + ASan/UBSan + 压测
python3 tools/ci.py ci

# 单独运行
python3 tools/ci.py build
python3 tools/ci.py test
python3 tools/ci.py asan
python3 tools/ci.py stress

# 两版服务器的黑盒测试与压测
bash tests/run_all.sh
bash tests/run_stress.sh
```

测试覆盖：

- RST、坏 JSON、超限请求、slowloris、EMFILE、SIGTERM 等稳定性用例
- HEAD、405、query、keep-alive、pipelining、Content-Length、chunked 等协议用例
- ETag/304、gzip、sendfile、writev、healthz、metrics
- SQLite 持久化、SSE、WebSocket、多进程
- 前端接线与 TLS

压测示例：

```bash
wrk -t2 -c100 -d10s http://127.0.0.1:8080/
wrk -t2 -c100 -d10s -H "Connection: close" http://127.0.0.1:8080/
```

早期版本在本机 WSL2 的压测记录（100 连接，仅作参考）：

| 指标 | 数值 |
|---|---|
| Latency | 139.63 ms |
| Req/Sec（单线程） | 351.12 |
| 总请求数 | 2099（3.01 s） |
| Requests/sec | 698.49 |
| Transfer/sec | 2.89 MB |

当前版本的压测与对照数据见 [P1–P4 阶段总结](docs/p1-p4-phase-summary.md)。

## 部署

Docker：

```bash
docker build -f deploy/Dockerfile -t co_http:latest .
docker run --rm -p 8080:8080 co_http:latest
```

镜像基于 `deploy/Dockerfile` 构建协程版。运行镜像前请确认基础镜像中已包含 SQLite 与 OpenSSL 运行库；如需持久化，可设置 `CHAT_DB=/data/chat.db` 并挂载 `/data`。

systemd：

```bash
sudo install -d /opt/co_http
sudo cp build/chat_server_co /opt/co_http/chat_server
sudo cp -r static /opt/co_http/static
sudo cp deploy/co_http.service /etc/systemd/system/co_http.service
sudo systemctl daemon-reload
sudo systemctl enable --now co_http
curl -s http://127.0.0.1:8080/healthz
```

更完整的部署说明见 `deploy/README.md`。生产环境建议在前面加 Nginx 或 Caddy，由反向代理负责 TLS、gzip 与连接数限制。



## 已知限制

- 仅支持 Linux；Windows 下只能编辑代码。
- 多进程版本中各 worker 的内存状态彼此独立，聊天消息与在线状态不共享，仅用于 `SO_REUSEPORT` 分流实验。
- 前端为原生实现，不包含账号、多房间、文件上传等完整 IM 功能。
- 协程版与回调版共用业务代码，但连接处理各有一份实现。

## 致谢与许可

原始项目：[archibate/co_http](https://github.com/archibate/co_http)，作者小彭老师。

仓库未附 LICENSE 文件；如需使用或分发，请先与作者确认授权。
