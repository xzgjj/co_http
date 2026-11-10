# HTTP 机制与模块分析



## 1. 项目概览

这是一个 C++17、Linux epoll、单线程回调式异步 I/O 的 HTTP 聊天服务器。没有协程、没有第三方网络库，HTTP 只实现教学所需的最小集合：

- 请求：请求行 + 头部 + `Content-Length` 指定的正文；
- 响应：状态行 + 头部 + 正文，服务器拼接后交给 `do_write` 写出；
- 连接：默认 keep-alive，同一连接可连续处理多个请求；
- 业务：`/send` 发消息，`/recv` 用长轮询拉取消息。



- 构建与运行：


```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target chat_server
cd build
./chat_server          # 监听 0.0.0.0:8080，并 chdir 到 ../static
```

**网络 I/O 系统**：把 socket 设为非阻塞，使用 epoll 判断何时可读、可写、可 accept。

**异步执行系统**：用 `callback` 延后执行后续操作，用 `stop_source` 在 I/O 与定时器之间传递取消/触发信号。

**HTTP 协议系统**：把 socket 收到的原始字节流解析成请求，再把业务响应序列化成 HTTP 字节流。

**HTTP 服务与业务系统**：维护连接、执行路由、处理静态文件和聊天业务。



![PixPin_2026-11-06_16-52-12](D:/图片/md/PixPin_2026-11-06_16-52-12.jpg)



### 文件职责



| 文件 | 职责 |
|---|---|
| `io_context.hpp` | epoll 事件循环、异步文件描述符、DNS 解析、socket 绑定/接受/连接 |
| `timer_context.hpp` | 基于 `std::multimap` 的定时器堆，给事件循环提供最近超时时间 |
| `callback.hpp` | 类型擦除的一次性回调，异步链路的基本单元 |
| `stop_source.hpp` | 取消令牌，用共享控制块在定时器与 I/O 之间二选一 |
| `expected.hpp` | 轻量错误返回：负值表示 `-errno`，0 表示成功 |
| `bytes_buffer.hpp` | 字节视图与拥有型缓冲区，供协议解析和 I/O 复用 |
| `string_map.hpp` | `std::map<std::string, std::string>` 别名，用于 HTTP 头和 JSON 对象 |
| `http_codec.hpp` | HTTP/1.1 头部/正文解析与响应写入 |
| `http_server.hpp` | 服务端：accept、连接状态机、路由、读写循环 |
| `http_client.hpp` | 客户端：URL 解析、连接池、请求序列化、响应解析 |
| `file_utils.hpp` | 静态文件读取，失败时抛 `std::system_error` |
| `reflect.hpp` | JSON 值模型、解析器、编码器，以及 `REFLECT` 宏驱动的结构体编解码 |
| `enum_magic.hpp` | 用 `__PRETTY_FUNCTION__` 在编译期提取枚举名，供 HTTP 方法解析 |
| `chat_server.cpp` | 聊天业务：路由、消息列表、长轮询 |
| `sample_server.cpp` | 最小 HTTP 服务端示例 |
| `sample_client.cpp` | 最小 HTTP 客户端示例 |

---



## 2. HTTP 机制



- 事务（Transaction） **1 次 Request + 1 次 Response = 1 个事务**

-   **Keep-Alive：长连接** **客户端请求长连接 多个事****务复用同一个 TCP 连接。

**连接建立一次   最后统一关闭**

```
(C1) 获取IP地址和端口号 
(C2) 创建新的套接字 (socket)
(C3) 连接到服务器IP：port上去 (connet)

(C4) 连接成功
(C5) 发送HTTP 请求 (write) 
    (C6) 等待HTTP响应 (read)

(C7) 处理HTTP 响应 
(C8) 关闭连接 (close)
```



- **HTTP报文是在HTTP应用程序之间发送的****数据块，**这些数据块以一些文本形式的**元信息开头 ；这些报文在客服端，服务器和代理之间流动**

