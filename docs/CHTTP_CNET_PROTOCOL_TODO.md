# CNet / CHTTP 协议能力 TODO

## 当前基线

核对日期：2026-11-18。当前 `.env` 选择 SaltsCNet 0.1.16；版本和 SDK 路径只由 `.env`
定义，本文记录能力与待办状态，不作为上游选择入口。

截至当前基线，CNet 已提供 TCP、TLS、UDP、有界 KCP session/packet endpoint、Pipe、TCP listener、plain/TLS accepted-socket
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
连接池与文件 source/sink。CNet 已有 KCP 不代表 CHTTP 已接入 HTTP-over-KCP：当前 HTTP
`connection_uri` 接受 `tcp://`、`tls://`，H1 另支持 `pipe://`，不得声明 CHTTP 的 KCP transport 可用。
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
- [x] 正式测试覆盖可信/不可信证书、hostname mismatch、mTLS、部分 record、超时、取消、
  close-notify、accepted socket 和 C/C++ 公开头文件。

完成条件：`tls://` 能以 mandatory verification 建连，CNet server caller 能通过同一
accepted-connection 路径终止 TLS；CHTTP client/server 复用该路径，并以
`connection_uri + authority + TLS profile identity` 隔离连接池。相关代码改动复用项目正式
CTest、既有 sanitizer presets 和正常 SDK 安装入口验证。

## P1：CNet KCP、WebSocket engine 与 CHTTP Upgrade

- [x] KCP 会话已在 CNet：NativeIO 提供 UDP datagram 与 timer，CNet owner 独占
  conversation、window、retransmit、ordered delivery、MTU 和 shutdown 状态。
- [x] `cnet_kcp_config` 已限制 MTU、send/receive window、interval、send segment capacity
  与 max message bytes；NativeIO 不承担 KCP packet、重传或消息语义。
- [x] `saltscore.cnet_policy` 已覆盖真实 peer ACK 后的逻辑 send terminal、重传、窗口满、
  timer 推进、未确认 tag 的取消、stop/drain，以及相邻 UDP/KCP session 隔离。
- [x] `cnet_kcp_set_dead_link()`、`cnet_secure_kcp_set_dead_link()` 与
  `cnet_packet_endpoint_set_kcp_dead_link()` 在首次使用前配置最大发送次数（默认 20，含首发）；
  耗尽永久返回 `SALTS_ETIMEDOUT`。secure handshake 同样有界；没有待确认发送的 idle peer
  不因空闲超时。公开 terminal-status 查询复用这一状态，既有 config 布局不变。
- [x] 正式 `cnet_kcp_fault_test` 覆盖 loss/reorder/duplicate 的八种组合、时钟回绕、plain/
  secure blackhole；`saltscore.cnet_policy` 验证真实 UDP tagged/untagged 超时、terminal
  一次性与 generation 复用。未建立独立验证工程。

核对依据为上游 `cnet/include/cnet/cnet.h`、`cnet/src/cnet_kcp.c`、
`cnet/src/cnet_packet_endpoint.c` 和 `cnet/tests/test_cnet_policy.c`。

- [x] 复用 CNet 的 `cnet/src/wsparser` 作为唯一 WebSocket frame parser；已覆盖 RFC 6455 canonical
  length、reserved opcode/RSV、NULL/overflow、控制帧边界与 borrowed payload 失效点，不在
  CNet 或 CHTTP 中复制第二套 frame parser。
- [x] 定义有界 WS message API：text/binary、fragment、ping/pong、close code/reason、
  max frame bytes、max message bytes、UTF-8 validation 和 backpressure。
- [x] 在该 frame parser 的语法结果之上实现 RFC 6455 session 状态机；client frame 必须 mask，
  控制帧、fragmentation 和 close handshake 必须严格校验。
- [x] CNet WebSocket engine 只依赖有序双向 byte stream，不解析 HTTP header；同一 frame/
  message/session 状态机同时承载 CHTTP HTTP/1.1 Upgrade 与 HTTP/2 RFC 8441 stream。
- [x] CHTTP 使用现有 llhttp request parser 完成 opening handshake 的 header/route 校验；Upgrade
  成功前不把应用 frame 字节交给 CNet WebSocket session。
