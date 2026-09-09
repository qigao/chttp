# HTTP 与 RPC 服务端

`src/` 包含 HTTP server、H1/H2 请求处理、路由、全局/路由级中间件、
Cookie Session、JWT admission、WebSocket server 与文件响应。
`rpc/` 包含 JSON-RPC server、方法注册、dispatch、batch 和结果/错误响应。
`examples/` 提供可独立构建运行的 RPC server 示例。

Session 由 HTTP server 持有，具备显式容量与 idle timeout。
使用 `chttp_session_get/set/remove/invalidate` 操作请求上的会话。
中间件使用 `chttp_server_use()` 注册，通过 `chttp_server_next_call()` 继续处理链；
路由也支持单独配置中间件。RPC server 通过 `crpc_server_http()` 暴露借用的 HTTP owner，允许在启动前配置这些能力。

服务端 owner、并发提交、deferred response 与关闭协议保持不变。
服务端实现只引用本侧私有头、公共协议头和安装后的 Salts API，不访问客户端私有运行时。
公开入口为 `<http_server/http.h>`、`<http_server/rpc.h>`，位于本模块的 `include/`。
HTTP 和 RPC 服务端共同编入 `CHttp::Server`，无需链接客户端库。

详细接口见 [HTTP 使用说明](../docs/HTTP.md)、[RPC 使用说明](../docs/RPC.md)，
deferred 生命周期见 [终态设计](ADR_DEFERRED_TERMINAL.md)。
`tests/` 保留服务端、Session/middleware、JWT、H2 和 RPC server 回归。

## Middleware 支持

HTTP/1.1、HTTP/2、WebSocket 路由和 RPC 共用 HTTP 中间件链。

| 范围 | 注册入口 | 执行位置 |
| --- | --- | --- |
| 全局 | `chttp_server_use(server, callback, user)` | 按注册顺序，在路由中间件之前执行，也覆盖内置 404/405 |
| HTTP 路由 | `chttp_server_route_with()` 的 `middleware` / `middleware_count` | 全局中间件之后、路由 handler 之前 |
| WebSocket 路由 | `chttp_server_websocket_with()` 的同名字段 | 开启握手的回调之前 |
| RPC | `chttp_server_use(crpc_server_http(server), callback, user)` | JSON-RPC dispatch 之前；作用于 HTTP 请求，而非 batch 中每个方法 |

回调接收 `user`、只读 `request`、`response` 和 `next`。
调用 `chttp_server_next_call(next)` 继续执行链，也可以直接调用 `chttp_server_reply()`
并返回其状态，提前结束请求。重复调用同一个 `next` 返回 `SALTS_EALREADY`。
应在调用 `next` 之前设置响应头；下游可能已经完成响应。

注册必须在 start 之前完成，启动后返回 `SALTS_EBUSY`。
全局容量由 `middleware_capacity` 限制，达到容量返回 `SALTS_ENOBUFS`；
每个路由受 `max_route_middleware_count` 限制。注册复制绑定，`user` 所指对象由应用持有，
必须存活至服务端停止并释放。请求、响应和 `next` 仅在当前回调中有效，不可跨线程保存。

普通中间件在请求正文接收完成后执行。需要在正文接收前拒绝请求时，应使用已有的
JWT admission 或路由 `body_open` 入口；流式上传不能依赖普通中间件提前阻止正文进入 sink。

可运行示例见 [crpc_server_example.c](examples/crpc_server_example.c)：
它在启动前注册全局中间件，为 RPC 响应添加 `X-Example: rpc-middleware`，再调用 `next`。
完整配置、启动和清理均包含在示例中：

```powershell
cmake --build --preset win-release-user --target crpc_server_example
```

路由级绑定和 Session 联动见 [chttp_server_test.c](tests/chttp_server_test.c)，
RPC 中间件回归见 [crpc_server_test.c](tests/crpc_server_test.c)。

### CORS 中间件