```C
<起始行>
<头部字段>
<空行>
[消息体]
```



### http服务器



- **轻量级异步 HTTP 服务端**实现：**每连接一个 handler + 读/处理/写状态循环 + 路由分发**”的模型。

监听 TCP 端口，接受连接。

每个连接由一个 `http_connection_handler` 独立管理。

使用异步读写 + 回调续体驱动连接状态机。

增量解析 HTTP 请求，支持 TCP 半包/粘包场景下的“继续读”。

将完整请求交给路由表 `http_router` 精确匹配处理。

响应写完后回到读状态，实现 **HTTP keep-alive**

通过 `stop_source` 实现读超时与取消。

通过 `shared_ptr` + 自引用回调保证异步操作期间对象生命周期安全。





### http客服端



- **极简异步 HTTP/1.1 客户端**，核心目标：**连接池 + 每连接一个 handler + 写请求→读响应→回调**”的模型。

解析 URL，拆出 scheme / hostname（含端口）/ path。

按 `scheme + hostname` 维护**连接池**，同 key 复用连接（客户端侧 keep-alive）。

把 `http_request` 序列化成 HTTP 请求报文。



异步 connect / write / read。

增量解析响应，`Content-Length` 收满后组装 `http_response`。

通过 `callback<expected<int>, http_response const&>` 把结果交回调用方。

通过 `stop_source` 支持外部取消



- 与server**是“结构对称、方向相反”的镜像实现，但不是完全对称的复用**。两者共享同一套抽象（`async_file`、`expected`、`stop_source`、`callback`、`*_parser` / `*_writer`、`enable_shared_from_this`），但面向的协议方向不同：

server：**读请求 → 解析 → 路由 → 写响应**

client：**写请求 → 读响应 → 解析 → 回调**

```
客户端	                          服务端
m_req_writer（写请求）	 m_res_writer（写响应）
m_res_parser（解析响应）	m_req_parser（解析请求）
m_cb（完成回调）	        m_router + m_resume（路由 + 续体）
m_stop（取消）	         stop_io / stop_timer（读超时取消）
m_parsed_url	       无（服务端不需要）
```







### 2.1 一条请求的完整路径

- 服务端处理一个 HTTP 请求经过四步：


1. `http_server::do_accept` 接受连接，创建 `http_connection_handler`；
2. `do_start` 把 fd 包成 `async_file`，进入 `do_read`；
3. 读满一个请求后，`do_handle` 填入 `http_request`，交给路由器；
4. 路由回调调用 `write_response`，经 `do_write` 写回，然后回到 `do_read` 等待下一个请求。

整条链路没有阻塞调用。每一步只注册回调，真正的推进发生在 `io_context::join()` 的事件循环里。

![PixPin_2026-11-06_16-51-28](D:/图片/md/PixPin_2026-11-06_16-51-28.jpg)





### 2.2 事件循环

- `io_context` 持有 epoll fd 和一个回调计数 `m_epcount`。`join()` 的循环体是：

1. `duration_to_next_timer()` 取出最近的定时器超时时间，没有定时器时为负值；
2. 用 `epoll_pwait2`（可用时）或 `epoll_pwait` 等待 I/O 事件，超时时间就是上一步的结果；
3. 对每个事件，把 `event.data.ptr` 还原成 `callback<>` 并调用，然后 `--m_epcount`；
4. `is_empty()` 同时检查“没有定时器”和“没有在途回调”，两者都空时循环退出。



- `async_file` 负责把一次异步操作挂到 epoll 上：

构造时设置 `O_NONBLOCK`，并用 `EPOLLET` 注册 fd；

发起读/写/accept/connect 时，先直接尝试一次系统调用；

返回 `EAGAIN` 才用 `EPOLL_CTL_MOD` 注册 `EPOLLIN`/`EPOLLOUT` + `EPOLLERR` + `EPOLLET` + `EPOLLONESHOT`，把回调地址写进 `event.data.ptr`，并 `++m_epcount`；

