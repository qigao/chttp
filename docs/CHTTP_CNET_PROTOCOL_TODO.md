# CNet / CHTTP 协议能力 TODO

## 当前基线

截至当前基线，CNet 已提供 TCP、TLS、UDP、Pipe、TCP listener、plain/TLS accepted-socket
接管、有序 send completion、send-and-close，以及独立于 transport 的有界 WebSocket session
engine；CHTTP 已在其上提供 HTTP/1.1/HTTPS client/server、显式 h2c/TLS HTTP/2 client/server、
静态与命名参数路由、中间件、有界内存 Cookie Session，以及不要求用户调用 poller 的
后台 server owner，以及 H1 Upgrade/RFC 8441 H2 Extended CONNECT 的 `ws://`/verified `wss://`
client/server。

CHTTP server 的网络命令采用每连接单 owner pending-action 状态机：callback 遇到有界 CNet
command ring 满额时保留 receive/send/close 动作和相关 buffer，worker 以 round-robin 公平重试。
该状态不跨线程写入，不通过无界分配绕过背压；动作成功提交或收到终态事件后才清除。

CHTTP 已通过可复用 TLS profile 把 CNet TLS 接入 H1/H2 client/server，并用同一 CNet
WebSocket engine 实现 HTTP/1.1 Upgrade 与 RFC 8441 Extended CONNECT route，以及
`ws://`/`wss://` 同步 client；独立的 S3 应用协议模块复用同一 CHTTP H1/H2 transport、TLS、
连接池与文件 source/sink。以下能力不得在对应公开文档、示例或 capability negotiation 中声明
为可用：KCP。
HTTP/3 明确不在本路线范围内。

## P0：CNet TLS transport

- [x] 定义纯 C、opaque 的 TLS client/server 配置：trust source、证书链、私钥、SNI、
  ALPN、peer/hostname verification、client certificate policy 和 handshake timeout。
- [x] 在 CNet transport 层实现 TLS 状态机，使 connect/adopt、handshake、加密 read/write、
  close-notify 与 cancellation 仍由同一个 owner 推进。
- [x] 默认强制证书链与 hostname verification；不提供从 `tls`/`https` 静默降级到明文的
  fallback。
- [x] 将 TLS profile 纳入连接复用 key；证书身份、trust source、SNI 或 ALPN 不同的连接
  不得复用。
- [x] 对 handshake input/output、单 record、累计 plaintext/ciphertext buffer 设置硬上限，
  并定义满额、timeout、peer truncation 和 shutdown 的可区分错误。
- [x] 覆盖可信/不可信证书、hostname mismatch、mTLS、部分 record、超时、取消、
  close-notify、accepted socket 和安装包链接测试。

完成条件：`tls://` 能以 mandatory verification 建连，CNet server caller 能通过同一
accepted-connection 路径终止 TLS；CHTTP client/server 复用该路径，并以
`connection_uri + authority + TLS profile identity` 隔离连接池。Release、ASan 与安装包回归
属于每次交付的统一门槛。

## P1：CNet KCP、WebSocket engine 与 CHTTP Upgrade

- [ ] 将 KCP 会话放在 CNet：NativeIO 只提供 UDP datagram 与 timer，CNet owner 独占
  conversation、window、retransmit、ordered delivery、MTU 和 shutdown 状态。
- [ ] 为 KCP 配置 receive/send/window/MTU/interval/dead-link 等硬边界；禁止在 NativeIO
  backend 中加入 KCP packet、重传或消息语义。
- [ ] 覆盖 loss/reorder/duplicate、窗口满、timer 推进、cancel、peer timeout 与 shutdown
  drain 测试。

- [x] 导入并复用 `tools/wsparser` 作为唯一 WebSocket frame parser；已覆盖 RFC 6455 canonical
  length、reserved opcode/RSV、NULL/overflow、控制帧边界与 borrowed payload 失效点，不在
  CNet 或 CHTTP 中复制第二套 frame parser。
