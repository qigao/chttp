# CHTTP

CHTTP 是建立在 CNet 之上的有界 HTTP/1.1 与 HTTP/2 client/server：

```text
Application / RPC
    -> CHTTP H1 serializer/llhttp session 或 H2 frame/HPACK/session
    -> CNet connection -> NativeIO network terminal completion
    -> CFlow shared file runtime -> NativeIO file terminal completion
```

CHTTP 随 HTTPServices 正常构建，source-tree target 是 `salts_chttp`；安装后通过
`Salts::CHTTP` 与 `<chttp/chttp.h>` 使用。llhttp 是基础依赖，不再由单独的 manifest
feature 控制。

CHTTP 当前 library version 是 2.2.0，ABI major/SOVERSION 是 2。Unix 安装包提供
`libsalts_chttp.so.2` SONAME，Windows 产物为 `salts_chttp-2.dll` 与
`salts_chttp-2.lib`；CMake 消费者仍只链接稳定的 `Salts::CHTTP` target。版本化产物名阻止
ABI 1 程序意外装载含新公开结构布局的 ABI 2 动态库，但源码使用者仍必须用匹配头文件重新编译。

## 当前能力与明确边界

当前实现提供：

- 面向普通用户的阻塞 `chttp_get()`、`chttp_head()`、`chttp_post()`、`chttp_put()`、
  `chttp_delete()` 和 `chttp_patch()`，内部自行推进 I/O；
- HTTP/1.0、HTTP/1.1 response 的严格增量解析；
- GET、HEAD、POST、PUT、DELETE、PATCH、OPTIONS request serialization；
- 固定长度、chunked、EOF-delimited body，以及有界 1xx informational response；
- TCP、TLS 和本地 Pipe ordered byte stream；
- 同一 `connection_uri + authority + TLS profile identity` 的有界 HTTP/1.1 keep-alive
  连接复用；
- 显式 `CHTTP_HTTP_2` client，支持 h2c prior knowledge、TLS ALPN `h2`、多 stream 复用、
  SETTINGS、flow control、RST_STREAM cancel 与 GOAWAY drain；
- 供 RPC、Executor 和既有 event loop 集成的高级 submit/cancel/poll 接口；
- H1/H2 共用的有界 body source/sink、同步文件上传与原子文件下载；
- 用户不接触 poller 的后台 HTTP/1.1/HTTP/2 server；同一 route、middleware、Session 和
  GET/HEAD/POST/PUT/DELETE/PATCH/OPTIONS handler API 同时服务两种协议；
- route-scoped 流式 request sink、服务端 response source 与文件响应；
- HTTP/1.1/H2 handler 可交出 generation-checked deferred response，在应用 worker 完成后从任意线程
  复制提交响应并唤醒 owner；
- 同一个显式 WebSocket route 同时支持 HTTP/1.1 Upgrade 与 RFC 8441 HTTP/2 Extended CONNECT，
  并提供无需用户驱动 poller 的 `ws://`/verified `wss://` 同步 client；两种协议继续复用
  全局/路由 middleware、Session 与同一 CNet WebSocket engine；
- 按注册顺序执行的全局/路由 middleware，以及只能调用一次的 `chttp_server_next_call()`；
- 使用系统 CSPRNG session id 的有界内存 Cookie Session，支持 get/set/remove/clear/invalidate；
- start-line、header count、header bytes、request body、response body、request count 的硬上限。

Server 的大型 payload 不在初始化时按槽位乘最大消息尺寸预留。请求体、缓冲响应体、H1/H2
发送缓冲和 WebSocket 暂存区在需要时增长，合计受 `buffer_capacity_bytes` 硬限制；超过预算返回
`SALTS_ENOBUFS`。零值按旧配置的逻辑最坏值计算预算，但仍不做物理预分配。可通过
`chttp_server_get_stats()` 观察当前/峰值字节和预算拒绝次数。

当前有意不提供 HTTP/3、WebSocket extension/subprotocol、通用 CONNECT route、redirect、
compression、proxy、multipart/Range、deferred streaming body 或自动 retry。
H1 client serializer 发送
`Connection: keep-alive`；final response 只有在 llhttp 判定协议允许持久连接时才回到池中，
`Connection: close`、EOF framing、解析失败、取消和 shutdown 都会关闭该连接。UDP/datagram 在
admission 前返回 `SALTS_ENOTSUP`。

当前 server 对 HTTP/1.0 的 `Expect`/`Upgrade` 字段按协议忽略；HTTP/1.1 处理
`Expect: 100-continue`，并只允许显式 WebSocket route 接受 RFC 6455 Upgrade。普通 GET 访问
WebSocket route 返回带 `Sec-WebSocket-Version: 13` 的 426；其他 route 不会被隐式转换成
WebSocket endpoint。
request target 必须是无 fragment 的 origin-form；Transfer-Encoding 仅接受 HTTP/1.1 的精确
`chunked`，未知 transfer coding 返回 501，HTTP/1.0 的 Transfer-Encoding 返回 400。当前
request API 不暴露 trailer，因此 server 拒绝任何非空 trailer，避免把尾部 Cookie 或
Authorization 错当成普通 header。