事件触发后，回调执行一次，`EPOLLONESHOT` 自动失效，避免同一事件重复触发。

- 定时器与 I/O 在同一个循环里竞争。`set_timeout` 把回调插入 `m_timer_heap`；`duration_to_next_timer` 发现队首已过期时，先摘掉定时器再调用回调。因此“超时”和“I/O 完成”谁先到，谁就执行。


![PixPin_2026-11-06_16-50-31](D:/图片/md/PixPin_2026-11-06_16-50-31.jpg)





### 2.3 请求解析

解析分成两层。

`http11_header_parser` 只管头部：

- `push_chunk` 把新数据追加到 `m_header`，从上次位置往前回溯 4 字节查找 `\r\n\r\n`；
- 找到后标记 `header_finished`，把分隔符之后的字节保存为 `m_body`，再截断头部并调用 `_extract_headers`；
- `_extract_headers` 解析请求行和每一行 `键: 值`，键统一转成小写。

`_http_base_parser` 在头部完成后接管正文：

- 从头部取 `content-length`；没有该字段时视为 0；
- `body_accumulated_size >= content_length` 时标记 `m_body_finished`；
- 后续到达的字节直接追加到 `body()`，处理 TCP 粘包和分片。

因此 `request_finished()` 的含义是“头部已结束且正文收满”，不是“socket 可读”。每轮请求结束后必须调用 `reset_state()`，否则上一轮的头部、正文和长度会污染下一次解析。



### 2.4 响应写回与 keep-alive

`http_response_writer` 把状态行、头部、空行和正文依次追加到 `bytes_buffer`。`http_request::write_response` 固定写入：

- `Server: co_http`
- `Content-type`
- `Connection: keep-alive`
- `Content-length`

写完后调用 `m_resume()`，进入 `do_write`：

- `async_write` 可能只写出一部分，`do_write` 用 `subspan(n)` 递归写剩余部分；
- 全部写完后 `m_res_writer.reset_state()`，再次调用 `do_read()`。

这就是 keep-alive 的实现：响应写完不关连接，而是回到读状态。连接只会在读超时、读错误或对端关闭时释放。



### 2.5 长轮询

`/recv` 有三条路径：

1. **立即返回**：`messages.size() > params.first`，直接切出 `[first, end)` 的消息并编码返回；
2. **挂起等待**：没有新消息时，`set_timeout(3s, ...)` 注册一个定时器，回调持有当前 `http_request` 的引用；
3. **被唤醒**：`/send` 追加消息后调用 `recv_timeout_stop.request_stop()`，触发定时器注册的停止回调；该回调把定时器从堆里摘掉并立即执行，返回新消息。

计时器回调与 `/send` 的 `request_stop()` 是同一个回调的两个触发源。`timer_context::set_timeout` 在停止回调里先 `erase` 再执行，保证两者不会重复触发。

注意：`chat_server.cpp` 里 `recv_timeout_stop` 是全局单例，而 `stop_source` 只有一个回调槽位。多个客户端同时挂起时，后注册的回调会覆盖先注册的；先挂起的请求不会被 `/send` 立即唤醒，只能等 3 秒超时。

---



## 3. Skeleton：五个核心模块



### M1 事件循环与定时器

文件：`io_context.hpp`、`timer_context.hpp`

职责：把 fd 事件和定时器事件收敛到一个循环；向上提供 `async_read`、`async_write`、`async_accept`、`async_connect`。

关键类型：

- `io_context`：epoll fd、回调计数、`join()`；
- `async_file`：fd 的 RAII 包装，析构时 `EPOLL_CTL_DEL` 并 `close`；
- `address_resolver` / `address_info`：`getaddrinfo` 结果的生命周期管理；
- `timer_context`：`multimap<time_point, {callback, stop_source}>`。

依赖：`callback`、`stop_source`、`expected`、`bytes_buffer`。

设计取舍：

