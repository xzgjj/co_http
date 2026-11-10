# co_http



-  co_async 协程看不懂，C++20 不支持、Linux 版本低等问题。


基于小彭老师用，基于 C++17 回调函数的异步 HTTP 服务器的学习与开发







- 编译和运行：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target chat_server
cd build
./chat_server
```

> 小彭老师的编译环境是 GCC 9.3.0 和 Ubuntu 20.04 LTS。

然后，访问 https://127.0.0.1:8080 就能访问聊天界面了。

下一期你想看什么呢？是数据库，https，还是 websocket？欢迎在视频中投票表决。



--------------------------------------------------------------------------------------------------

- **测试**

```
sudo apt update
sudo apt install -y build-essential cmake wrk

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target chat_server -j
```



- **启动服务器**

```
cd build
./chat_server

//新开第2个WSL终端 ;压力测试
wrk -c100 -d3s http://127.0.0.1:8080/
wrk -c100 -d10s http://127.0.0.1:8080/
wrk -t4 -c100 -d10s
```



- 测试结果

```
Latency  139.63ms  ← 每个请求平均耗时（100 条连接在排队，所以数值大；100÷700≈0.14s）
Req/Sec  351.12    ← 每线程每秒 351 个，×2 线程 ≈ 700
2099 requests in 3.01s, 8.68MB read
Requests/sec: 698.49   ← 总数，和上面 Req/Sec×2 对得上
Transfer/sec: 2.89MB
```

-------------------------------------------------------------------------------

- **http**



- 请求

起始行            ← 请求：方法 路径 版本 / 响应：版本 状态码 原因短语
头字段1: 值
头字段2: 值
                  ← 空行！这是"头结束"的唯一标志
body（正文，可为空）



```
> GET / HTTP/1.1
> Host: 127.0.0.1:8080
> User-Agent: curl/8.5.0
> Accept: */*
> 
```

请求行	GET / HTTP/1.1	方法 + 请求目标 + 版本，三段用空格隔开 

路径 /	只写路径（origin-form）	不写  http://... ，域名放Host 头里

 Host	必填（HTTP/1.1 规定）	一台服务器托管多个域名就是靠它区分；本项目忽略它，只看路径

User-Agent	客户端自报	curl 会带，wrk 也带 wrk/4.1.0 

Accept	能接受什么类型	这里 */* = 都行 

空行	表示请求头结束	后面没有 body，所以也没有 Content-Length



```
> POST /send HTTP/1.1
> Host: 127.0.0.1:8080
> User-Agent: curl/8.5.0
> Accept: */*
> Content-Type: application/json
> Content-Length: 35
> 
{"user":"tester","content":"hello"}
```



- 响应

```
HTTP/1.1 200 OK
Server: co_http
Content-type: text/html
Connection: keep-alive
Content-length: 4229

<!DOCTYPE html>
<html>
...
```

- **状态行** = 版本 + 状态码 + 原因短语