HTTP/2 的 transport-independent 引擎及 client/server CNet adapter 已导入；后续只把 S3 作为
CHTTP 上层模块导入，HTTP/3 不在范围内。TLS 与 WebSocket frame/session 状态归 CNet，CHTTP 负责
HTTPS policy、H1 Upgrade、H2 Extended CONNECT、路由和 byte-stream Adapter；UDP/KCP 由 CNet
的统一 packet endpoint 独立承载，不进入 HTTP transport。
详细阶段与验证门槛见
`../docs/CHTTP_CNET_PROTOCOL_TODO.md`。

这里的 keep-alive 是 HTTP/1.1 顺序复用或 HTTP/2 session 多 stream 复用，不等于内核
`SO_KEEPALIVE` 探测参数。内核 socket policy 统一使用 CNet 的
`cnet_stream_socket_options`：server 在启动前调用 `chttp_server_set_socket_options()`，同步
WebSocket client 通过 `chttp_websocket_client_config.socket_options` 配置。零值保留系统默认；
平台不支持的细分选项返回 `SALTS_ENOTSUP`，不回退到另一套语义。

`cnet_send()` admission 成功只证明 bytes 已复制进有界 command storage；`observer.on_send`
在完整 ordered write terminal 后报告一次完成。H1 server 只在该 callback 后重新申请 receive；
H2 为处理 WINDOW_UPDATE/PING 等控制帧允许收发全双工，但每条连接仍只有一个 CNet send in-flight。
这个完成事实仍不等于应用级 exactly-once，CHTTP
不会根据断线猜测 peer 是否消费了请求，也不做隐式 retry。

## HTTP Server：路由、中间件与 Session

普通服务端用户只注册 handler/middleware，然后调用 `chttp_server_start()`；后台线程独占 CNet
poller，业务代码不调用 `cnet_client_poll()`。路由 path 不含 query，可包含完整 segment 参数，
例如 `/users/:user/posts/:post`。静态路由优先于参数路由，HEAD 在没有显式 HEAD route 时回退
GET handler，但只发送 headers。

`enable_http2 = 1` 后，同一个 listener 同时接受 H1 与 H2。明文 H2 使用 h2c prior knowledge，
不支持 `Upgrade: h2c`；TLS 通过 ALPN 在 `h2` 与 `http/1.1` 间选择。H2 stream 可以在一条连接上
交错收发，但 handler 仍由 server owner thread 串行执行。停服先关闭 listener admission、发送
GOAWAY，再排空已经接纳的 stream；新 stream 不再进入 handler。最后通过 drain PING/ACK
确认此前帧已按序到达后关闭 transport；不响应 PING 的 peer 使用有界 grace 后关闭。

Server 的 H2 容量必须显式提供：`h2_input_buffer_bytes >= 16393`，
`16468 <= h2_output_buffer_bytes <= network.max_send_bytes`。不满足时
`chttp_server_init()` 返回 `SALTS_EMSGSIZE`，不会延迟到首个连接才失败。init 还会根据
request/response header count 与 bytes 上限计算 HPACK 最坏编码界；output 放不下该界时同样拒绝。

内部采用协议 Adapter + 共享请求模板流程：H1/llhttp 与 H2 frame/HPACK 分别把 wire input
归一化为 `chttp_server_request_view`，随后共同执行 route → middleware → Session → handler →
response builder。协议选择只发生在连接边界；公开 ABI 不暴露解析器类型，也不在逐帧热路径引入
CMeta interface/vtable。未来若加入第三种 wire protocol，可在保持公开 API 不变的前提下把该私有
边界提升为 Strategy。

网络-backed handler 还可从 request view 读取 borrowed `peer`；HTTPS/mTLS 在客户端提交并通过
验证时同时提供 `peer_certificate_sha256`。明文连接或未提交客户端证书时指纹为 `NULL`，因此应用
无需从 HTTP header 信任客户端自报的来源身份。

全局 middleware 先于 route middleware 执行。middleware 可以调用一次
`chttp_server_next_call()`，也可以直接 `chttp_server_reply()` 形成认证、CORS、限流或错误处理中断。
request、route param、header、body、session value 与 response builder 都是 handler-scoped；不得跨
callback、线程 handoff 或 coroutine suspension 保存裸指针。

### JWT Bearer（HS256）

CHTTP 公开的是一个有界的 HS256 JWT Bearer admission 边界，不公开 CJWT 类型或头文件。客户端用
`chttp_jwt_hs256_token_create()` 生成拥有型 token，再用 `chttp_jwt_bearer_header()` 写入调用方持有的
Authorization header buffer；请求提交完成后用 `chttp_jwt_token_destroy()` 释放 token。

服务端先初始化一个 validator，再在 `chttp_server_start()` 之前把它绑定为 server-wide policy、单个
HTTP route policy，或单个 WebSocket route policy。JWT 不再作为 ordinary middleware 注册：

```c
static const unsigned char key[] = "0123456789abcdef0123456789abcdef";
chttp_jwt_bearer_validator validator = {0};
chttp_jwt_bearer_validator_options jwt = {
    .size = sizeof(jwt),
    .key = key,
    .key_size = sizeof(key) - 1u,
    .expected_issuer = "issuer.example",
    .expected_audience = "api.example"};

chttp_jwt_bearer_validator_init(&validator, &jwt);
chttp_server_use_jwt_bearer(&server, &validator);               /* all routes */
/* or chttp_server_route_with_jwt_bearer(..., &validator); */  /* selected HTTP route */
/* or chttp_server_websocket_with_jwt_bearer(..., &validator); */
```