- 单线程，无锁；
- 回调地址直接存进 `event.data.ptr`，省去 fd 到回调的查表；
- 定时器用 `multimap` 而不是最小堆，插入 `O(log n)`，取最早 `O(1)`，实现简单。

隐含前提：回调对象在事件触发前必须一直存活。代码靠 `stop_source` 和 `unique_ptr` 的地址转移来保证；取消路径会直接调用仍挂着的那份回调。



### M2 异步控制原语

文件：`callback.hpp`、`stop_source.hpp`、`expected.hpp`

职责：定义整条异步链路的调用约定。

- `callback<Args...>`：类型擦除的 `unique_ptr<_callback_base>`。`operator()` 调用后立即把 `m_base` 置空，因此一个回调只能调用一次；需要多次调用时显式传 `multishot_call`。
- `stop_source`：共享控制块。默认构造为空，`request_stop()` 是 no-op；`make()` 才分配控制块。`set_stop_callback` 是单槽覆盖，后设置的回调替换前一个。
- `expected<T>`：`m_res < 0` 表示 `-errno`，0 表示成功；`expect()` 失败时抛 `std::system_error`。

这三者是所有模块的共同词汇。理解“回调只能调用一次”和“stop_source 单槽”是读懂后续状态机的前提。



### M3 HTTP 编解码与缓冲区

文件：`http_codec.hpp`、`bytes_buffer.hpp`、`string_map.hpp`

职责：在字节流和 HTTP 消息之间转换。

- `bytes_const_view` / `bytes_view`：非拥有视图，只保存指针和长度；`subspan` 越界抛异常，长度超出剩余部分时截断。
- `bytes_buffer`：拥有 `std::vector<char>`；`append` 可能扩容，使之前的视图全部失效。
- `http11_header_parser`：头部解析和头键小写化。
- `_http_base_parser`：`Content-Length` 累积和完成判断。
- `http_request_parser` / `http_response_parser`：在基础解析器上增加方法/URL 或状态码。
- `http_request_writer` / `http_response_writer`：把消息写进 `bytes_buffer`。

边界：只支持 `Content-Length`，不解析 `Transfer-Encoding: chunked`；没有 `Content-Length` 的请求正文按 0 处理。



### M4 服务端连接状态机

文件：`http_server.hpp`（`http_client.hpp` 为对称实现）

职责：管理监听 socket、每个连接的生命周期、路由分发。

`http_connection_handler` 的状态流转：

```text
do_start -> do_read -> (request_finished) -> do_handle -> write_response
   ^                                                          |
   +---------------------- do_write <- m_resume <-------------+
```

关键点：

- `do_read` 同时注册 10 秒空闲定时器和读回调，谁先完成谁取消对方；
- `do_handle` 把解析结果移动到 `http_request`，设置 `m_res_writer` 和 `m_resume`，然后重置解析器；
- 路由回调通过引用拿到 `http_request`，可以在长轮询中延后调用 `write_response`；
- `m_resume` 捕获 `shared_from_this()`，保证响应写完前 handler 不会被析构。

`http_client` 使用同一套 codec 和 `async_file`，但方向相反：先序列化请求，再解析响应；连接按 `scheme + hostname` 复用。



### M5 JSON 反射与聊天业务

文件：`reflect.hpp`、`chat_server.cpp`

`reflect.hpp` 的目标是让普通结构体直接变成 JSON：

- `REFLECT(user, content)` 生成成员模板 `REFLECT__MEMBERS(ReflectorT &)`，对每个字段调用 `reflector("字段名", 字段)`；
- `ReflectorJsonEncode` 在编码时写 `"字段名":值`，并处理逗号；
- `ReflectorJsonDecode` 按字段名查 `JsonValue::Dict`，缺失字段收到 `Null`，类型不符时记录 `JsonError`；
- `JsonTrait<T>` 是分派核心：主模板未特化时 `static_assert`；容器、字符串、算术类型、指针、`optional`、`variant` 分别继承对应的 `JsonTraitXxxLike`；带 `REFLECT` 的结构体通过 SFINAE 检测 `reflect_members`，继承 `JsonTraitObjectLike`；
- `json_encode` 是唯一编码入口；`json_decode` 有四个重载，带 `error_code` 的返回 `bool`，不带的失败时抛 `std::system_error`。

