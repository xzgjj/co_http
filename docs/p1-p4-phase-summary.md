# P1–P4 阶段总结（协程版 + 交付 + 工程化）—— 待 review

> 交付范围：`docs/server-evolution-design.md` 的 P1–P4。P0 见 `docs/p0-phase-summary.md`。
> 状态：**全部实现完成，本地统一 CI 全绿（build / test / asan / stress），未提交，等 review。**
> 环境：WSL2 Ubuntu 24.04 + GCC 13.3 + CMake 3.28 + zlib；项目仅支持 Linux（epoll），Windows 侧只做代码编辑。
> 一键复现：`python3 tools/ci.py ci`。

---

## 0. 结果总览

| 阶段 | 交付 | 用例 / 证据 | 结果 |
|---|---|---|---|
| **P1** 协议正确性 | method+path 路由、HEAD、OPTIONS/405、query、Date/reason、Connection/HTTP1.0、pipelining、Content-Length 严格化、chunked 501、Host、505 | `tests/test_p1_protocol.py` 20 条；`ctest codec_tests` 41 个断言 | 两版全过 |
| **P2** 协程架构 | `task.hpp` / `co_io.hpp` / `co_http_server.hpp` / `chat_server_co.cpp`；work-stealing 调度器（自检） | 协程版跑同一套 P0(13)+P1(20)+P3(11)+P4持久化(3)；`--selftest`（Task/sleep/线程池） | 全过，功能等价 |
| **P3** 性能与交付 | ETag/Last-Modified/304、gzip、8KB 读缓冲、TCP_NODELAY、**sendfile / writev**、**时间轮**、访问日志、`/healthz`、`/metrics` | `tests/test_p3_delivery.py` 11 条；`tests/unit/test_timer_wheel.cpp` 13 断言；`tests/stress.py`（两版） | 两版 0 错误，≈24–26k rps（ext4 静态根），p99≈1.4ms |
| **P4/P5** 工程化与实时 | `tools/ci.py`、GitHub Actions、`--version`、Dockerfile、systemd、**SQLite**、**多进程实验**、**SSE/WebSocket**、**前端接线（WS 优先/SSE 回退）**、**WS 广播**、**SSE 历史重放**、**TLS** | `test_p4_persistence.py` 3；`test_p4_realtime.py` 7；`test_p5_frontend.py` 3；`test_p5_tls.py` 3；`test_p4_multiprocess.py` 2 | 两版全过 |

**统一 CI 实测输出**（`tools/ci.py ci`）：

```
build    OK
test     OK     # ctest 4/4 + P0/P1/P3/P4持久化/实时推送 ×（回调版 + 协程版）+ 多进程实验
asan     OK     # ASan/UBSan 构建 + 同一套黑盒用例 × 两版
stress   OK     # 两版各 20 连接 × 3s + 10 客户端长轮询
```

压测数据（本机 WSL，Python 客户端，20 连接 keep-alive；静态根放在 ext4 以排除 WSL DrvFs 干扰）：

| 版本 | 请求数 / 3s | rps | p50 | p99 | 错误 | 长轮询唤醒 |
|---|---|---|---|---|---|---|
| 回调版 `chat_server` | 73,362 | 24,454 | 0.77ms | 1.38ms | 0 | 10/10，204ms（含 200ms 等待窗口） |
| 协程版 `chat_server_co` | 78,311 | 26,104 | 0.71ms | 1.35ms | 0 | 10/10，203ms |

> 两版同量级（差异在噪声范围内）。rps 由 Python 客户端能力封顶，不作为服务端上限。
> **WSL DrvFs 提醒**：静态文件在 `/mnt/d` 时 `sendfile` 实测只有约 4.5k rps，复制到 ext4 后约 26k rps；
> `tests/stress.py` 在 WSL 下自动把 `static/` 复制到临时 ext4 目录再压测，避免把文件系统差异当成服务端退化。

---

## 1. 统一设计（"不独立"的落点）

两个服务器共用同一套内核与业务，**只有控制流不同**：