route-specific validator 覆盖 server-wide default；配置了 server-wide policy 后没有隐式 per-route opt-out。
认证时序固定为：validated headers → route selection → JWT verification → `100 Continue` →
`body_open`/HTTP/2 DATA → ordinary middleware → handler/WebSocket opening callback。因此未认证的 streaming
request 不会先进入应用 body sink；ordinary middleware 可以读取已验证的 `request->jwt_claims`，但不负责
执行 JWT admission。

当前安全 contract：HS256 secret 至少 32 bytes；`exp` 默认必须存在，只有显式设置
`allow_missing_exp != 0` 才允许缺失；Bearer scheme 大小写不敏感，并要求 scheme 后至少一个 literal SP，
多个 SP 合法，HTAB 不是 separator。缺失/重复 Authorization、malformed token、错误 key/签名、非 HS256、
issuer/audience 不匹配、expired 或 future-`nbf` token 都统一返回 `401 Unauthorized` 与
`WWW-Authenticate: Bearer`，不把内部验证原因暴露给客户端。

验证成功后的 claims view 与 request 一样只在当前 callback 有效，不能跨 callback、线程或异步 handoff 保存
裸指针。validator 持有 key/expected claims 的副本；所有使用该 validator 的 server 必须先 stop/destroy，
之后才能调用 `chttp_jwt_bearer_validator_destroy()`。HTTP/1.1、HTTP/2、HTTP/1.1 WebSocket Upgrade 与
RFC 8441 Extended CONNECT 共用同一个 pre-body admission contract。完整验证见
`chttp/tests/chttp_jwt_test.c`、`chttp/tests/chttp_server_test.c`、`chttp/tests/chttp_h2_server_test.c` 与
`chttp/tests/chttp_websocket_test.c`。

需要阻塞数据库或外部服务时，HTTP/1.1/H2 handler 先复制业务所需的 request 字段，再调用
`chttp_server_response_defer()` 封住当前 builder，并把拥有型 job 投递到应用已有的有界 worker
队列。worker 必须成功调用一次 `chttp_server_deferred_reply()` 或
`chttp_server_deferred_cancel()`；reply 在返回前复制 headers/body，成功后 job 即可释放响应内存。
reply 失败会让 handle 保持 pending，调用方必须显式 retry 或 cancel；fail-fast 路径使用 cancel，
不发送替代响应：H1 会关闭独占连接，H2 会发送 `RST_STREAM(CANCEL)` 且不影响 sibling stream。
H1 每条连接最多挂起一个响应；H2 每个固定 stream slot 最多挂起一个，总量受
`network.connection_capacity` 与 `h2_stream_capacity` 约束。同一 H1 连接的后续流水请求会保留并在
前一响应完整写出后恢复；H2 sibling stream 可继续推进。
队列 admission 失败时，handler 应直接同步返回 429/503，不得先 defer。deferred 当前要求
`session_capacity == 0`；应用级 Session 应随 job 自行解析与持有。H2 的 RST_STREAM/peer close 会让
尚未 claim 的 handle 变为 stale，GOAWAY drain 仍允许已接纳 stream 完成。停服会等待
已经交出的 handle 完成，未完成时有超时的 `chttp_server_stop()` 返回 `SALTS_ETIMEDOUT`，调用方
必须先终结工作再重试 stop。

开启 Session 后，Cookie 只保存 128-bit 随机 session id，实际 key/value 留在 server 的固定容量
存储中。Cookie 默认带 `Path=/; HttpOnly; SameSite=Lax`，可配置 `Secure`；idle timeout 滑动刷新。
达到 `session_capacity` 时不会驱逐仍有效的会话，新的 `chttp_session_set()` 返回
`SALTS_ENOBUFS`。当前 store 是进程内内存，不承诺重启持久化、多进程共享或分布式一致性。