- [x] 为 `ws://` 和 `wss://` client 实现 HTTP Upgrade；`wss` 必须复用 P0 TLS transport。
- [x] 在 CHTTP server 增加显式 WebSocket route。CHTTP 负责路由与 Upgrade header 校验，
  H1 成功后通过 `cnet_websocket_transport_init()` 绑定已有连接，H2 由 CHTTP 的 stream
  adapter 向同一 engine 馈入数据，CHTTP 继续管理 H2 stream 生命周期和流控。
- [x] HTTP/2 由 CHTTP 校验 extended CONNECT 与 `:protocol = websocket`，再把该 H2
  stream 适配给同一 CNet WebSocket engine；不得为 H1/H2 复制两套 WS 状态机。
- [x] 明确协议切换：HTTP parser 不继续消费 WebSocket frame；opening 失败返回 HTTP 错误，
  成功后 frame 进入 WebSocket engine。request/Session 等借用视图按既有回调契约结束，
  不得在回调结束后继续使用。
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
  CNet transport adapter 由本项目正式测试验证。

当前 frame、HPACK、protocol、CNet client/server adapter、h2c、TLS ALPN、multiplex、peer stream limit、跨
origin/H1-H2 idle eviction、per-stream cancel/response-error 隔离、GOAWAY、阻塞调用 deadline、
H2 server route/middleware/Session、双向 body source/sink、文件传输、stream 隔离、flow-control
drain 与 public-header 编译已覆盖。advanced per-stream timer 仍是后续增强，
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
  正式测试覆盖 signer、URL、XML、CRUD、streaming、multipart/resume/abort 和 C/C++ 公开头文件。

完成条件：S3 只依赖 CHTTP 的 H1/H2 能力，H3 不成为构建、运行或 fallback 依赖；真实凭据
测试仅作补充，离线协议与 mock-server 测试必须可重复。

## CNet 连接恢复前的 READY 验证（#231）

CNet 0.1.4 是该边界的早期接入基线；当前依赖以 `.env` 为准。
`http_client/tests/chttp_pool_lifecycle_test.c` 在项目正式 CTest
入口增加确定性交错：真实 TCP 连接已完成，但暂扣属于该连接的 NativeIO receive completion，
让 HTTP/2 暂时看不到 peer SETTINGS。用 request slot/generation 识别被暂扣的完成，其他完成
仍正常路由；fixture 在成功、失败和 teardown 时都负责交还该完成。

- CONNECTED 后仍保留 CONNECTING 池记录，READY 为零；解析 SETTINGS 后才可发布 READY。
- 在 SETTINGS 前取消的请求保持 `SALTS_ECANCELED`，迟到的 peer 字节不能重新交付成功，
  旧 request handle 不再有效。健康连接随后可以进入 READY，但不复活已取消请求。
- 连接计数保留一个 fixture H1 连接加一个 H2 连接，没有额外的恢复连接。

显式 ManagedDial 已接入 `http_client/src/chttp_connection_recovery.c`，使用
`cnet_managed_dial_init_admitted()` 和当前 attempt 的 protocol READY；
`chttp_connection_recovery_test` 与上述 pool 测试覆盖接入边界。配置和扩展测试见
[连接恢复设计与接口](CHTTP_CONNECTION_RECOVERY.md)。阻塞内存请求可使用独立的
[逻辑重试入口](CHTTP_LOGICAL_RETRY.md)；异步逻辑操作另有显式入口和回放契约。
接入使用当前 attempt 的 recovery ticket，并同时满足 protocol READY、旧请求/
保留字节/stream lease 的 terminal settlement 和 Manager recycle；不能把单个池快照当作所有
资源均可回收的证明。恢复开关及预算必须显式，TLS/ALPN/authority 失败不能授权重放或降级。

## 后续增强

- [x] `chttp_async_client_submit_with_timeouts()` 为 H1 请求与 H2 stream 提供固定 total 和
  仅由本响应进展续期的 idle budget；复用现有 poll/cancel/drain，RPC deadline 契约保留。
- [x] `chttp_async_websocket_*` 提供 caller-driven WS/WSS H1/H2，固定 progress 线程，
  多生产线程有界复制提交/取消、generation handle、FIFO terminal callback 和 stop/drain。
