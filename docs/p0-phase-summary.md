# P0 阶段总结（稳定性）—— 待 review

> 阶段：S1（P0 稳定性，对应 `docs/server-evolution-design.md` §3.3）
> 状态：**实现完成，测试双绿（Release + ASan/UBSan），未提交，等待 review**
> 基线：`b54f089`（设计文档 v1）；本阶段改动全部在工作区，`git checkout -- .` 可整体回退。

---

## 1. 本阶段目标与思路

**目标**：让单个畸形请求、对端 RST、资源耗尽、信号退出都**不能再终止进程**，同时把"取消/生命周期"的口子堵上，为后面的协程化（P2）留一个干净的内核。

**思路（一个内核、两处收口）**：

1. **把"回调指针"从 epoll 里赶出去**：旧代码把 `callback` 裸地址塞进 `epoll_event.data.ptr`，事件到达时用 `from_address()` 还原；一旦该 fd 上没有在途操作（典型：请求在首次 `read()` 就整包成功、之后的 HUP/RST，或长轮询挂起期间对端复位），就会拿 `nullptr` 还原并调用 → SIGSEGV。现在 `data.fd` + `fd → 操作槽` 查表，查不到就交给 fd 所有者收尾。
2. **取消/完成"恰好一次"**：操作槽三态（注册/触发/取消），取消时先 `EPOLL_CTL_MOD(0)` 解除武装，再把续体投递到延迟队列执行——不在任意调用栈里同步执行用户回调（旧版 `stop_source` 取消会就地执行续体，导致重入）。
3. **异常边界**：事件循环执行续体处统一兜底；业务改用非抛 JSON；accept 的 EMFILE/ENFILE 退避重试；静态文件启动时预加载。
4. **资源上限与 deadline**：头部/URL/正文/连接数上限 + **整请求 deadline**（不再"每读一次重置 10 秒"）。
5. **业务正确性**：消息环形缓冲 + 绝对序号；`chat_hub` 广播唤醒**所有**长轮询（替代全局单槽 `stop_source`）。
6. **可退出**：忽略 SIGPIPE；SIGINT/SIGTERM → eventfd 唤醒事件循环 → `join()` 正常返回 → 退出码 0。

**测试策略（TDD）**：先写黑盒用例（只通过 TCP/HTTP 外部行为断言），在**未修复代码**上跑出 red，再实现到 green；同一套用例最后再用 ASan/UBSan 构建跑一遍。

---

## 2. 用例（新增 `tests/`）

| 文件 | 内容 |
|---|---|
| `tests/harness.py` | 启动/停止服务器、原始 socket 收发、RST 注入、fd 上限注入；`CHAT_SERVER_BIN` 可指向 ASan 构建 |
| `tests/test_p0_stability.py` | 13 个黑盒用例（下表） |
| `tests/run_p0.sh` | 一键验收：Release 构建 + 用例，再 ASan/UBSan 构建 + 同一套用例 |

| 用例 | 对应 P0 项 | 修复前（red 证据） | 修复后 |
|---|---|---|---|
| `test_rst_during_long_poll_keeps_server_alive` | P0-1 | 第 2 次 RST 后进程 `rc=-11`（SIGSEGV） | 存活并继续服务 |
| `test_bad_json_send_returns_400` / `_recv_` / `_bad_long_poll_cursor_` | P0-3 | 进程 terminate（连接无响应/退出） | 400 + 存活 |
| `test_big_header_returns_431` | P0-5 | 返回 200 | 431 |
| `test_big_url_returns_414` | P0-5 | 返回 404 | 414 |
| `test_big_declared_body_returns_413` | P0-5 | 服务器继续等 2MB 正文（超时） | 立即 413 |
| `test_slow_header_connection_is_closed` | P0-5 | 连接一直挂着不关 | deadline（800ms）后关闭 |
| `test_send_wakes_all_waiters_quickly` | P0-6 | 第 1 个等待者 1s 内无响应（单槽只唤醒最后一个） | 3 个等待者 ≤1s 全部返回 |
| `test_write_after_peer_reset_does_not_kill_process` | P0-4 | ——（原本可能偶发 SIGPIPE，现为回归护栏） | 存活 |
| `test_emfile_keeps_server_alive` | P0-3/P0-5 | `terminate: accept: Too many open files`（`rc=-6`） | 退避重试，存活 |
| `test_sigterm_exits_cleanly` | P0-7 | 默认信号动作，`rc=-15` | 优雅退出，`rc=0` |
| `test_two_requests_on_one_connection` | 回归护栏 | 200/200（原本即通过） | 200/200 |

**命令与结果**：`bash tests/run_p0.sh` → Release 13/13 OK；ASan+UBSan 13/13 OK；末尾 `P0 验收通过`。

---

## 3. 修改文件与关键代码

> `git diff --stat`：6 个文件，+1018/−446；另有新增 `tests/`（约 300 行）。

### 3.1 `io_context.hpp`（核心重写，P0-1/P0-2/P0-4/P0-7）

- epoll 只存 `data.fd`；新增 `m_ops: fd → callback<>`（在途操作槽）与 `m_watchers: fd → fd_watcher*`（fd 所有者）。
- `arm()`：登记操作槽 + `EPOLL_CTL_MOD(EPOLLET|EPOLLONESHOT|事件)`；取消令牌回调只做 `cancel_fd(fd)`。
- `cancel_fd()`：从槽取出续体 → `EPOLL_CTL_MOD(0)` 解除武装 → `post()` 投递；续体自己检查 `stop_requested()` 返回 `-ECANCELED`。
- `join()`：`EINTR` 重试；**先整批收集事件、再统一执行**（避免回调关闭后续 fd）；每轮清空延迟队列；`g_quit` 检查。
- `eventfd` 作信号/跨线程唤醒；`~io_context` 先置 `g_instance=nullptr` 再清容器，避免析构期间回调重入 `m_ops`。
- `async_read/write/accept/connect` 保留原回调签名（**P2 的对照基线**），内部改走操作槽；`async_file` 实现 `fd_watcher`，无在途操作时收到事件 → 主动关闭。