- [x] 定义有界 WS message API：text/binary、fragment、ping/pong、close code/reason、
  max frame bytes、max message bytes、UTF-8 validation 和 backpressure。
- [x] 在 `tools/wsparser` 语法结果之上实现 RFC 6455 session 状态机；client frame 必须 mask，
  控制帧、fragmentation 和 close handshake 必须严格校验。
- [x] CNet WebSocket engine 只依赖有序双向 byte stream，不解析 HTTP header；同一 frame/
  message/session 状态机同时承载 CHTTP HTTP/1.1 Upgrade 与 HTTP/2 RFC 8441 stream。
- [x] CHTTP 使用现有 llhttp request parser 完成 opening handshake 的 header/route 校验；Upgrade
  成功前不得把字节或 stream 所有权交给 CNet WebSocket session。
- [x] 为 `ws://` 和 `wss://` client 实现 HTTP Upgrade；`wss` 必须复用 P0 TLS transport。
- [x] 在 CHTTP server 增加显式 WebSocket route。CHTTP 负责路由与 Upgrade header 校验，
  成功后把 accepted stream 的所有权一次性移交给 CNet WebSocket engine。
- [x] HTTP/2 由 CHTTP 校验 extended CONNECT 与 `:protocol = websocket`，再把该 H2
  stream 适配给同一 CNet WebSocket engine；不得为 H1/H2 复制两套 WS 状态机。
- [x] 明确移交状态机：HTTP parser 不得继续消费已移交字节；失败必须发送 HTTP 错误并关闭；
  成功后 response builder、Session view 和 request view 立即失效。
- [x] 覆盖 opening handshake、mask violation、fragment 重组、oversize、ping/pong、双方 close、
  abrupt EOF 与 WS/WSS；跨线程 mailbox 驱动仍属于 future async client adapter 范围。

完成条件：普通 handler API 不受影响；WebSocket route 不需要用户调用 poller；缺少 TLS 时
请求 `wss` 明确返回 capability error；H2 client 只在 peer 发布
`SETTINGS_ENABLE_CONNECT_PROTOCOL=1` 后发送 Extended CONNECT，且不回退 H1。

## P2：导入旧 codebase 的 HTTP/2

来源：同一所有者的旧 HTTP codebase，审查基线 commit
`c804424d8ea57298250f8e0b5af78bb933b9ec5e`。迁移时记录实际源 commit，并保留 frame、HPACK、
protocol/session 测试的来源映射；不复制 HTTP/3 代码。

- [x] 先导入 transport-independent 的 frame、HPACK 与 protocol engine，并把公开命名、
  export macro、Salts error 与 CMake target 适配到 CHTTP。
- [x] 不直接导入 CoroNet socket ownership。将 HTTP/2 session 的 socket/read/write/cancel/
  timer 接口改为依赖 CNet stream；h2c 使用 TCP，HTTPS/ALPN `h2` 依赖 P0 TLS。
- [x] 在 CHTTP 中定义 H1/H2 共用 request/response ownership，并保留 stream concurrency、
  SETTINGS、flow control、header list、HPACK dynamic table、GOAWAY 与 per-stream cancellation
  的硬边界。
- [x] 明确选择策略：显式 H2 失败不得降级；AUTO 只有在请求体尚未开始消费且策略允许时
  才能选择 H1。
- [x] 迁移 frame、HPACK、protocol、session、streaming、shutdown 与 public-header 测试；
  增加 CNet transport adapter 和 installed consumer 测试。

当前 frame、HPACK、protocol、CNet client/server adapter、h2c、TLS ALPN、multiplex、peer stream limit、跨
origin/H1-H2 idle eviction、per-stream cancel/response-error 隔离、GOAWAY、阻塞调用 deadline、
H2 server route/middleware/Session、双向 body source/sink、文件传输、stream 隔离、flow-control
drain、public-header 与 installed consumer 编译已覆盖。advanced per-stream timer 仍是后续增强，
但不再阻塞 HTTP/2 协议引擎与统一 H1/H2 API 的完成状态。

