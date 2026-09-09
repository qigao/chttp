# CHTTP HTTP/2 与 S3 设计

## 背景与范围

本阶段把同一所有者的旧 HTTP codebase 中的 HTTP/2 与 S3 能力迁入
Salts。审查基线 commit 为
`38f1e389b3f94909db6cb2482a8cbc16522e7e4f`。源仓库没有根级
`LICENSE`、`COPYING` 或 `NOTICE`；迁移保留本节的来源、commit 与作者记录，
不推断或新增许可文本。

S3 协议边界与测试思路另外依据旧 codebase commit
`5f1068f5194f94472e54a185ec51638f421d4fc5` 重建；准确的文件映射和许可事实记录在
`s3/PROVENANCE.md`。

范围包括 HTTP/2 client/server、h2c、TLS ALPN `h2`、同步 request/reply、异步
submit/callback、共享 server handler chain、S3 SigV4、对象 CRUD、流式传输与 multipart。HTTP/3 不在范围内，
也不得成为构建依赖或运行时 fallback。

## 架构决策

HTTP/2 不新增一套并列的公开 client。`chttp_client` 与
`chttp_async_client` 仍是唯一 client 入口，请求通过显式协议选择进入 HTTP/1.1
连接池或 HTTP/2 session 池。缺省值保持 HTTP/1.1，因此现有零初始化调用方的行为不变。

HTTP/2 分三层：

1. `chttp_h2_frame`、`chttp_h2_hpack`、`chttp_h2_proto` 是私有、无 I/O 的协议核心；
2. `chttp_h2_session` 独占一个 CNet stream、协议引擎、stream table 与 pending output；
3. `chttp_client` 把公开 request handle、取消、callback 和 owning response 投影到 H1 slot
   或 H2 stream；requests-style 调用另外拥有本次阻塞等待的 deadline。

S3 是独立的上层模块，只借用调用方提供的 CHTTP client。S3 的 canonical request
是签名与 wire request 的唯一事实源；签名完成后不得修改 method、authority、path、query、
headers 或 body hash。

### 下游依赖边界

`llhttp` 与 `c-ares` 是实现细节。安装包使用者只链接 `Salts::CHTTP`、
`Salts::CNet` 或更高层 target，不需要发现、链接或配置这两个包。由于静态库的 PRIVATE
依赖仍会以 link-only 形式进入 CMake export，CNet 与 CHTTP 使用共享库边界封装各自私有
依赖；Windows 生成 import library/DLL，Linux 生成带项目版本的 shared object。公开
header 不出现第三方类型，安装测试显式禁用两个 package 的发现以验证边界。

## HTTP/2 数据与生命周期协议

- 线程拓扑：一个 `chttp_async_client` 由调用其 API/poll 的单线程拥有；一个 H2 session
  只有该 owner 推进。同步 client 也由当前调用线程推进，不创建私有 worker。公开对象不承诺 MPMC。
- 请求输入：submit 在返回前复制 method、authority、target、headers 与 flat body；成功后
  调用方可立即释放输入。流式 source 只能在 owner 上回调，回调返回前借用输出 buffer。
- 响应输入：HPACK callback 的 name/value 与 DATA view 只在 callback 内有效。普通 async
  response view 在 completion callback 返回时失效；同步 response 复制为 owning storage。配置 sink
  时 DATA 先由 sink 成功消费再归还窗口，response body 不保留但累计 size 仍可见。
- session key：`connection_uri + authority + TLS profile identity + HTTP version`。HTTP/1.1
  与 HTTP/2 连接不能互相复用。物理池满时只驱逐没有活动 request/stream 的连接；advanced
  submit 返回一次 `SALTS_ENOBUFS`，由 owner poll 后重试，requests-style 调用在内部完成该过程。
- 状态：`NEW -> CONNECTING -> ACTIVE -> DRAINING -> CLOSED`，任一阶段可进入
  `FAILED`。收到 GOAWAY 后停止接收新 stream；已接受 stream 继续收敛，超过
  Last-Stream-ID 的请求返回明确的未处理错误。
- 取消：未提交 stream 直接完成为 canceled；已提交 stream 发送
  `RST_STREAM(CANCEL)`，并且每个请求只产生一次 terminal callback。
- 隔离：单 stream 的响应语义错误或 body/header 上限错误发送 RST_STREAM；HPACK block 仍完整
  解码以保持动态表一致，兄弟 stream 和物理连接继续工作。
- 关闭：stop 关闭 admission、发 GOAWAY 并 drain 已提交 stream；timeout 返回可重试的
  `SALTS_ETIMEDOUT`，不偷偷取消剩余 stream。destroy 只在 stop 完成且没有 callback 正在执行时释放。

### 硬上限与背压