- [x] `chttp_async_client_submit_retry()` 提供固定 deadline/累计字节预算、单次最终回调与
  逻辑取消；source rewind、sink rollback 在旧 attempt 真实排空后执行。
  `chttp_async_client_submit_file_retry()` 关闭并重开上传源，拒绝 metadata 变化；下载重开
  私有临时文件，完整 2xx 才 flush/close/rename。当前选择 CMeta 0.1.8 / SaltsUtils 0.1.6：
  IOCP 的 FLUSH 由有界 Windows work 异步执行，取消等待实际刷盘终态；不阻塞调用线程或
  IOCP dispatcher。原单次/RPC/S3 入口不变。

## Castle 功能映射

| Castle/Iris 风格能力 | 当前状态 | 归属与下一步 |
| --- | --- | --- |
| HTTP/1.1 route、参数、middleware | 已实现 | CHTTP server |
| 有界内存 Cookie Session | 已实现 | CHTTP server；持久化/分布式 backend 不在当前路线 |
| 同步 template/static response | 可组合 | handler 内使用现有 parser/fs 能力 |
| TLS listener / HTTPS | 已实现 | CNet TLS transport + CHTTP H1/H2 client/server；支持 verified TLS/mTLS 与 ALPN |
| WebSocket session / WS/WSS route | H1/H2 client/server 已实现 | CNet session engine + CHTTP H1 Upgrade/RFC 8441 Extended CONNECT |
| KCP transport | CNet 已实现；CHTTP 未接入 | CNet session/packet endpoint 已有；dead-link/peer timeout 与完整故障矩阵见 P1 |
| HTTP/2 | client/server 已实现 | 同一 CHTTP API；h2c prior knowledge 或 TLS ALPN；body streaming 已实现，advanced timer 待增强 |
| S3 | H1/H2 已实现 | CHTTP 之上的独立应用协议；含 SigV4、CRUD、文件传输、SSE、multipart resume 与 bucket 配置 |
| HTTP/3 | 不在范围内 | 不导入、不提供隐式 fallback |

## 统一验证门槛

- 从旧 codebase 迁移源码前确认并保留许可与 provenance；上述审查基线未发现仓库根目录的
  `LICENSE`、`COPYING` 或 `NOTICE`，因此正式迁移必须先补齐这一事实。
- 每一阶段先增加失败测试，再实现公开 API；未完成能力不得用 experimental flag 暴露。
- 每个可增长 buffer/queue/pool 都必须有硬上限、checked arithmetic、背压和 shutdown
  协议；不得用无界分配或静默丢弃处理压力。
- 代码改动通过相关正式 CTest、C/C++ header 与相邻 CNet/CHTTP/CRPC 回归，并按风险运行
  既有 sanitizer presets；SDK 使用正常 install/package 入口。不新增或恢复独立 consumer、
  installed-consumer 或 smoke 验证工程。纯文档改动核对事实、路径、引用和 diff。
- 更新 CHTTP/CNet README、书稿功能矩阵与公开示例；能力不可用时返回明确错误，禁止
  TLS-to-TCP、WSS-to-WS 或 HTTP/2-to-HTTP/1.1 的静默降级。

## 本次核对记录

- 上游 [SaltsCNet v0.1.16](https://github.com/qigao/salts-cnet/releases/tag/v0.1.16)
  发布源为 `ba44f12afdc385421e822b6b97a80c3e6c2c45f3`；十个平台 SDK 来自
  [producer 44213515721](https://github.com/qigao/salts-cnet/actions/runs/44213515721)。
  八个 native profile 的正式 CTest 全通过（Windows 102，其余 104，含 TSan/ASan/UBSan）；
  Android 仅完成交叉构建/安装及 ELF 对齐验收，不宣称设备运行。
- HTTP 从 `.env` 选择并校验恢复该公开原始 SDK，执行 `cmake --fresh --preset native-release-user`。
  切换到 CMeta 0.1.8 / SaltsUtils 0.1.6 后全量构建、123/123 CTest（101.37 秒）和安装通过；
  IOCP 下载提交与并发 FLUSH 的定向回归通过。新记录位于 `build/async-flush/`，
  先前协议补全记录保留在 `build/fill-protocol-gaps/`。
  HTTP 的其他平台/sanitizer 未在本机运行。IOCP FLUSH 与下载提交的后续记录见
  [逻辑重试验证](CHTTP_LOGICAL_RETRY.md#设计选择与验证)。