`chat_server.cpp` 在这一层之上只做三件事：

- `Message` / `RecvParams` 用 `REFLECT` 声明可序列化字段；
- `/send` 解码消息、追加到全局 `messages`、唤醒长轮询；
- `/recv` 比较 `messages.size()` 与客户端持有的 `first`，决定立即返回还是挂起。

---



## 4. 情景分析



### S1 打开首页 `GET /`

1. `do_accept` 接受连接，创建 handler，`do_start` 注册 10 秒读超时；
2. `async_read` 读到 `GET / HTTP/1.1\r\n...\r\n\r\n`；
3. 头部解析器在 `\r\n\r\n` 处结束，`Content-Length` 缺失，正文长度按 0 处理，`request_finished()` 为真；
4. `do_handle` 找到 `/` 路由，`file_get_content("index.html")` 读取静态文件；
5. `write_response(200, html, "text/html")` 拼出响应并调用 `m_resume`；
6. `do_write` 写完 4229 字节（本仓库的 `index.html`）后重置 writer，回到 `do_read`。



### S2 发送消息 `POST /send`

1. 请求头和正文分片到达，`_http_base_parser` 按 `Content-Length` 累积，正文收满后进入路由；
2. `json_decode<Message>(request.body)` 解出 `{user, content}`；
3. 追加到 `messages`；这一步没有上限，消息列表只增不减；
4. `recv_timeout_stop.request_stop()` 唤醒挂起的长轮询；
5. 重新创建 `stop_source`，给下一轮长轮询使用；
6. 返回 `200 OK`，连接回到 `do_read`。



### S3 立即返回的 `POST /recv`

1. 解码出 `first`；
2. 如果 `messages.size() > first`，用迭代器切出新增消息；
3. `json_encode` 成数组，`write_response` 返回；
4. 不注册定时器，连接立即回到读状态。



### S4 长轮询超时

1. `messages.size() <= first`，执行 `set_timeout(3s, cb, recv_timeout_stop)`；
2. 回调捕获 `&request` 和 `params`，定时器挂进 `m_timer_heap`；
3. `join()` 计算最近超时时间，`epoll_pwait` 最多等 3 秒；
4. 没有 I/O 事件时，`duration_to_next_timer` 发现定时器过期，先清掉 stop 回调、摘除定时器，再执行回调；
5. 回调重新检查 `messages`，把这段时间新增的消息编码返回；没有新消息则返回 `[]`；
6. `write_response` 走正常的写回路径。

### S5 长轮询被新消息唤醒

1. 客户端 A 挂起在 `/recv`，定时器已注册，`recv_timeout_stop` 里保存着停止回调；
2. 客户端 B 的 `/send` 追加消息后调用 `request_stop()`；
3. 停止回调把定时器回调从 `_timer_entry` 中 move 出来，`erase` 定时器，然后立即调用；
4. 定时器回调按 `first` 切出新消息，写回 A 的连接；
5. B 的 `/send` 自己继续写 `OK` 响应。

两条连接各自写各自的响应，互不阻塞。



### S6 keep-alive 空闲 10 秒

1. `do_read` 创建 `stop_io` 和 `stop_timer`，注册 10 秒定时器后发起读；
2. 定时器先到：`stop_io.request_stop()` 触发读回调的停止分支，读请求以 `-ECANCELED` 结束，用户回调看到错误后直接返回；
3. 读先到：回调第一件事是 `stop_timer.request_stop()`，把定时器摘掉；
4. 对端正常关闭时 `read` 返回 0，回调直接返回，handler 的引用计数归零，`async_file` 析构时关闭 fd 并从 epoll 删除。