所有加法和乘法先做 checked arithmetic。默认值可由 client config 调整：

| 资源 | 默认上限 | 满额行为 |
| --- | ---: | --- |
| 本地并发 stream | 100 | submit 返回 `SALTS_EBUSY` |
| frame payload | 16 KiB | connection protocol error |
| header list | 64 KiB | stream header-too-large error |
| HPACK dynamic table | 4 KiB | SETTINGS/HPACK 超界为 compression error |
| 单 stream 请求或响应 body | 100 MiB | stream body-too-large error |
| session retained response | 128 MiB | 暂停 receive；无法恢复时 fail fast |
| 单 stream pending output | 128 KiB | source 暂停并等待 WINDOW_UPDATE/send completion |
| parser input/header block | frame/header 上限 | 超界立即终止 connection |

协议核心的 growable buffer 带 `max_capacity`；reserve 超过上限返回 capacity error，不允许
无界扩容。stream table 容量在 session 创建时确定；terminal stream 的 active slot 可立即复用。
本地 RST 的 stream id 进入同容量的固定环形历史，用于有界吸收迟到 frame，且绝不把迟到数据
投递给新 request；历史满时覆盖最旧 id，不让协议元数据随连接寿命无界增长。

## TLS 与协议选择

- 显式 HTTP/2 over TLS 要求 TLS profile 提供 `h2` ALPN，并在 CONNECTED 后通过
  `cnet_tls_negotiated_alpn()` 验证结果恰为 `h2`；缺失或其他结果返回 protocol error。
- 显式 h2c 使用 prior knowledge client preface，不发送 HTTP/1.1 Upgrade。
- 显式 HTTP/2 失败不得降级到 HTTP/1.1。
- AUTO 只有在请求 body 尚未交给 source 且调用方显式允许时才可选择 HTTP/1.1；首版不
  暴露 AUTO，避免产生隐式重放语义。

## HTTP/2 Server 设计

HTTP/2 Server 复用现有 `chttp_server` 门面、route、middleware、Session 与后台
owner thread，不新增第二套 handler API。零初始化 server config 仍只接受
HTTP/1.1；显式启用 H2 后，同一 listener 按如下规则选择协议：

- 明文 TCP 只支持 h2c prior knowledge。首批字节逐字节匹配 24-byte client
  preface；完整匹配进入 H2，首次不匹配即将已保留字节全部交给 H1 parser。
  不支持 `Upgrade: h2c`，也不把 H2 失败回退成 H1。
- TLS 在 CONNECTED 事件后查询 ALPN；协商结果 `h2` 进入 H2，`http/1.1`
  或没有 ALPN 进入 H1，其他结果 fail fast。启用 H2 的 TLS server 可按
  server preference 配置 `h2`, `http/1.1`；未启用 H2 时仍只允许 H1。

### 统一 API 与协议适配模式

公开层只保留一套 `chttp_server_*`、route、middleware、Session 与 response builder API。
H1 的 llhttp adapter 和 H2 的 frame/HPACK adapter 分别把 wire message 归一化为同一个
`chttp_server_request_view`，之后复用固定的 dispatch 模板：route match、middleware chain、
Session begin/commit、handler、response builder。协议差异只留在连接协商、解析/编码、flow
control 与 shutdown adapter 中。

当前只有 H1/H2 两种实现，协议在每条连接上选定后不再变化，因此热路径保留按连接协议的可预测
分支，不为形式统一增加 CMeta `interface(...)` 函数指针分派，也不让 CHTTP 新增 CMeta 链接依赖。
CMeta 适合生成元数据和薄 facade，运行时 buffer、stream 生命周期与协议算法仍由普通 C 状态机
拥有。若未来确实加入第三种 wire protocol，再把私有 adapter 边界提升为 Strategy vtable；公开
ABI 与 handler API 无需随之改变。

### 数据、所有权与状态

- 数据单元是一条 H2 stream。每条 stream 拥有独立的 header 数组、header 存储、普通 route
  request body、route params、response builder 和 Session context；streaming route 把 DATA
  直接交给 sink。所有 request 指针均在对应 callback 内有效；memory response 保留到协议引擎
  不再借用它，source response 则按窗口逐块拉取。
- 连接是 H2 协议、HPACK 动态表、flow-control window 和 stream table 的唯一
  事实源。stream 是 request/response/Session 状态的唯一事实源。
- 连接状态为 `UNKNOWN -> H1 | H2 -> DRAINING -> CLOSED`；stream 状态为
  `FREE -> HEADERS -> BODY -> DISPATCHED -> RESPONDING -> CLOSED`，异常可进入
  `RESET`。一条 stream 的请求语义错误只发 RST_STREAM，HPACK/连接帧错误才终止物理连接。