完成条件：CHTTP 可显式使用 h2c，并在 P0 完成后使用经 ALPN 验证的 HTTPS H2；公开 API
不暴露 CoroNet 或第三方协议类型。

## P3：导入旧 codebase 的 S3

来源：同一所有者的旧 HTTP codebase，审查基线 commit
`5f1068f5194f94472e54a185ec51638f421d4fc5`。S3 是 CHTTP 之上的应用协议模块；不得放入
CNet 或 NativeIO。迁移时记录实际源 commit，保留 SigV4、URL、XML、multipart 与响应解析
测试的来源映射，并删除所有 H3 专用策略与测试分支。

- [x] 导入 credential provider、SigV4、path/virtual-hosted URL、error、XML、SSE、bucket、
  object、multipart、lifecycle、notification 与 replication 协议模块。
- [x] 用注入的 CHTTP client/async client 替换旧 HTTP facade 和 CoroNet
  context；S3 借用 client，不能销毁它，且切换 client 时必须无 in-flight request。
- [x] signing 的 method、canonical path/query/header/body hash、authority、region 与最终
  CHTTP wire request 必须来自同一事实源；禁止签名后修改 transport-visible 字段。
- [x] 内存对象设置硬上限；大对象保留 bounded streaming、multipart、resume、ordered
  ETag、abort 与 temporary-file commit 协议，不能退化成整文件无界缓冲。
- [x] 秘钥、session token、derived signing key、SSE-C key 与 presigned secret 不记录日志；
  覆盖 signer、URL、XML、CRUD、streaming、multipart/resume/abort 和 installed consumer。

完成条件：S3 只依赖 CHTTP 的 H1/H2 能力，H3 不成为构建、运行或 fallback 依赖；真实凭据
测试仅作补充，离线协议与 mock-server 测试必须可重复。

## Castle 功能映射

| Castle/Iris 风格能力 | 当前状态 | 归属与下一步 |
| --- | --- | --- |
| HTTP/1.1 route、参数、middleware | 已实现 | CHTTP server |
| 有界内存 Cookie Session | 已实现 | CHTTP server；持久化/分布式 backend 不在当前路线 |
| 同步 template/static response | 可组合 | handler 内使用现有 parser/fs 能力 |
| TLS listener / HTTPS | 已实现 | CNet TLS transport + CHTTP H1/H2 client/server；支持 verified TLS/mTLS 与 ALPN |
| WebSocket session / WS/WSS route | H1/H2 client/server 已实现 | CNet session engine + CHTTP H1 Upgrade/RFC 8441 Extended CONNECT |
| KCP transport | 未实现 | NativeIO UDP + CNet KCP session，P1 |
| HTTP/2 | client/server 已实现 | 同一 CHTTP API；h2c prior knowledge 或 TLS ALPN；body streaming 已实现，advanced timer 待增强 |
| S3 | H1/H2 已实现 | CHTTP 之上的独立应用协议；含 SigV4、CRUD、文件传输、SSE、multipart resume 与 bucket 配置 |
| HTTP/3 | 不在范围内 | 不导入、不提供隐式 fallback |

## 统一验证门槛

- 从旧 codebase 迁移源码前确认并保留许可与 provenance；上述审查基线未发现仓库根目录的
  `LICENSE`、`COPYING` 或 `NOTICE`，因此正式迁移必须先补齐这一事实。
- 每一阶段先增加失败测试，再实现公开 API；未完成能力不得用 experimental flag 暴露。
- 每个可增长 buffer/queue/pool 都必须有硬上限、checked arithmetic、背压和 shutdown
  协议；不得用无界分配或静默丢弃处理压力。
- Release、ASan、C/C++ header、installed consumer 与相邻 CNet/CHTTP/CRPC 回归全部通过。
- 更新 CHTTP/CNet README、书稿功能矩阵与公开示例；能力不可用时返回明确错误，禁止
  TLS-to-TCP、WSS-to-WS 或 HTTP/2-to-HTTP/1.1 的静默降级。
