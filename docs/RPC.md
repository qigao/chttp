# CRPC

CRPC 是建立在 CHTTP 之上的有界 JSON-RPC 2.0 client/server toolkit：

```text
CMeta method semantics + CSerde params/result
    -> CRPC JSON-RPC envelope and deadline
    -> CHTTP POST and HTTP limits
    -> CNet connection
    -> NativeIO terminal completion
```

RPC 客户端编入 `CHttp::Client`，入口为 `<http_client/rpc.h>`；
RPC 服务端编入 `CHttp::Server`，入口为 `<http_server/rpc.h>`。
接口为纯 C，调用方须使用对应头文件和目标重新编译。

## 当前能力

- 编码 JSON-RPC 2.0 request，并严格校验 result/error response envelope；
- 面向普通用户的 `crpc_client_init()`、`crpc_request_reply()` 与
  `crpc_client_destroy()`，内部自行推进 HTTP/CNet；
- 面向 Executor、Actor 和事件循环的 `crpc_async_client_*` submit/cancel/poll 接口；
- 每次调用可显式选择明文或 TLS transport，以及 HTTP/1.1 或 HTTP/2；
- `crpc_server_*` 后台 server，按 `target + service.name` 注册多个 endpoint/method；
- 严格处理 unary call、notification 与有序 batch，并抑制 notification response；
- 使用 `uint64_t` request id，拒绝同一个 client 中重复的 active id；
- 通过 CSerde token writer 编码 Array/Map params，通过 single-pass reader 消费 result 或 error
  data；
- 可选绑定 `cmeta_callable`，把 signature、effects 和 properties 与本地 method 语义一起交付；
- 区分 JSON-RPC remote error 与 transport、HTTP、deadline、decode、envelope failure；
- generation-checked request handle、显式 cancel、exactly-once terminal callback；
- request count、method bytes、JSON depth、HTTP headers/body 和底层网络资源均有硬上限。

当前实现 JSON-RPC over HTTP POST 的 unary client/server、notification 与 batch；不提供 streaming、
自动 retry、service discovery 或 HTTP/3。TLS、ALPN、H1/H2 与连接能力由 CHTTP/CNet 实现，CRPC
只组合协议语义与 transport 选择，不私自建立第二套 socket runtime。

## CMeta 与 CSerde 的职责

`crpc_method` 的 `service` 和 `name` 决定 wire method；可选 `cmeta_callable` 描述本地 callable 的
signature、调用协议、effects 和 properties。CRPC 在 admission 期间复制并绑定 metadata，因而
response 读取的是稳定的 method semantic snapshot。CRPC 不用 CMeta 猜测 wire schema，也不在
运行时调用该 callable。

params encoder 必须向 `cserde_writer` 写入恰好一个 Array 或 Map root；CRPC 负责结束 writer，调用
方不能自行 finish。`crpc_response_view` 中的 result reader 与 remote error message/data 只在异步
completion callback 内有效；`crpc_request_reply()` 返回的 owning `crpc_response` 则保持这些
reader/view 有效，直到调用 `crpc_response_destroy()`。两种 reader 都是单遍读取。

server 注册时复制 target、wire method 与已绑定的 `cmeta_callable`，handler/user 则借用到
`crpc_server_destroy()`。`crpc_server_request_view`、HTTP request、params reader 与 callable pointer
仅在 method handler 返回前有效。result/error encoder 在 `crpc_server_response_*()` 调用期间同步
执行并立即生成有界响应，不保留 encoder 或 user pointer；result 可以是任意 JSON value，NULL
encoder 表示 JSON null。

## 两种调用风格

普通业务代码采用 request/reply：先以完整的 `crpc_client_config` 调用 `crpc_client_init()`，再把
`connection_uri`、`authority`、`target`、method、request id、deadline、可选 `tls` profile 与
`protocol` 放入 `crpc_options`，交给 `crpc_request_reply()`；消费结果后调用
`crpc_response_destroy()`，最终调用 `crpc_client_destroy()`。零初始化 `protocol` 保持明文
HTTP/1.1 默认；h2c 使用 `tcp://` 与 `CHTTP_HTTP_2`，TLS H1/H2 使用 `tls://`、匹配 ALPN 的
`chttp_tls_profile` 与对应 protocol。profile 在调用期间是 borrowed，其初始化、销毁与并发请求
保活语义遵循 CHTTP 契约。

同一个 client 可以保持 `connection_uri` 与 `authority` 不变，只替换 `target` 和 `method`，依次
调用同一站点的多个 endpoint。CRPC 直接继承 CHTTP 的有界 H1/H2 connection pool：服务器
允许持久连接且 pool key 一致时，这些 request/reply 可以复用 transport；服务器返回
`Connection: close`、连接超时、解析失败、取消或 shutdown 时则关闭并回收。pool key、容量、
背压与线程归属均由 CHTTP/CNet 定义，CRPC 不维护第二份连接状态。复用 client 不保证每次请求
落在同一条 TCP 连接；TLS profile identity、协议、origin、并发与连接健康状态都可能选择不同 slot。

需要并发、取消或统一 progress owner 时采用 `crpc_async_client_init()`、
`crpc_async_client_submit()`、`crpc_async_request_cancel()`、`crpc_async_client_poll()`、
`crpc_async_client_stop()` 与 `crpc_async_client_destroy()`。业务层不应为了发起一次 RPC 而调用
poll。