| 层 | 共享组件（两版同一份代码） | 回调版独有 | 协程版独有 |
|---|---|---|---|
| 异步内核 | `io_context.hpp`（epoll/fd 操作槽/定时/取消/post）、`timer_context.hpp`、`stop_source.hpp` | — | `co_io.hpp`（awaiter 适配） |
| 协议 | `http_codec.hpp`（解析/写头/reason/Date）、`bytes_buffer.hpp` | `http_server.hpp`（连接状态机） | `co_http_server.hpp`（线性连接协程） |
| 业务 | `chat_business.hpp`（消息环/hub/静态/`/send`/`/recv` 纯逻辑） | `chat_server.cpp`（路由 + 长轮询适配） | `chat_server_co.cpp`（协程路由 + event 桥接） |
| 交付 | `http_delivery.hpp`（ETag/304/gzip）、`metrics.hpp`、`server_config` | — | — |
| 测试 | `tests/harness.py` + 同一套黑盒用例；`CHAT_SERVER_BIN` 切换二进制 | — | — |

收益：P0/P1/P3 的修复与用例对两版**一次生效**；对照实验（§0）天然可比。

---

## 2. P1 协议正确性

**目标思路**：先冻结"前端可依赖的 HTTP 语义"，再让协程版在语义上追平。

实现要点：

- **路由**：`path → method → handler`；HEAD 复用 GET；未注册方法 → `405 + Allow`；OPTIONS → `200 + Allow`。
- **请求行**：拆 `method / path / query / version`；`GET /?x=1` 不再 404。
- **响应**：标准 reason phrase、`Date`、`Connection` 协商（HTTP/1.0 默认 close、`Connection: close` 生效）、HEAD 不写正文但 `Content-Length` 为真实长度。
- **流水线**：`Content-Length` 之后的剩余字节进入 `m_pending`，写完响应后按序处理下一请求（`tests` 中两个 GET / POST+GET 均通过）。
- **严格化**：缺 Host（1.1）→ 400；重复/非法 Content-Length → 400；POST/PUT/PATCH 缺 Content-Length → 411；`Transfer-Encoding: chunked` → 501；`HTTP/2.0` → 505；头部 431 / URL 414 / 正文 413。
- **解码器增强**：兼容 `Key:value`（无空格）、值去首尾空白、重复 CL 与 chunked 标记、正文按 CL 截断并保留剩余字节。

改动文件：`http_codec.hpp`（重写扩展）、`http_server.hpp`（路由/响应/流水线/校验）、`chat_server.cpp`（method 路由）、`sample_server.cpp` 兼容旧 `route_any`。

用例结构（准备 → 执行 → 断言）：`tests/test_p1_protocol.py` 每条都先 `harness.Server()` 起独立进程、再发原始字节、最后断言状态码/头部/正文/连接是否关闭。

---

## 3. P2 协程架构

**设计**（与设计文档 §4 一致，按实际落地修正）：

- `task.hpp`：`Task<T>` / `Task<void>`；惰性启动、final suspend **对称转移**、`unhandled_exception` 暂存 `exception_ptr`（不 terminate）、顶层任务自毁。
- `co_io.hpp`：`co_await coio::read/write/accept/connect/sleep_for`；全部**投递到事件循环再 resume**（避免在 I/O 回调里同步恢复导致深栈/重入）；`async_event` 作为"发布方/定时器 → 协程"的单次唤醒桥。
- `co_http_server.hpp`：`Task<void> serve(fd)` 线性循环（读请求 → 路由 → 写响应 → keep-alive）；协议校验与回调版同一套规则；整请求 deadline 通过**取消读操作**实现（不是直接 close，避免 awaiter 悬垂）。
- **epoll + ONESHOT 约束**：同一 fd 同一时刻只有一个在途操作；HTTP 请求-响应天然串行，所以 P2 采用"单操作串行"（设计文档 §4.4 的方案 A）。
- `scheduler.hpp`：自建 work-stealing 线程池（本地 deque + 互斥量 + CV，空闲从别人队尾窃取；`--selftest` 验证 200 任务跨 4 线程完成）。**默认请求路径仍是单 I/O 线程**；线程池作为可选能力 + 自检，符合"不做数据竞争冒险"的取舍。
- 构建：`chat_server_co` 单独 `CXX_STANDARD 20`，全局仍 C++17（`set_target_properties`，不动既有目标）。

**调试中发现并修复的真实 bug（已留回归/自检）**：

1. **协程 lambda 捕获悬垂**（ASan：`stack-use-after-scope`）：`[capture](...) -> Task<...>` 的协程帧只保存 closure 的 `this`，closure 临时对象销毁后使用即 UAF。修复：路由注册改为**无捕获函数指针**（`handle_recv` 读全局超时），`--selftest` 用命名协程函数。
2. **长轮询 deadline 取消方式**：直接 `close(fd)` 会让等待中的 awaiter 永挂（协程帧泄漏）；改为 `stop_source` 取消读操作，读回调返回 `-ECANCELED` 后协程正常收尾。
3. **回调版（P0 期间暴露）**：`m_resume` 在调用栈上时重置 `m_request` 会销毁正在执行的回调 → double-free；`io_context` 析构期间 `http_server` 析构回访 `m_ops.erase` → UAF。修复：响应完成统一 `post` 到事件循环；`~io_context` 先置空 `g_instance` 再清容器。