```c
#include <chttp/chttp.h>
#include <string.h>

static int health(void *user, const chttp_server_request_view *request,
                  chttp_server_response *response) {
  const char *name = chttp_server_request_param(request, "name");
  (void)user;
  return chttp_server_reply(response, 200, "text/plain", name, name != NULL ? strlen(name) : 0);
}

static int add_security_headers(void *user, const chttp_server_request_view *request,
                                chttp_server_response *response, chttp_server_next *next) {
  int status;
  (void)user;
  (void)request;
  status = chttp_server_response_set_header(response, "X-Content-Type-Options", "nosniff");
  return status == SALTS_OK ? chttp_server_next_call(next) : status;
}

int main(void) {
  chttp_server server = {0};
  const chttp_server_config config = {
    .host = "127.0.0.1", .port = 8080, .backlog = 128,
    .network = {
      .backend = NATIVE_IO_BACKEND_IOCP,
      .connection_capacity = 128, .command_capacity = 256,
      .command_buffer_bytes = 4 * 1024 * 1024,
      .request_capacity = 256, .completion_batch_capacity = 64,
      .event_capacity = 256, .max_send_bytes = 64 * 1024,
      .event_buffer_bytes = 2 * 1024 * 1024,
      .receive_buffer_bytes = 16 * 1024,
      .read_timeout_ms = 30000, .write_timeout_ms = 5000,
    },
    .route_capacity = 64, .middleware_capacity = 16,
    .max_route_middleware_count = 8, .max_route_param_count = 8,
    .max_route_param_bytes = 4096, .max_target_bytes = 4096,
    .max_header_count = 64, .max_header_bytes = 16 * 1024,
    .max_request_body_bytes = 32 * 1024,
    .max_response_header_count = 64, .max_response_header_bytes = 8 * 1024,
    .max_response_body_bytes = 32 * 1024,
    .session_capacity = 1024, .session_entry_capacity = 16,
    .max_session_key_bytes = 64, .max_session_value_bytes = 1024,
    .session_idle_timeout_ms = 30 * 60 * 1000,
    .session_cookie_name = "sid", .session_cookie_secure = 0,
    .poll_slice_ms = 5,
    .enable_http2 = 1, .h2_stream_capacity = 64,
    .h2_input_buffer_bytes = 64 * 1024,
    .h2_output_buffer_bytes = 64 * 1024,
    .h2_hpack_dynamic_table_bytes = 4 * 1024,
    .h2_max_settings_count = 16,
    .buffer_capacity_bytes = 32 * 1024 * 1024,
  };
  int status = chttp_server_init(&server, &config);
  int started = 0;
  if (status == SALTS_OK) status = chttp_server_use(&server, add_security_headers, NULL);
  if (status == SALTS_OK) status = chttp_server_get(&server, "/health/:name", health, NULL);
  if (status == SALTS_OK) {
    status = chttp_server_start(&server);
    started = status == SALTS_OK;
  }
  /* Wait here for the application's shutdown signal. */
  if (started) {
    const int stop_status = chttp_server_stop(&server, 0);
    if (status == SALTS_OK) status = stop_status;
  }
  if (server.impl != NULL) {
    const int destroy_status = chttp_server_destroy(&server);
    if (status == SALTS_OK) status = destroy_status;
  }
  return status == SALTS_OK ? 0 : 1;
}
```

非 Windows 平台应把 backend 换成实际支持的 EPOLL 或 KQUEUE。handler 在单一 owner thread 上
串行执行，不能做阻塞数据库 I/O，也不能从 handler 调用 stop/destroy。Castle 风格的预加载
模板/静态资源可以直接 reply；`chttp_server_response_file()` 在 handler 返回后通过 server 共享的
CFlow file runtime 按有界 chunk 异步读取文件，完成事件唤醒 owner，并且只恢复对应的 H1 connection
或 H2 stream。阻塞业务工作使用上述 deferred handle，不把 borrowed request view 交给 worker。

## H1/H2 共用的流式正文与文件 API

`chttp_body_source` 每次向 CHTTP 提供一块数据，`chttp_body_sink` 只在成功消费整块后返回
`SALTS_OK`。同一调用点通过 `protocol` 选择 H1/H2，不接触 chunked、DATA frame 或 flow-control
window。已知长度 source 自动产生 `Content-Length` 并严格检查 EOF；未知长度在 H1.1 使用
chunked，在 H2 以 END_STREAM 结束且不发送 Content-Length。累计大小仍受
`max_request_body_bytes`/`max_response_body_bytes` 限制。

同步客户端可用 `chttp_post_file()`、`chttp_put_file()` 与 `chttp_download_file()`。下载先写同目录
UUID 临时文件，只有 2xx、正文完成、fsync 和 close 都成功才 rename；非 2xx 返回正常 owning
response，同时删除临时文件并保留原目标。sink 模式的 response 使用 `body == NULL`，但
`body_size` 仍是成功消费的累计字节数。

每个 client/server owner 懒创建一个有硬容量的共享 CFlow file runtime。正文 read/write 通过
IOCP 或 io_uring 原生异步操作完成；completion callback 仍只在 CHTTP owner 上执行。Windows
IOCP 不提供异步 flush，因此下载完成关闭异步句柄后，仅 durability barrier 使用同步
`salts_fs_fsync`；Linux io_uring 使用异步 flush。不支持 regular-file async I/O 的 backend 在
file open 前返回 `SALTS_ENOTSUP`，不会把正文数据路径静默降级为同步读写。

H1 在文件 write completion 前不再提交 receive。H2 在 write completion 前不归还该 DATA 的
stream/connection credit，并把其后的 frame 留在有界 protocol input buffer；完成后恢复 input 与
receive。这个单 lease 设计将每个活跃下载的额外内存固定为一个 input-sized buffer，也意味着一次
文件写等待期间，同连接中排在其后的 sibling frame 会短暂等待。

服务端 route 通过成对的 `body_open/body_close` 接收流式上传。open 在 header 与 route 参数可用后
执行；最终 middleware、Session 和 handler 只在完整正文成功到达且 close(SALTS_OK) 之后执行。
最终 request 使用 `body_streamed == 1`、`body == NULL` 和累计 `body_size`。鉴权若必须先于正文
spool，应在 `body_open` 完成；普通 middleware 保持最终 dispatch 语义。响应使用
`chttp_server_response_source()` 或 `chttp_server_response_file()`，HEAD 只发送 metadata，不读取
source/file bytes。