### S7 异常路径

- **非法或空 JSON**：`json_decode` 抛 `std::system_error`，`chat_server.cpp` 没有捕获，异常逃出事件循环，进程终止。复现：`GET /recv`（无正文）或 `POST /recv` 带 `{"first":"x"}`。
- **长轮询期间客户端 RST**：fd 仍保留构造时的注册状态，`event.data.ptr` 为 `nullptr`；事件循环把空指针还原成回调并调用，Release 下解引用空指针崩溃，Debug 下断言失败。
- **客户端正常断开**：`read` 返回 0，回调返回，handler 析构，连接清理，不影响其他连接。

---



## 5. 跨文件不变式

1. **回调只能调用一次**：`callback::operator()` 执行后立即置空；需要复用必须传 `multishot_call`（路由回调就是这样做的）。
2. **stop_source 单槽**：`set_stop_callback` 会覆盖旧值；同一个 `stop_source` 不能同时服务多个等待者。
3. **视图不拥有内存**：`bytes_view` / `bytes_const_view` / `std::string_view` 只在底层 buffer 不变时有效；`append` 或 `resize` 之后旧视图立即失效。
4. **解析器每轮重置**：`http_request_parser::reset_state()` 必须在处理完一个请求后调用，否则下一轮会读到上一轮的残留。
5. **Content-Length 是完成条件**：没有该头部时正文长度按 0 处理；服务端不支持 chunked。
6. **expected 的符号约定**：负值是 `-errno`，0 是成功；`expect()` 只在不可恢复错误时抛出。
7. **对象保活**：所有 handler 继承 `enable_shared_from_this`，异步回调捕获 `self`，避免回调执行前对象被析构。
8. **隐式头文件依赖**：`http_client.hpp` 使用 `async_file` 和 `address_resolver` 却没有包含 `io_context.hpp`，使用方必须先包含 `io_context.hpp`（见 `sample_client.cpp`）。

---



## 6. 已知边界

- 只支持 HTTP/1.1 的 `Content-Length` 正文；不支持 chunked、TLS、IPv6 字面量 URL。
- `chat_server.cpp` 的 JSON 解码没有做输入校验和异常捕获，一个格式错误或空正文的请求会让整个进程退出。
- `async_file` 构造时以 `EPOLLET + data.ptr = nullptr` 注册 fd；如果 fd 在重新注册前收到事件（典型场景是长轮询挂起时客户端 RST），事件循环会调用空回调。
- `recv_timeout_stop` 是全局单例，多个并发长轮询只有最后注册的能被立即唤醒，其余退化为 3 秒超时。
- `http_client` 读到 EOF（`n == 0`）时直接返回，不调用完成回调，调用方可能永远等不到结果。
- `http_client::http_request::headers` 没有参与请求序列化，自定义头不会发送。
- `static_bytes_buffer` 使用 `std::array` 但没有包含 `<array>`，只在其他头文件先引入时才能编译。
- `http_response_writer::begin_header` 固定写 `"OK"`，所以 404 的状态行是 `HTTP/1.1 404 OK`，只有状态码和正文能表达错误。
- 静态文件在事件循环线程里同步读取，文件较大时会阻塞其他连接。
- `README.md` 写的是 `https://127.0.0.1:8080`，实际服务是 HTTP，且监听 `0.0.0.0:8080`。
- `reflect.hpp` 中有若干未实例化的分支：`JsonTraitDictLike::putValue` 调用 `putString` 的参数个数不对，`JsonTrait<JsonValue>::putValue` 的参数类型不匹配，`JsonTraitVariantLike::getValue` 直接返回 `NotImplemented`。一旦真正使用这些分支会编译失败或解码失败。
- `enum_magic` 依赖 `__PRETTY_FUNCTION__`，假定枚举值从 0 连续、最大不超过 255；`parse_enum` 找不到名字时返回 `-1`。
- `messages` 无限增长，没有裁剪或分页。