### 3.2 `timer_context.hpp`（P0-2）

- 取消定时器 = **从堆里删除、不执行回调**；回调只在真正到期时执行一次。旧行为"取消即执行回调"被移除（它既是重入来源，也让"取消 deadline"变成"触发 deadline"）。

### 3.3 `http_server.hpp`（P0-3/P0-5/P0-7）

- `config`：`max_header=16KB`、`max_url=8KB`、`max_body=1MB`、`max_conn=1024`、`request_timeout_ms=10000`、`write_timeout_ms=10000`、`poll_timeout_ms=3000`；可由环境变量覆盖。
- 连接状态机：`start_request()`（装 deadline）→ `do_read()` → `parse_chunk()`（上限检查）→ `do_handle()`（try/catch）→ `do_write()`（写超时 + 短写续写）→ 投递 `start_request`。
- **响应完成后不直接递归开始下一个请求**，而是 `post(start_request)`：`m_resume` 可能就在本次调用栈上，直接重置 `m_request` 会销毁正在执行的回调对象（这是调试中实际抓到的 double-free/UAF）。
- `fail(status, msg)`：回错误响应后关闭；accept 失败按 `EAGAIN/EINTR/EMFILE/ENFILE` 分类；超过 `max_conn` 直接关闭新连接。
- 连接计数用 `shared_ptr<size_t>`，规避"server 先析构、handler 后析构"的顺序问题。

### 3.4 `chat_server.cpp`（P0-3/P0-6）

- 静态页启动预加载（`index.html` / `favicon.ico`），不再在事件循环里同步读盘。
- `chat_hub`：`deque<Message>` + 绝对序号 `base` + 容量 1000；`publish()` 唤醒所有 `weak_ptr<waiter>`，唤醒动作用 `post()` 投递。
- `/send`：非抛 JSON（失败 400），字段长度校验；`/recv`：游标非法 400，无增量则挂起，超时或 publish 各唤醒一次（`done` 标志保证恰好一次）。
- SIGPIPE 忽略；SIGINT/SIGTERM 处理器只置 `g_quit` + 写 eventfd。
- 环境变量：`PORT`、`STATIC_ROOT`、`MAX_HEADER/MAX_URL/MAX_BODY/MAX_CONN`、`REQUEST_TIMEOUT_MS/WRITE_TIMEOUT_MS/POLL_TIMEOUT_MS`。

### 3.5 两个小修

- `http_codec.hpp:140`：`headers_raw()` 返回类型写错（`std::string&` ← `bytes_buffer&`），此前未被实例化；改为 `bytes_buffer&`。
- `stop_source.hpp`：新增 `explicit operator bool()`（`if (token)` 语义）。
- `.gitignore`：补 `__pycache__/`。

---

## 4. 行为变化（需要你确认可接受）

| 变化 | 说明 |
|---|---|
| 坏 JSON / 坏游标 → **400** | 原来会 terminate 或返回 200 |
| 超限请求 → 431/414/413 | 原来无上限/200/404 |
| 慢连接 → deadline（默认 10s）关闭 | 原来 10s 无数据也会关，但每读一次就重置，可被 slowloris 绕过 |
| `MAX_CONN` 超限 → 直接关闭新连接 | 原来无限接受 |
| SIGTERM → 退出码 0 | 原来是 -15（默认动作） |
| `/send` 增加字段长度限制（昵称 64 / 内容 4096） | 前端未触及上限 |
| 环形缓冲 1000 条 + 游标语义 | 见"已知边界" |

---

## 5. 已知边界（留给 P1/P2，未在本阶段做）

1. **HEAD/Host/Date/method 路由/query/pipelining/chunked** 等协议项仍属 P1（见设计文档 §3.4）。
2. 长轮询仍沿用"客户端自己数消息条数"的游标；消息超过 1000 条且客户端离线过久时，游标会落到已淘汰区间 → 400（前端会停止轮询）。P1 用 `next_cursor` 语义替换。
3. 客户端断开后的"未写响应"仍由连接对象析构收尾；P1/P2 会统一到"响应对象 + 取消"。
4. 事件循环仍是单线程、同步业务（JSON/静态页已预加载，影响很小）。
5. `sample_client.cpp` 有两处既有 `unused parameter` 警告（本次未动）。

---

## 6. 验证方式（可复现）

```bash
cd <repo>
bash tests/run_p0.sh            # Release + ASan/UBSan 两轮，期望末尾 "P0 验收通过"
```

---

## 7. 请你 review 的点

- [ ] 事件分发重构的取舍：`fd→操作槽` + `fd_watcher`（而不是引入第三方事件库）
- [ ] `timer_context` 语义改为"取消不执行回调"——确认没有其他代码依赖旧语义
- [ ] `http_server` 的默认上限值（16KB/8KB/1MB/1024 连接/10s）
- [ ] `/send`、`/recv` 的 400 行为与字段长度限制
- [ ] 环形缓冲 1000 条 + 游标策略（是否接受"过旧游标 400"作为 P1 前的临时行为）
- [ ] 是否同意本阶段文件拆分与测试放置方式（`tests/` 黑盒 + 一键脚本）