`chttp_server_use_cors(server, &policy)` 在启动前注册全局 CORS 策略；RPC 使用
`crpc_server_http()` 取得同一个 server。完整示例已允许 `http://localhost:3000`，
可从该来源访问 `/rpc` 与 `/file`。

策略通过 `origins` / `origin_count` 指定精确来源；`methods` 指定预检允许的方法；
`allowed_headers`、`exposed_headers` 分别指定请求头白名单与可读响应头，NULL 表示不配置。
这些列表用逗号分隔，方法区分大小写、头名称不区分大小写。
`max_age_seconds` 控制预检缓存时间，零表示不缓存；`allow_credentials` 允许凭据。
只有来源支持单独的 `*`，且不得同时允许凭据。列表不支持通配头名称，Authorization 必须显式列出。

| 请求 | 行为 |
| --- | --- |
| 无 Origin | 继续原执行链，仍添加 `Vary: Origin` |
| 来源匹配的普通请求 | 添加 Allow-Origin、可选凭据与 Expose-Headers，继续执行链 |
| OPTIONS + Origin + Request-Method | 校验方法和请求头，成功直接返回 204 |
| 来源、预检方法或头不在白名单 | 403，不进入后续 middleware 或 handler |
| 重复 Origin / Request-Method、非法预检字段 | 400 |

预检成功不表示目标路由存在或已授权。普通 OPTIONS 仍走原路由；方法白名单仅控制预检，
不能代替路由权限。来源按序列化字符串精确比较，不做大小写、默认端口或尾斜杠修正；
`null` 来源只有显式列入或使用 `*` 才允许。按需配置，默认不注册即不启用。

策略结构、数组及字符串归应用所有，注册后必须保持不变并存活至 server 销毁。
middleware 在 server 的单一执行线程内读取策略，请求期间不创建配置副本或额外堆缓冲。
所有新增头都占用现有响应头数量和字节预算，超限返回错误，由既有 handler 错误边界生成 500；
不会发送一半 CORS 头。`Vary` 保留此前中间件设置的字段，后续 handler 不应覆盖它或 CORS 响应头。

JWT admission 和流式 body sink 仍先于普通 middleware：此入口不会跳过认证，也不能阻止此前的
正文处理。使用全局 JWT 时，无凭据预检同样可能被拒绝；需要匿名预检的服务应把 JWT 绑定到业务
路由。JWT/协议层提前拒绝的响应不会经过 CORS。CORS 不提供 CSRF 防护或 WebSocket Origin 鉴权。