## 状态、deadline 与错误

`crpc_async_client_submit()` 成功表示 request 已被 CHTTP 接受，并保证以后恰好一次 terminal
callback；立即 admission 失败不产生 callback。`request_id` 是 JSON-RPC wire identity，
`crpc_request` 是本地 generation-checked cancellation handle，二者不能混用。

RPC `deadline_ms` 从 call 开始计时，并约束编码、HTTP 和网络等待；CNet 的 connect/read/write
timeout 仍是独立 transport deadline。RPC deadline 到期后只请求下层取消，payload、parser、
callback state 和 handle 必须保留到 CHTTP/CNet/NativeIO 给出 terminal result 后才能回收。

`crpc_error` 保留四类上下文：Salts status、native status、HTTP status 与 stable stage。
合法的 JSON-RPC error object 以 `CRPC_RESPONSE_REMOTE_ERROR` 返回，不伪装成 transport failure。
metadata 不能覆盖 CRPC 自己拥有的 Content-Type、Accept、Host、Content-Length 或 Connection。

一个 async client 是单 progress owner。submit/cancel/poll/stop/destroy 不得并发；callback 可以
取消另一条 active request，但不能递归 submit/poll/stop/destroy。`crpc_async_client_poll()` 是
框架级异步入口，适合由 RPC runtime、Executor 或 event-loop adapter 统一驱动。一个 blocking
client 同一时刻只接受一个 request/reply；并行同步调用应使用多个独立 client。

连接复用不等于 pipelining 或 retry：每条 HTTP/1.1 连接同一时刻仍只有一个 RPC，且任何已接受
send 之后的断线都按原错误路径交付，不自动重放可能带副作用的 method。

## Server、endpoint 与 batch

`crpc_server_init()` 创建 stopped server，并一次性分配 method registry。用
`crpc_server_register(server, target, method, handler, user)` 注册 method；同一个 target 可注册多个
method，同一个 server 也可注册多个 target。注册键是完整的 `target + wire method`，重复键返回
`SALTS_EALREADY`，达到 `method_capacity` 返回 `SALTS_ENOBUFS`。CRPC endpoint 必须是固定
origin-form path，不接受 CHTTP 的 `:segment` 动态 route pattern；注册只允许发生在 start 前。

`crpc_server` 拥有内部 `chttp_server`。`crpc_server_http()` 返回 borrowed pointer，供 start 前安装
CHTTP middleware、session 或非 RPC route；listener、worker、TLS 和 H1/H2 生命周期仍统一由
`crpc_server_start/stop/destroy` 管理。handler 在 CHTTP 单 owner worker 上顺序运行，不应阻塞，也
不能从 handler 内 stop/destroy server。

普通 call 必须恰好调用一次 `crpc_server_response_result()` 或
`crpc_server_response_error()`；第二次完成返回 `SALTS_EALREADY`，没有完成或 handler 失败转换为
JSON-RPC `-32603`。notification 执行已注册 handler，但所有 success/error response 都被抑制；单个
notification 或全 notification batch 返回 HTTP 204 空 body。

batch 必须是非空数组且元素数不超过 `max_batch_items`。元素按输入顺序 dispatch，返回项也保持
顺序；notification 不占 response slot，非法元素产生 id 为 null 的 `-32600`。合法 JSON-RPC
result/error 使用 HTTP 200 与 `application/json`；parse、invalid request 和 method-not-found 分别
使用 `-32700`、`-32600`、`-32601`。完整 request/response、JSON depth、batch item 与 method registry
都由 config 的硬上限约束。CRPC server 要求显式设置非零
`http.max_buffered_response_body_bytes`。init 会按 `max_batch_items` 验证总预算足以容纳每项最坏内置
协议错误与 JSON array framing；实际 batch 再从该总预算导出相同的 per-item quota。某个 handler 的
result/error 超过 quota 时只把该项转换为保留原 id 的 `-32603`，后续元素仍会执行，最终响应始终是
合法 JSON array；实现不会退化成无界暂存。

完整可编译的注册、handler、start/stop 示例见
[`examples/crpc_server_example.c`](../http_server/examples/crpc_server_example.c)。`init` 会拒绝无效或溢出的容量；
`register` 还可能返回 `SALTS_EBUSY`、`SALTS_EALREADY`、`SALTS_ENOBUFS`，response helper 会传播
encoder/容量错误且第二次完成返回 `SALTS_EALREADY`；`stop` 超时或底层 transport 失败时不得直接
destroy，调用方应按返回码继续完成 shutdown。

## 构建与验证

Windows 命令需要先进入 Visual Studio x64 开发环境：

```text
cmake --fresh --preset win-release-user
cmake --build --preset win-release-user --target \
  crpc_json_test crpc_api_test crpc_transport_test crpc_server_test crpc_header_cpp_test
ctest --preset win-release-user -R "^crpc_" --output-on-failure
```

测试覆盖 bounded codec、UTF-8、uint64/int64 数值边界、malformed envelope、remote error、真实
HTTP round trip、h2c、TLS H1/H2、同站点多 endpoint request/reply、owning response、CMeta callable
metadata、deadline、重复 id、manual cancel、callback reentrancy、server lifecycle/middleware/session、
notification suppression、ordered batch 与协议错误映射。