**对照**（同一连接状态机）：

| 维度 | 回调版 | 协程版 |
|---|---|---|
| 连接处理 | `do_read/do_handle/do_write` 三段续体 + `shared_from_this` 保活 | `serve()` 线性 `for` + `co_await` |
| 长轮询 | 全局 hub 等待者 + 定时器回调写响应 | `co_await *event`（publish/超时唤醒） |
| 功能 | —— | P0/P1/P3 同一套 41 条用例全过 |
| 性能 | 24.5k rps | 26.1k rps（同量级） |

---

## 4. P3 性能与交付

- `http_delivery.hpp`：静态资源 FNV-1a ETag、`Last-Modified`、`If-None-Match` → 304（含 `*`）、gzip（zlib，>1KB 且 `Accept-Encoding: gzip`，压缩后更小才用）、`Cache-Control: no-cache`。
- 两版共用 `build_static_response()`，保证逐字节一致。
- 读缓冲 8KB；accept 后 `TCP_NODELAY`；访问日志（方法/路径/状态/字节/耗时）；`/healthz`、`/metrics`（请求/错误/活跃连接/连接总数/字节/超时/sendfile/writev/uptime）。
- **sendfile 零拷贝**（`io_context::async_sendfile`）：未压缩的静态 200 由内核直传；两侧（回调 `write_out_response` / 协程 `sendfile_all`）共用同一套内核原语。
- **writev 写合并**（`io_context::async_writev`）：响应头与正文一次 `writev`（回调 `do_writev` / 协程 `writev_all`）。
- **时间轮**（`timer_context` 重写）：256 桶 × 5ms + overflow multimap，取消只摘除不执行；`duration_to_next_timer()` 推进轮并统一执行到期回调。
- 用例：`test_p3_delivery.py` **11 条**（含 sendfile 字节一致性、sendfile/writev 计数增长）；`test_timer_wheel.cpp` **13 断言**（顺序/取消/溢出迁移/1000 定时器/重复取消）。
- 压测 + 长轮询（§0 表），脚本 `tests/stress.py`，可指定 `CHAT_SERVER_BIN` 跑两版。

**未做的 P3 项**：`Server-Timing` 响应头（UI 脉搏用，未纳入本轮）。

---

## 5. P4 工程化与收尾

- **统一协调脚本 `tools/ci.py`**（用户要求的"自动构建 .py"）：
  - `build`：cmake 配置 + 构建（含 `chat_server_mp`）；
  - `test`：ctest（codec / timer / co_smoke）+ 两版 P0/P1/P3/P4持久化 + 协程自检 + 多进程实验；
  - `asan`：ASan/UBSan 构建并跑同一套用例（两版）；`stress`：两版压测；
  - `ci`：以上四步串联，末尾打印 `summary`，任一步失败非 0。
- **GitHub Actions** `.github/workflows/ci.yml`：push/PR 触发，安装 cmake/g++/python3/zlib/sqlite3，执行 `python3 tools/ci.py ci`。
- **版本**：CMake `VERSION 4.0` + `CO_HTTP_VERSION` 宏；三个二进制 `--version` / `--help`。
- **交付**：`deploy/Dockerfile`（gcc:13 构建 → debian 运行，healthcheck 打 `/healthz`）、`deploy/co_http.service`（systemd，Restart=always / LimitNOFILE）、`deploy/README.md`。
- **SQLite 持久化**（`chat_business.hpp` 内 `store`）：`CHAT_DB=<path>` 时启动加载最近 1000 条、`/send` 入库（WAL）；未设置时保持纯内存。两版共用；`test_p4_persistence.py` 用 sqlite3 直查作为独立事实来源，并验证重启恢复、重启后游标、坏路径降级。
- **多进程 `SO_REUSEPORT` 实验**（`chat_server_mp.cpp`）：应用层抽到 `chat_server_app.hpp` 与单进程共用；父进程 fork N 个 worker、转发 SIGTERM 并回收，`X-Pid` 响应头用于验证分流；定位为架构对比实验，不替换默认部署。`test_p4_multiprocess.py` 验证 ≥2 个 worker 分流、父进程退出无孤儿。