source/sink 的 buffer/view 只在回调内有效。回调在所属 client/server owner thread 上执行，不得
保存裸指针、递归 poll/stop/destroy 或执行无界阻塞工作。H1 回调失败关闭该独占连接；H2 只 reset
当前 stream，兄弟 stream 继续推进。服务端 response source 描述符会被复制，但 `source.user`
必须保持有效直到 EOF 或取消；文件响应的私有句柄由 CHTTP 在终态关闭。

## 用户接口：requests-style 调用不暴露 poller

普通用户只使用 `chttp_client_init()`、`chttp_get/head/post/put/delete/patch()`、
`chttp_response_destroy()` 和 `chttp_client_destroy()`。一次调用在当前线程阻塞，内部驱动
CHTTP → CNet → NativeIO，返回后 `chttp_response` 的 status、reason、headers 和 body 都由
调用方拥有；用户不创建 poller，也不需要提供 Executor 或 worker thread。

一个 client 同一时刻只允许一个调用，并归一个线程所有。需要并行请求时，可以把多个互不
共享的 client 交给应用已有的 `salts_threadpool` 或 Executor；不能让多个 worker 同时推进同一个
client。CHTTP 不创建隐藏的全局线程池，也不会把阻塞工作转交给不可控的后台线程。

`timeout_ms` 限制等待 HTTP result 的时间。到期后 CHTTP 会取消并等待底层 terminal 状态，再让
client 恢复到可安全复用或销毁的状态；因此底层 terminal drain 所需时间可能使函数返回略晚于
该 deadline。错误通过函数返回值与 `chttp_error` 的 status、native_status、stage 一起交付。

## 配置与连接参数

`chttp_client_config.network` 完整承载 CNet owner 的 backend、connection/command/native
request/event capacity、单次 send/receive bytes 和 connect/read/write timeout。CHTTP 自己只
增加 HTTP message/session 上限。`network.read_timeout_ms` 也约束 idle 连接等待 peer EOF 的时间；
零仍表示不设置该 deadline：

| 字段 | 单位与满额行为 |
|---|---|
| `request_capacity` | H1 request slot 或 H2 stream 的总量；满时 `SALTS_ENOBUFS` |
| `max_start_line_bytes` | request/status line；超限 `SALTS_EMSGSIZE` |
| `max_header_count` | 包含自动生成的 Host、Content-Length、Connection；超限 `SALTS_EMSGSIZE` |
| `max_header_bytes` | header line bytes（不含最终空行）；超限 `SALTS_EMSGSIZE` |
| `max_request_body_bytes` | copied/source request body 总量；超限 `SALTS_EMSGSIZE` |
| `max_response_body_bytes` | buffered/sink response body 总量；超限 `SALTS_EMSGSIZE` |
| `stream_chunk_bytes` | 每个 source 的 chunk 上限；零选择不超过 transport 的 64 KiB |
| `max_informational_responses` | 一个 final response 前允许的 1xx 数量 |
| `h2_input_buffer_bytes` | H2 parser input 上限；零选择 128 KiB 或更大的 CNet receive buffer |
| `h2_hpack_dynamic_table_bytes` | HPACK dynamic table 上限；零选择 4 KiB |
| `h2_max_settings_count` | 单个 SETTINGS frame 的 entry 上限；零选择 32 |

H2 header-list 预算包含应用 header 与自动生成的 `:method`、`:scheme`、`:path`、`:authority`、
`content-length`。请求与响应字段在 admission/解析边界执行 lowercase token、value whitespace/control
字符及 connection-specific header 校验；请求 `te` 只接受 `trailers`。越界或非法字段不会进入半可信
session 状态。

Server 另用 `max_buffered_response_body_bytes` 限制 `chttp_server_reply()` 的整块 copy；零选择
`max_response_body_bytes` 与单次 transport send 可容纳值中的较小者。这样
`max_response_body_bytes` 可以作为大文件/source 的传输总量上限，而不要求每条 connection/stream
预分配同样大的 response buffer。`stream_chunk_bytes` 同样限制服务端 response source；H2 还会受
peer window、最大 frame 和有界 output buffer 的更小限制。

每次 requests-style call 或高级 submit 再提供三项不同事实：

- `connection_uri`：CNet endpoint，例如 `tcp://127.0.0.1:8080` 或
  `tls://127.0.0.1:8443`；
- `authority`：HTTP Host/virtual-host，例如 `api.example.test:8080`；
- `target`：HTTP origin-form target，例如 `/v1/users?id=7`。

三者不能合并为一个模糊 URL：连接 endpoint、HTTP authority 和 request target 的归属不同，
Pipe、TLS SNI 与 virtual host 也需要能独立组合。CHTTP 不把 `https://` 当作模糊 URL 解析，
HTTPS 明确使用 `tls://` connection URI 和可选的 `chttp_tls_profile`。