仅新增可选 API，不改变已有结构的布局、不增加依赖；撤回时移除注册调用即可恢复原执行链。
测试见 [chttp_cors_test.c](tests/chttp_cors_test.c)，覆盖 H1/H2、凭据、预检、重复字段、容量耗尽与连接复用。
协议依据：[Fetch CORS protocol](https://fetch.spec.whatwg.org/#http-cors-protocol)。

## 正文前 Admission 与阶段 Deadline

这两个入口均在 `chttp_server_init()` 后、`start()` 前设置；RPC 使用
`crpc_server_http()` 取得底层 server。未注册 hook、未设置预算时保持原行为，
已有 `chttp_server_config` 的布局不变。配置只在控制面修改，启动后返回 `SALTS_EBUSY`。

`chttp_server_set_admission(server, callback, user)` 设置一个全局决策入口，NULL 清除。
H1/H2 的执行顺序为：头部及路由解析 → JWT → admission → body_open/正文 → middleware/handler。
H1 的 `100 Continue` 也在 admission 成功后发送。回调能读取已归一化的路径、路由参数、peer
和已验证的 `jwt_claims`；`body`、Session 不可用。没有对应路由的请求也执行 hook，
因此可在内置 404/405 前拒绝。WebSocket 握手同样执行此决策。

回调接收清零的 `chttp_server_admission_result`：返回 `SALTS_OK` 且 `status_code=0` 继续，
`400..599` 拒绝；拒绝时可设置 `retry_after_seconds` 输出 `Retry-After`。回调错误、非法状态码
或放行却设置 Retry-After 都生成 500，并计入已有 handler_errors。JWT 拒绝不会调用此 hook。
H1 拒绝后发送响应并关闭连接；H2 只响应当前 stream，不将后续 DATA 交给 body sink。
这不是普通 middleware，也不会自动给提前拒绝的响应补上 CORS 头。

server 保存 callback/user 绑定；应用持有 user，直至 server 销毁。回调在单一 owner 线程执行，
不得阻塞、重入或跨回调保留请求指针。决策仅复制两个整数，没有新增队列或请求数据副本。
多个限流/权限条件可在应用 callback 内组合；内置令牌桶的接入见下文“内置限流”。
完整注册和请求示例见 [H1 测试](tests/chttp_server_test.c) 与 [H2 测试](tests/chttp_h2_server_test.c)。

`chttp_server_set_deadlines(server, &budgets)` 复制三个毫秒预算，零独立关闭对应阶段：

| 字段 | 开始与结束 | 超时结果 |
| --- | --- | --- |
| `headers_ms` | H1 首个请求字节至完整头；H2 首个片段至完整 preface/frame/连续头块 | 关闭连接，不保证生成 HTTP 错误响应 |
| `body_ms` | 头部完成至完整正文（含 trailers；也计入 admission 执行时间） | H1 关闭连接；H2 当前 stream 收到 RST_STREAM CANCEL |
| `handler_ms` | 普通 middleware/handler 开始至同步返回或 deferred 终态被接纳 | 同上；不包含后续响应正文传输 |

使用单调时钟累计，接收零星字节不刷新当前阶段预算。H2 不完整 frame 阻塞连接级解析，
因此采用连接级期限；已经解析完成头部的 stream 各自拥有正文/执行期限。
空闲连接和 WebSocket 建立后的消息流仍使用 CNet 原有超时配置，TLS 握手仍由 TLS policy 管理。
每个阶段只保存一个 owner 所有的截止时间，轮询成本与连接/stream 槽位数线性相关，不新增分配。

**执行边界：** owner 在轮询和请求阶段边界检查期限，精度受调度与现有 I/O 关闭过程影响。
同步 handler 不能被安全抢占；若超时后才返回，其响应会被丢弃。应用应把长任务交给 worker
并使用 deferred。到期时 owner 仅以 CAS 取消仍处于 PENDING 的 deferred；先取得 WRITING 的
提交者拥有终态，继续发布/清理，不会在写入中被释放。被取消句柄在 drain 前返回
`SALTS_EALREADY`，drain 后返回 `SALTS_ENOENT`。超时不撤销应用已经执行的外部副作用。

已打开的请求 body sink 在正文超时后恰好关闭一次，状态为 `SALTS_ETIMEDOUT`。未执行的 handler
不会在超时后开始；一个 owner 上正在阻塞的回调仍会推迟其它请求的期限检查。
流式响应传输不在此批 deadline 范围，继续受网络写超时和已有容量预算约束。

本次在现有分层上增加独立配置入口，复用关闭和 deferred 原子终态；未新增后台定时线程，避免
跨线程抢占 callback 和 buffer。部署时逐项设置预算，过小会中断合法慢请求；将预算设回零、
移除 admission 绑定即可恢复原策略。示例 [crpc_server_example.c](examples/crpc_server_example.c)
已设置三个阶段预算。协议边界参考 [HTTP/1.1 超时](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.5)
与 [HTTP/2 stream 错误处理](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.4.2)。

## 内置限流

包含 `<http_server/rate_limit.h>`，链接 `CHttp::Server`。`chttp_rate_limiter_init()`
创建有界令牌桶，再通过 `chttp_server_set_admission(server, chttp_rate_limiter_admit, &limiter)`
绑定。可运行的 [服务器示例](examples/crpc_server_example.c) 按真实 peer IP 分组，最多保留
64 组，每组初始/最大 100 个令牌，每秒补充 50 个。配置由应用传入，初始化时复制。

| scope | 身份与隔离边界 |
|---|---|
| `CHTTP_RATE_LIMIT_GLOBAL` | 此 limiter 的所有请求共用一组，`group_capacity` 必须为 1 |
| `CHTTP_RATE_LIMIT_PEER_IP` | TCP 地址；忽略端口、Forwarded 和 X-Forwarded-For，IPv6 scope ID 保留 |
| `CHTTP_RATE_LIMIT_JWT_SUBJECT` | 已验证 JWT 的 issuer 与 subject；缺失/空 subject 返回 403 |

JWT 模式必须先配置已有 JWT validator。issuer 缺失与空串等价；不同 issuer 的同名 subject
相互隔离。`key_bytes` 限制 issuer 与 subject 的总字节数（包含两个 NUL），允许 2..256，
超长返回 431，不截断身份。其它 scope 忽略该字段。IPv4 与 IPv6（包括 mapped IPv4）按各自
地址族分组，不自动合并；peer 缺失或地址族不支持时回调返回 `SALTS_EINVAL`，服务器生成 500。

每次通过 admission 消耗一个令牌；后续正文、middleware、handler 或响应失败不退还。
补充按单调毫秒连续累计，保留小数额度。额度不足返回 429 和向上取整的 `Retry-After` 秒数，
参考 [RFC 6585 §4](https://www.rfc-editor.org/rfc/rfc6585.html#section-4)。分组满且没有完全补满的
旧组时，新身份返回 503；已有组继续按自身额度处理。只有完全补满的组可被复用，避免更换身份
挤掉欠额记录后恢复 burst。满额 503 不承诺重试时间，应用可以调整容量或入口策略。

**状态与生命周期：** limiter 独占所有 bucket 与复制后的 key，只在一个 server owner 线程
推进；不可在多个 server owner 间共享或并发调用。初始化预建固定容量 CSTL Vec 槽位，
请求期无分配，查找为 `O(group_capacity × key_bytes)`，存储约为每组 256 字节 key 加三个
64 位字段（具体含平台对齐）。这是有限分组场景的直接实现；大规模身份表需另行评估查找成本。
未采用自动逐出 HashMap，避免删除/rehash 在请求期分配或丢失欠额状态。新增依赖仅为 Server
私有的 `Salts::CSTL`，现有 HTTP/RPC 配置结构不变，Client 与 S3 接口不变。

先初始化 limiter，再绑定并启动 server；成功销毁 server 后再销毁 limiter。若自定义 admission
已有策略，由应用显式组合回调并检查返回值与 `status_code`，一个 hook 不会自动串联多个策略。
移除绑定即可停用限流；重新初始化会重置所有额度。它限制此实例的请求入场速率，不限制 JWT
验证前的连接/解析成本、正在执行的请求数或出站带宽，也不提供跨进程配额。

验证覆盖令牌补充/取整、时间倒退、整数极值、容量复用、JWT key 隔离及真实 H1/H2 入场拒绝：

```powershell
cmake --build --preset win-release-user
ctest --preset win-release-user -R "chttp_rate_limit_test|chttp_server_test|chttp_h2_server_test" --output-on-failure
```

## 静态文件响应

`chttp_server_serve_file()` 支持 GET/HEAD、ETag、Last-Modified、条件请求、单段 Range 和
按扩展名选择 MIME；具体选项和状态码见 [文件服务边界](../docs/HTTP.md#文件服务边界)。
原 `chttp_server_response_file()` 继续作为不处理缓存条件的底层文件响应入口。

示例可接受一个本地文件参数，将该文件挂到 `/file`；路径由应用显式指定：

```powershell
cmake --build --preset win-release-user --target crpc_server_example
./build/Msvc-Release/bin/crpc_server_example.exe ./README.md
```

按程序输出的端口访问 `/file` 或发送 Range / If-None-Match。文件内容和路径绑定必须在传输期间
保持不变。默认元数据 ETag 是弱标签；需要 `If-Range` 时由应用提供可靠的强 ETag。