### 5.1 实时推送（SSE / WebSocket）+ 前端 + TLS

- **共用协议件 `realtime.hpp`**：SHA1 / Base64 / `Sec-WebSocket-Accept`、服务端帧编码、客户端帧解析（校验掩码）、SSE 事件与响应头格式化。
- **回调版**：`http_server` 增加流式模式——路由调用 `begin_sse()` / `begin_websocket()` 后连接切到流式：SSE 订阅 `chat_hub` + 心跳、写出 `data:` 帧；WS 先 101、再帧循环（文本回显/发布到 hub、ping→pong、close）。
- **协程版**：`co_response` 增加 `stream=sse/websocket`，`serve()` 分流到 `serve_sse` / `serve_ws`（线性 `co_await` 循环，`write_all_ok` 检测对端断开）。
- **语义**：WS 文本消息既回显，也发布到聊天 hub（SSE/长轮询客户端同时可见）；SSE 使用 `Connection: close` 定界（省去 chunked 编码），心跳可用 `SSE_HEARTBEAT_MS` 配置。
- **指标**：`/metrics` 增加 `sse_total` / `ws_total`。
- **SSE 历史重放**：`GET /events?first=N` 先推从 N 开始的历史（最多 1000 条），再推新消息/心跳；前端先用 `/recv` 拿到游标再连。
- **WS 广播**：`ws_hub.hpp` 维护 WS 订阅者；文本消息回显自己 + 广播给其他 WS 客户端（实现上用"取消阻塞读 → flush 发送队列 → 继续读"，保持单 fd 单操作约束）。
- **前端接线**：`static/index.html` 重写为原生 JS（无 CDN）：先 `/recv` 拉历史 → **WebSocket 优先**（发送/接收/广播）→ 失败自动回退 **SSE**（接收）+ POST /send；状态点/重连退避/localStorage 昵称。
- **TLS**：设置 `TLS_CERT`/`TLS_KEY` 后同端口 HTTPS（OpenSSL，非阻塞握手复用操作槽；TLS 下 sendfile 回退普通写，writev 回退拼接写）。
- **用例**：`test_p4_realtime.py` 7 条 ×2（含 SSE 历史重放、WS 广播给另一客户端）、`test_p5_frontend.py` 3 条 ×2、`test_p5_tls.py` 3 条 ×2、`realtime_tests` 23 断言。

- **测试金字塔**：快速单元测试 `codec_tests`（41 断言）+ `timer_tests`（13 断言）+ `realtime_tests`（23 断言）；适量集成（P0 13 + P1 20 + P3 11 + P4 持久化 3 + 实时推送 7 + 前端 3 + TLS 3 + 多进程 2）；少量端到端（两版压测 + 长轮询）。

**未验证**：Docker 镜像构建（本机无 docker）、GitHub Actions 真实运行（只验证了同一入口在本地全绿）；这两项需要 GitHub/CI 环境才会真正执行。

---

## 6. 需求 → 实现对照

| 要求 | 落点 |
|---|---|
| P1–P4 全部完成 | §2–§5 + §10：SQLite、多进程实验、sendfile/writev/时间轮、SSE/WebSocket、前端接线、WS 广播、SSE 重放、TLS 均已落地；仅 **io_uring 对比** 仍为选做未做（见 §7） |
| 测试与 CI 与连接与压测都过 | `tools/ci.py ci` 四步全 OK；stress 两版 0 错误 |
| 统一设计不独立 | §1：共享内核/codec/business/delivery/metrics；SQLite 与多进程也复用同一份应用层 |
| 每阶段每步有用例 | P0 13 / P1 20 / P2（复用 41 + selftest）/ P3 11 / P4 10 / 单元 77 断言 / e2e stress |
| 修复 bug 留回归 | RST、坏 JSON、EMFILE、slowloris、广播、pipelining、304、gzip、sendfile 等均在 `tests/` |
| 三动作用例 | 所有黑盒用例：启动独立服务器（准备）→ 原始请求（执行）→ 断言状态/头/正文/连接（检查） |
| 大量快速单测 | `tests/unit/test_http_codec.cpp`（41）+ `test_timer_wheel.cpp`（13）+ `test_realtime.cpp`（23 checks），均无网络 |
| 持续集成 bug | `.github/workflows/ci.yml` + `tools/ci.py` |
| 自动构建 .py 统一协调 | `tools/ci.py` |
| 跨平台/特殊场景暂停通知 | 项目依赖 epoll，仅 Linux；Windows 仅编辑代码，已在 WSL2 完成全部验证 |
| git / 回滚 | 仓库已有 git；**本次改动未提交**，`git checkout -- .` + `git clean -fd` 可整体回退 |
| review 后提交 | 见 §8 提交拆分建议 |