pool key 是经过验证后的 `connection_uri + authority + TLS profile identity + protocol` 精确组合，`target`
不参与。因此同一站点的 `/users`、`/orders` 与 `/status` 可以顺序复用一条连接，但不同
authority 或不同 TLS profile 不做隐式 coalescing。内容相同但分别初始化的 profile 也属于不同
安全域；需要复用时应共享同一个 immutable profile。H1 每条连接同一时刻最多承载一个 request，
且不实现 pipelining；H2 session 可在一条连接上并发承载多个 stream。
`network.connection_capacity` 是 H1 connection 与 H2 session 的物理连接总上限，可以小于
`request_capacity`。匹配的 H2 session 达到 peer `SETTINGS_MAX_CONCURRENT_STREAMS` 时，client 会先
尝试其他同 key session；若仍有物理连接容量则新建 session，而不是让一个拥塞 session 阻塞整个
origin。pool 满且没有可用 session/idle slot 时，高级 submit 返回
`SALTS_ENOBUFS`；client 会先开始关闭一个没有活动 request/stream 的不匹配 H1 connection 或 H2
session，调用方 poll 后再重试。H1/H2 之间切换也遵守同一物理容量和驱逐协议。
requests-style client 则在本次调用 deadline 内自行推进该关闭并重新提交，因此普通用户切换站点
仍不需要接触 poller。这里的重新提交发生在新 request admission 之前，不是断线后的 HTTP retry。

## HTTPS 与 TLS profile

客户端先用 `chttp_tls_profile_init()` 把 CA、可选客户端证书、SNI/verified identity 与 ALPN
构造成可复用 profile，再把它放入 `chttp_options.tls` 或 `chttp_request_options.tls`。CHTTP
只支持空 ALPN、唯一的 `http/1.1` 或唯一的 `h2`。profile 与 request 的显式 protocol 必须一致；
H2 over TLS 必须提供 `h2` profile，并在 CONNECTED 后确认实际 negotiated ALPN 恰为 `h2`。
任何不一致都 fail fast，绝不降级为 H1 或明文。profile public wrapper 可在 request admission 后
销毁；connection/session 持有内部引用直到关闭。CNet 始终执行 peer 与 hostname verification，
不提供不安全开关。

服务端把 `cnet_tls_server_config` 放入 `chttp_server_config.tls`。`chttp_server_init()` 同步建立并
拥有 TLS context，因此证书配置字符串在 init 返回后即可释放；start 后仍由原有后台 owner
accept 和推进握手。TLS 模式要求 `network.tls_io_buffer_bytes >= CNET_TLS_MIN_IO_BUFFER_BYTES` 且
`tls_handshake_timeout_ms` 非零，配置或证书错误在监听前失败，不回退明文。mTLS 继续使用 CNet 的
`CNET_TLS_CLIENT_AUTH_REQUIRED` 与显式 CA source。HTTPS Session 应设置
`session_cookie_secure = 1`。

## WebSocket 与 WSS

Server 用 `chttp_server_websocket()` 注册默认容量 route，或用
`chttp_server_websocket_with()` 显式设置 route middleware、frame/message/input 上限。同一路由
接受 H1 Upgrade 与已启用 HTTP/2 listener 上的 RFC 8441 Extended CONNECT。两者都先执行全局
middleware、route middleware 与 Session admission；`on_open` 返回错误时生成 500，
调用 `chttp_server_reply()` 时按该内存 HTTP response 拒绝；WebSocket opening admission 不接受 stream/file
response，否则 H1 发送 101、H2 发送 200 且保持 stream 打开。`on_open` 可以预先发送一帧，
该帧会在 opening response 完整写出后才进入 wire。`on_event` 与 `chttp_websocket` 都属于 server owner
callback，不能跨 callback、线程或协程挂起保存；text/binary/ping/pong/close 均委托给 CNet 的
同一有界 RFC 6455 engine。

业务需要在 callback 返回后异步回包时，必须在 callback 内调用
`chttp_server_websocket_session_capture()` 取得 generation-checked
`chttp_server_websocket_session`，再从任意线程调用
`chttp_server_websocket_send_text()`、`chttp_server_websocket_send_binary()`、ping/pong 或 close。
提交会在返回前复制 payload，并受 `network.command_capacity`、`network.command_buffer_bytes` 与
`network.max_send_bytes` 约束；任一队列预算耗尽
返回 `SALTS_ENOBUFS`，已经关闭或复用的 connection/stream handle 只会丢弃对应旧命令，不会误投到
新会话。该 value 不延长连接或 server 生命周期，所有提交者必须在 `chttp_server_destroy()` 前静默。

普通 client 使用 `chttp_websocket_client_init()`、`chttp_websocket_client_connect()`、阻塞
send/receive/close 与 `chttp_websocket_client_destroy()`；这些调用内部推进 CNet，应用不接触
poller。connect 接受完整 `ws://` 或 `wss://` URI；WSS 可传可复用的 `chttp_tls_profile` 来配置
CA 与 verified identity/SNI。`chttp_websocket_connect_options.protocol` 零值选择 HTTP/1.1；显式
`CHTTP_HTTP_2` 选择 RFC 8441，h2c 会先等待 server 的 `SETTINGS_ENABLE_CONNECT_PROTOCOL=1`，
WSS 则额外要求 TLS profile 精确使用 `h2` ALPN。能力未发布、ALPN 不匹配、TLS/证书失败时均
明确报错，不回退 H1 或明文。client 是 single-owner，一次只拥有一条物理连接上的一个 WebSocket
stream，不能并发调用；`receive` 返回的 event view 只在下一次 client 操作前有效。