- handler 只在收到 END_STREAM 后运行，且仍由唯一 owner thread 串行调用。
  wire 层允许多流交错，但不声明 handler 并行或可重入。
- stop 先停止 listener admission，对 H2 连接发送 GOAWAY，将已接收的响应输出
  flush；应用层排空后发送 drain PING，匹配的 ACK 证明此前响应和控制帧已经按序到达，
  server 在该 receive callback 边界关闭 transport。对不响应 PING 的 peer 保留 64 个
  `poll_slice_ms` 的有界兜底；stop timeout 保持现有可重试的 `SALTS_ETIMEDOUT` 语义。

### 容量、背压与失败

- `h2_stream_capacity` 是每条物理连接的 stream 槽与本地
  SETTINGS_MAX_CONCURRENT_STREAMS 上限。达到上限的新 stream 被协议层拒绝，
  不转为无界分配。
- active stream slot 在 terminal callback 后立即复用；本地 RST id 另存于同容量的固定环形历史，
  用于忽略已在途的 DATA/HEADERS/WINDOW_UPDATE/RST。历史满时覆盖最旧 id，不让 reset tombstone
  永久占用 active table。
- header count/bytes、target、request body、response header/body 复用现有
  `chttp_server_config` 硬上限。H2 input、output、header block、HPACK table 与
  SETTINGS count 均有创建时固定的硬上限，所有容量运算先检查溢出。init 根据公开的
  request/response header count 与 bytes 计算包含 HPACK field/pending-update 开销的最坏界；
  output 无法保证编码时立即拒绝配置，不把失败推迟到 handler 已提交 Session 之后。
- DATA 只在成功拷贝进该 stream 的有界 body buffer 或被 route/response sink 完整消费后归还
  connection/stream flow-control credit。超限时对该 stream 发 `RST_STREAM(ENHANCE_YOUR_CALM)`，
  不影响兄弟 stream。
- CNet 每条连接只有一个 in-flight send。协议引擎的 bounded output 拷贝进
  连接 outbound buffer 后才可继续调用 engine；`ENOBUFS/EBUSY` 保留待重试操作，
  不丢帧、不覆盖、不无界增长。

## S3 一致性协议

- credential provider context、TLS profile 与注入的 CHTTP client 始终 borrowed，并且必须
  活过 S3 client 与全部请求；endpoint、authority 与 region 在 init 中复制。
- object body、stream chunk、ETag 与 parsed XML 都有明确 owning/view 形式；callback view
  在 callback 返回时失效。
- multipart 的事实源是 upload id、part number、ETag 与已提交状态。complete 前按 part
  number 排序并拒绝缺口；HTTP 200 body 为空、格式错误或包含 `<Error>` root 时仍视为
  protocol/service failure，handle 保持 ACTIVE 以便重试或 abort。SSE-C 的派生请求头由 handle 持有并在每个 UploadPart 重复，
  destroy 前清零；checkpoint 只保存 key MD5 fingerprint，不保存 raw/base64 key。
- 高层 multipart checkpoint 通过同目录临时文件、fsync 与原子 rename 更新；只有显式
  `preserve_on_failure` 且已有有效 checkpoint 时才保留 server upload，否则尝试 abort。
- 文件下载写入同目录临时文件，校验成功后原子 rename；失败删除临时文件，不覆盖目标。
- access key、secret、session token、derived key、SSE-C key 与 presigned query 不写日志。

## 兼容性、验证与回滚

新增字段只追加到公开配置结构尾部；零初始化保持 HTTP/1.1。源码兼容，重新编译后生效。
由于 C 结构大小变化，本阶段把 CHTTP library version 提升到 2.0.0、ABI/SOVERSION 提升到 2。
Unix 通过 `libsalts_chttp.so.2` SONAME、Windows 通过 `salts_chttp-2.dll` 与对应 import library
隔离旧布局；`Salts::CHTTP` 的 CMake target 名保持不变。ABI 1 二进制不会意外装载 ABI 2，源码
使用者仍必须使用匹配头文件重新编译、链接。未来继续扩展公开 options 时，应引入
`struct_size`/版本化 options 或再次提升 ABI major，不能假定追加字段天然二进制兼容。

验证按 frame、HPACK、in-memory protocol、CNet h2c、TLS ALPN、同步/异步 API、shutdown、
S3 signer/URL/XML、mock-server CRUD、streaming/multipart、C/C++ header、installed consumer
逐层进行，再运行 Release、ASan 与相邻 CNet/CHTTP/CRPC 回归。

协议核心、CNet adapter 和 S3 是独立提交。某一层回滚不得改变较低层公开接口；未完成的
能力保持私有，不以 experimental flag 暴露。