---

## 7. 已知边界（需要你确认是否接受）

1. **P4 剩余选做**：仅 io_uring 对比（设计文档标注"选做"）。SQLite、多进程、SSE、WebSocket、前端接线、TLS 已落地。
2. **`Server-Timing`** 未加（P3 可观测项之一）。
3. **调度器只进自检、未进请求路径**：避免与业务状态产生数据竞争；默认请求仍单 I/O 线程。这是刻意取舍，不是遗漏。
4. **多进程实验的聊天状态**：各 worker 内存独立（/send 与 /recv 可能落在不同进程），因此多进程只作为架构对比演示；需要共享状态时应走 SQLite 或独立消息进程。
5. **WSL DrvFs**：`sendfile` 在 `/mnt/d` 上明显慢于 ext4（实测 4.5k vs 26k rps）；生产/CI 使用原生文件系统，压测脚本已在 WSL 下自动切换到 ext4 静态根。
6. **访问日志写 stderr**（未做文件轮转/syslog）；测试下由 harness 捕获。
7. Docker / GitHub Actions 未在真实 CI 环境跑过。
8. 长轮询游标仍沿用"客户端计数"，环形缓冲淘汰后返回 400（原设计的 `next_cursor` 未纳入本轮）。

---

## 8. 建议提交拆分（review 通过后）

1. `fix(p0): 事件分发/取消/异常边界/上限与 deadline/长轮询广播/优雅退出`（P0 文件 + `tests/test_p0_stability.py`）
2. `feat(p1): method+path 路由、HEAD、pipelining、CL 严格化、chunked 501、Date/Connection`（`http_codec`/`http_server`/`chat_server` + P1 用例 + codec 单测 + CMake）
3. `feat(p2): C++20 协程版（Task/awaiter/线性连接/长轮询桥接）+ 工作窃取调度器自检`（新文件 + `chat_server_co` target）
4. `feat(p3): ETag/304/gzip/访问日志/healthz/metrics + sendfile/writev/时间轮 + 压测`（`http_delivery`/`metrics`/`timer_context`/`io_context` + P3 用例 + `timer_tests` + stress）
5. `feat(p4): SQLite 持久化 + 多进程 SO_REUSEPORT + SSE/WebSocket（realtime.hpp + 两版流式模式）`（`chat_business`/`chat_server_app`/`chat_server_mp`/`realtime`/两版流式 + P4 用例）
6. `ci(p4): tools/ci.py + GitHub Actions + Dockerfile/systemd + 版本`（`.github`/`deploy`/`tools`/CMake/README）
7. `docs: P0/P1–P4 阶段总结 + 实现说明 + 架构关系（co_async 复用点）`

---

## 9. 请你 review 的点

- [ ] 两版是否都要保留（默认 `chat_server`，协程 `chat_server_co`）——本方案保留，符合设计 D1/D2
- [ ] P4 剩余选做项（io_uring 对比）是否还需要做（其余均已完成）
- [ ] 调度器只做自检、不进请求路径的取舍
- [ ] 长轮询游标（仍为计数语义）是否可接受
- [ ] 访问日志格式/去向（stderr）与 `/metrics` 字段
- [ ] 提交拆分与提交信息是否按 §8 执行

---

## 10. 现有架构 ↔ 协程版本的关系（以及 co_async 的复用/学习点）

### 10.1 关系：同一内核，替换"控制流"

```text
                 ┌────────────── 共享（两版同一份代码） ──────────────┐
业务语义        │ chat_business.hpp：消息环 / hub / /send /recv / 静态 │
交付            │ http_delivery.hpp：ETag/304/gzip；metrics.hpp        │
协议            │ http_codec.hpp：解析 / 写头 / reason / Date          │
异步内核        │ io_context（epoll + fd 操作槽 + 定时轮 + 取消 + post）│
                 └───────────────────────────┬──────────────────────────┘
                                             │
                     ┌───────────────────────┴────────────────────────┐
            回调版   │ http_server.hpp：do_read/do_handle/do_write     │
                     │ chat_server.cpp：回调路由 + 长轮询 timer 适配   │
                     └────────────────────────────────────────────────┘
                     ┌────────────────────────────────────────────────┐
            协程版   │ co_io.hpp：把操作槽包装成 awaiter               │
                     │ co_http_server.hpp：serve() 线性循环            │
                     │ chat_server_co.cpp：协程路由 + async_event 桥接 │
                     └────────────────────────────────────────────────┘
```