需要在同一站点保持多条长连接逻辑通道时，使用 HTTP/2 专用的
`chttp_websocket_pool`。pool 只建立一条 TCP/TLS + H2 连接；每次
`chttp_websocket_pool_open()` 以完整 URI 打开一个独立 RFC 8441 stream，并返回
generation-checked `chttp_websocket_session`。首个 URI 固定 scheme、authority 和 TLS profile，后续 URI
可以使用不同 path/query；origin 或 TLS profile 不一致会返回 `SALTS_EINVAL`，不会暗中建立第二条连接。
关闭一个 session 只完成该 WebSocket Close handshake 和 H2 END_STREAM，其他 session 仍可收发。

调用顺序是：用共享 network/WebSocket limits 和 `session_capacity` 初始化 pool；将
`chttp_websocket_connect_options.protocol` 设为 `CHTTP_HTTP_2`，分别以 `/chat`、`/notices` 等完整
URI 调用 `open`；此后每次 send/receive/close 都同时传入 pool 与对应 session handle；所有 handle
关闭后销毁 pool。WSS 的每次 open 都传入首个连接使用的同一个 `h2` TLS profile。

pool 与其所有 session 都由一个 progress owner 串行调用，应用仍不接触 poller。`session_capacity`
同时是本地 stream 硬上限；peer 的 `SETTINGS_MAX_CONCURRENT_STREAMS` 可能进一步收紧它，满额统一返回
`SALTS_ENOBUFS`。每个 slot 独立拥有 WebSocket parser、event ring、payload slab 与在途 frame storage；
stream parse/容量错误只终止该 handle，物理 transport/H2 connection 错误才终止所有 session。
pool `receive` 返回的 view 在下一次任意 pool 操作时失效，stale generation 返回 `SALTS_ENOENT`。

三个 WebSocket 容量同样是硬边界：frame 决定单帧 payload 上限，message 决定 fragment 重组后
总量，buffered input 决定尚未形成完整 frame 的保留量。client event queue 也受
`event_capacity` 限制，满额返回 `SALTS_ENOBUFS`，不会静默丢帧或无界扩容。CHTTP 不直接导出
`tools/wsparser`；它由 CNet 私有使用，frame masking、UTF-8、control frame、fragment 与 close
状态只有一个事实源。H2 DATA 只是这套 byte-stream engine 的 transport adapter；stream flow-control
credit 在输入被同步消费后归还，Close frame 排空后才发送 END_STREAM。

## 高级 event-loop 接口

`chttp_async_client_submit()`、`chttp_async_request_cancel()`、`chttp_async_client_poll()`、
`chttp_async_client_stop()` 和 `chttp_async_client_destroy()` 是供 CRPC、Executor 或已有事件循环
适配器使用的高级接口，不是普通 HTTP 调用流程。只有这类集成层负责调用
`chttp_async_client_poll()`；应用业务代码默认使用 requests-style API。

高级接口当前不创建 per-request timer；Executor/事件循环拥有 deadline，并在到期时调用
`chttp_async_request_cancel()`。`chttp_options.timeout_ms` 只属于 requests-style 阻塞调用，不能把它
误解为 async stream 字段。

## Ownership、状态与背压

CHTTP 与内嵌 CNet 共享一个 progress owner。`submit/cancel/poll/stop/destroy` 不得并发调用；
completion callback 在 `chttp_async_client_poll()` 或 `chttp_async_client_stop()` 的调用线程内同步执行。
callback 可以取消另一条已接受 request，但不能递归 poll/stop/destroy，也不能直接 submit 新
request；submit 必须等 callback 返回后再进行，否则返回 `SALTS_EBUSY`。

```text
H1: FREE -> CONNECTING -> BUSY -> IDLE/CLOSING -> TERMINAL -> RECYCLE
H2 session: FREE -> CONNECTING -> ACTIVE -> DRAINING/CLOSING -> TERMINAL
H2 stream: SUBMITTED -> HEADERS/DATA -> COMPLETE 或 RST_STREAM -> RECYCLE
```

- 新连接 submit 成功前由 CNet 复制 URI，CHTTP 复制 pool key、retain TLS profile，并由 serializer 复制
  method/authority/target/headers/body；
- `options.user` 是 borrowed，必须存活到 completion callback 返回；
- response、header string 和 body 只在 completion callback 期间有效；需要跨 callback 或协程
  挂起保留时，调用方必须复制；
- `protocol_keep_alive` 描述响应协议是否允许持久连接；它不是某条物理连接仍在 pool 中可借出的
  快照。H2 响应为真，收到 GOAWAY 后由内部 session 状态阻止新 stream admission；
- 每次只调用 `cnet_receive(connection, 1)`；当前 byte chunk 消费并确认 parser/body 仍有容量后
  才申请下一个 receive value；idle slot 也只保留一个 receive demand，用于观察 EOF/read timeout；
- idle slot 收到没有对应 request 的 bytes 属于协议异常，该连接直接失效并关闭；
- H1 cancel 关闭连接；H2 cancel 发送 `RST_STREAM(CANCEL)`，已经在途的已取消 stream frame 会被
  有界消费且不触发用户 callback，也不影响同 session 的其他 stream；
- H2 单-stream 响应语义错误或 body/header 上限错误只重置该 stream；HPACK block 仍完整解码以
  保持动态表一致，兄弟 stream 与物理连接继续推进；
- stop/close 后资源仍保留到 stream 与 CNet terminal callback 收敛后才 recycle；
- stop timeout 不重开 admission；调用方可重试 stop。若 H2 graceful drain 尚未完成，重试继续推进
  GOAWAY/stream 收敛；一旦进入 CNet stop，重试只继续底层 drain，不再调用已关闭的普通 poll；
- submit 成功恰好产生一次 completion callback。立即 admission 失败不产生 callback。

每个 active request 的 retained HTTP 内存上界可按下式复算：

```text
serialized request <= network.max_send_bytes
+ llhttp/H2 session metadata
+ max_header_count * header metadata
+ max_header_bytes + terminators
+ max_start_line_bytes
+ max_response_body_bytes
```

总预算还需乘 `request_capacity`，再加每个 H2 session 的 protocol/input/output/HPACK/stream table、
每个连接的 URI/authority/profile pool key、CNet command/event/native-request 和 receive buffer
预算。H1 idle slot 已释放上一条 response parser 的
reason/header/body storage，只保留连接、key 和 CNet receive state。

## 普通用户示例

```c
#include <stdio.h>

#include <chttp/chttp.h>

int main(void) {
  chttp_client client = {0};
  chttp_response response = {0};
  chttp_error error = {0};
  const chttp_client_config config = {
    .network = {
      .backend = NATIVE_IO_BACKEND_IOCP,
      .connection_capacity = 1,
      .command_capacity = 8,
      .command_buffer_bytes = 256 * 1024,
      .request_capacity = 4,
      .completion_batch_capacity = 4,
      .event_capacity = 8,
      .max_send_bytes = 64 * 1024,
      .receive_buffer_bytes = 16 * 1024,
      .connect_timeout_ms = 5000,
      .read_timeout_ms = 30000,
      .write_timeout_ms = 5000,
    },
    .request_capacity = 1,
    .max_start_line_bytes = 4096,
    .max_header_count = 64,
    .max_header_bytes = 32 * 1024,
    .max_request_body_bytes = 32 * 1024,
    .max_response_body_bytes = 1024 * 1024,
    .max_informational_responses = 4,
  };
  const chttp_options options = {
    .connection_uri = "tcp://127.0.0.1:8080",
    .authority = "api.example.test:8080",
    .target = "/v1/health",
    .timeout_ms = 30000,
  };
  int status = chttp_client_init(&client, &config);

  if (status == SALTS_OK)
    status = chttp_get(&client, &options, &response, &error);
  if (status == SALTS_OK)
    (void)fwrite(response.body, 1, response.body_size, stdout);
  else
    (void)fprintf(stderr, "CHTTP failed: status=%d native=%d stage=%s\n",
                  error.status, error.native_status,
                  error.stage != NULL ? error.stage : "unknown");

  chttp_response_destroy(&response);
  if (client.impl != NULL) {
    const int destroy_status = chttp_client_destroy(&client, 5000);
    if (status == SALTS_OK) status = destroy_status;
  }
  return status == SALTS_OK ? 0 : 1;
}
```

非 Windows 平台应选择实际支持的 NativeIO backend。生产代码必须消费并传播调用返回的
`error.status`、`error.native_status` 和 `error.stage`。

## 架构决策

候选方案包括直接从 CHTTP 使用 NativeIO、把 llhttp 放进 CNet，以及在 CNet 上建立独立
CHTTP。选择第三种：NativeIO 只拥有 native operation terminal truth；CNet 拥有 DNS、URI、
connection handle、receive demand 和 shutdown；CHTTP 拥有 HTTP framing、parser state 与
message limits；llhttp 是私有 parser adapter。

这一选择增加一个显式层；小型 memory response 仍完整 buffering，而 source/sink 将文件和大正文
保持为有界 chunk。它避免 HTTP/RPC 重复连接状态机，也避免
第三方 parser 类型进入 CNet API。当前 keep-alive pool 只复用完整 message 之间的空闲连接；若
迁移到 streaming、pipelining 或 retry，需保留当前 message API，并先补 CNet write-terminal
契约。若实现需要回滚，应回退对应提交并恢复上一版安装包；不能依赖运行时 fallback 或已经
移除的构建开关。

## 构建与验证

Windows Release：

```text
cmake --fresh --preset win-release-user
cmake --build --preset win-release-user --target \
  chttp_request_test chttp_response_test chttp_server_parser_test chttp_server_test \
  chttp_api_test chttp_requests_test chttp_header_cpp_test chttp_tls_test \
  chttp_h2_frame_test chttp_h2_hpack_test chttp_h2_proto_test chttp_h2_client_test \
  chttp_h2_server_test
ctest --preset win-release-user -R "^chttp_" --output-on-failure
```

Windows 命令必须先进入 `VsDevCmd.bat` 环境。llhttp 的上游 API 与 strict/lenient 安全说明见
[nodejs/llhttp](https://github.com/nodejs/llhttp)；HTTP/2 wire 与 malformed-message 规则见
[RFC 9113](https://www.rfc-editor.org/rfc/rfc9113.html)。