关键判断：**协程化只替换"控制流"，不替换协议/业务/交付**。因此：

- 新增的 sendfile / writev / 时间轮都落在**共享内核**（`io_context` / `timer_context`），两版自动同时获得；
- SQLite 落在**共享业务层**（`chat_business.hpp`），两版自动一致；
- 多进程实验只替换"进程模型"，应用层抽到 `chat_server_app.hpp`，路由与业务仍只有一份。

### 10.2 从 co_async 复用/学习到的点（按实际落地）

| co_async 的做法 | 本项目落地 | 备注 |
|---|---|---|
| `Task` 惰性启动 + final suspend **对称转移** | `task.hpp`：`initial_suspend=suspend_always`、final awaiter 返回 continuation；顶层任务自毁 | 结构照搬、代码自写（仓库无 LICENSE） |
| 返回值/异常不 `terminate` | `unhandled_exception` 暂存 `exception_ptr`，由 await/顶层统一处理 | 比 co_async 默认 terminate 更稳 |
| 事件循环"先收集事件、再统一 resume" | `io_context::join()` 先拷贝 `(fd, events)` 再执行，避免回调里关 fd 影响后续事件 | 直接按此实现了 |
| awaiter 注册到事件循环，完成时 resume | `co_io.hpp` 的 read/write/accept/connect/sendfile/writev 全部走操作槽 + `post` 恢复 | 与回调版共用同一个槽 |
| 定时器"最早就绪循环 + delta 当 timeout" | `timer_context` 时间轮：`duration_to_next_timer()` 推进轮/执行到期/返回 delta | 在此基础上做成哈希轮 + 溢出堆 |
| HTTP"响应前先把未读 body 读干" | P1 的 Content-Length 严格化 + 流水线 leftover 已覆盖 | 未按它的流式 response 实现 |
| `when_all` / `when_any` | 未引入（当前无并发任务组合需求）；`async_event` 已够长轮询 | 留作后续扩展 |
| 工作窃取线程池 | **co_async 本身没有**（`CO_ASYNC_STEAL` 未被实现），本项目自建 `scheduler.hpp` 并只做自检 | 不谎称"照搬" |
| io_uring / 主库平台层 / PMR / 自带 CMake | **不采用**：平台不兼容、会污染既有类型体系 | 只参考结构 |

---

## 11. 本轮 P4 收尾清单（新增证据）

| 项 | 交付 | 用例/验证 |
|---|---|---|
| sendfile 零拷贝 | `io_context::async_sendfile`；回调 `write_out_response`、协程 `sendfile_all` | `test_p3_delivery.py::test_sendfile_serves_exact_file_bytes`（与文件逐字节一致）+ `sendfile_total` 计数 |
| writev 写合并 | `io_context::async_writev`；回调 `do_writev`、协程 `writev_all` | `test_sendfile_and_writev_counters` |
| 时间轮 | `timer_context` 重写（256 桶 × 5ms + overflow） | `test_timer_wheel.cpp` 13 断言；全套 P0/P1/P3/P4 仍过（超时/长轮询依赖它） |
| SQLite 持久化 | `chat_business.hpp` 的 `store` + `CHAT_DB` | `test_p4_persistence.py` 3 条（直查 sqlite 作为独立 oracle、重启恢复、坏路径降级） |
| 多进程 SO_REUSEPORT | `chat_server_app.hpp` 抽公共应用层 + `chat_server_mp.cpp` 父/子进程 | `test_p4_multiprocess.py` 2 条（≥2 worker 分流、父进程退出无孤儿） |
| SSE / WebSocket | `realtime.hpp` + 回调流式模式 + 协程 `serve_sse`/`serve_ws` | `test_p4_realtime.py` 5 条 ×2 版本；`realtime_tests` 23 断言；`sse_total`/`ws_total` 指标 |
| 统一验证入口 | `tests/run_all.sh`（全套）、`tests/run_stress.sh`（两版压测） | `ALL SUITES PASSED`；stress `ok: true` |

**最终验证**：`python3 tools/ci.py ci` → `build / test / asan / stress` 四步 OK（见 §0）。
