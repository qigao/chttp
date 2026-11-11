# 显式连接恢复

HTTP Client 可选择使用已发布 CNet 0.1.4 的 ManagedDial 恢复 TCP/TLS 连接能力。
默认仍为单次尝试。恢复连接不等于重放请求：已经发出的 HTTP/RPC/S3 请求失败后只完成一次，
后续由调用方独立提交的请求才能使用恢复后的连接。WebSocket client/pool 与 Pipe IPC 不启用此策略。

## 配置与公开接口

在 `chttp_async_client_init()` / `chttp_client_init()` 后、第一次请求或连接获准前配置：

```c
chttp_connection_recovery_options recovery = CHTTP_CONNECTION_RECOVERY_OPTIONS_INIT;
recovery.max_attempts = 3;          /* 包括首次物理建连 */
recovery.episode_timeout_ms = 2000; /* 一个建连/恢复 episode 的总预算 */
recovery.initial_backoff_ms = 25;
recovery.maximum_backoff_ms = 250;
recovery.jitter_seed = 7;
int status = chttp_client_set_connection_recovery(&client, &recovery);
/* 检查 status；异步客户端使用 chttp_async_client_set_connection_recovery()。 */
```

两个 setter 同步复制配置，不保留调用方内存或回调。未调用 setter，或使用 initializer 中的
`max_attempts = 1`，保持默认单次行为；大于 1 才启用恢复。延迟单位均为毫秒，退避使用 CNet
既有的有界 exponential jitter。零退避显式表示不等待，但仍受尝试数和 episode 预算限制。

| 输入/状态 | 结果 |
| --- | --- |
| NULL client/options、错误 size、零 attempts、初始退避大于上限 | `SALTS_EINVAL` |
| attempts > 1 但 episode budget 为零 | `SALTS_EINVAL` |
| 已有请求/连接 admission，或处于 poll/callback/stop/stopped | `SALTS_EBUSY` |
| 绝对 deadline 算术溢出 | admission 返回 `SALTS_ERANGE` |
| 恢复 episode 到期 | 对应请求以 `SALTS_ETIMEDOUT` 结束，再排空真实 terminal |
| 本地配置、容量、分配或未分类错误 | 保留错误，不将其转成远端重试 |

已有公开结构布局和函数签名保持兼容；新增 setter 需要包含它们的新 CHttp 构建。
阻塞客户端在超时后的内部重建会恢复同一策略，不能借重建绕过“首次 admission 前配置”的限制。

## 就绪、请求与恢复的边界

- endpoint、snapshot generation、authority、TLS profile、ALPN 和协议保持原身份，不换端点或降级。
- H1 在 CNet 完成 TCP/TLS 验证和 HTTP authority admission 后，用当前 attempt ticket 发布 READY。
  H2 还须收到有效 peer SETTINGS，并确认有实际 stream admission 容量。
- 启用恢复的 H2 在 READY 前，只发连接 preface/SETTINGS 等控制数据。应用 HEADERS 在有界
  request registry 中暂存；名称和值同步复制，正文字节沿用已有 owned body/borrowed source 契约。
  每份暂存 header storage 受 `max_header_bytes` 限制，耗尽明确失败，不另建无限等待队列。
- 建连失败时，只有从未发送 HTTP 字节、从未消费 forward-only body 的 opening 请求可以跨物理尝试等待。
  先前握手的 partial parser/output 被清理；暂存请求不靠 rewind 或重新调用应用生成器恢复。
- READY 之后断链，所有受影响请求按原失败语义完成一次。恢复的是空连接能力，旧请求不会重新进入
  admission。H2 sibling、取消请求和已经交付的结果不因后续 SETTINGS/连接 generation 而复活。
- 每个 episode 固定一个绝对 monotonic deadline，退避和尝试不重置它。阻塞和 RPC 已有的绝对
  deadline 在 admission 时限制初始 episode；普通 async HTTP 的响应超时契约不被改成新的总请求超时。
  先前 READY peer 丢失后，空连接恢复有自己的新 episode，不继承已完成请求的过期 deadline。

## 失败分类

复用 CNet 的分类回调，仅允许明确的连接拒绝、reset、abort、timeout 等瞬时 transport 失败。
部分 NativeIO 完成由 CNet 报 `SALTS_EIO` 并保留原生错误码，因此分类器对白名单原生码进行精确识别；
未知码（包括不可取绝对值的 int 边界输入）永久终止。最终失败保留对应 status/native status。

`SALTS_EAUTH` 和所有非 NONE 的 TLS diagnostic 均不恢复。TLS 验证完成前，未知 reset/timeout
可能是对端拒绝 SNI/身份/握手，不能按普通断链重试；只有明确的 TCP connection refusal 可继续尝试。
证书、身份、ALPN、协议错误和本地配置错误不授权重放、换信任根或明文 fallback。

## 所有权与实现选择

一个固定物理 session 拥有一个 address-stable recovery adapter。它借用现有 CNet client、Manager
和 Pool，既不创建另一套 runtime/observer，也不增加线程或 timer。Manager 中的 context 归
ManagedDial；HTTP 通过自身有界 session 数组和完整 managed slot/generation 找到协议状态，
不再将外部 context 指针强制转换成 H1/H2 session。

选择 admission 时的有界查找，避免再维护一份具有独立失效时机的 context registry。查找成本与配置的
session 数量线性相关，不进入逐字节解析路径；本变更不声称性能提升。另一套 Manager 会拆分容量与
terminal 权威，因此不采用。直接重放整个 HTTP 请求则破坏业务与 streaming source/sink 契约，也不采用。

冷建连的 CONNECTING pool reservation 可跨顺序 opening 尝试保留。原 READY pool entry 在 transport
结束后先退役；旧 logical request、stream lease、retained write 和 Manager record 均 settlement 后，
才能建立新 CONNECTING entry，不能把旧 READY entry 绑定给新连接。

stop 先关闭 recovery admission，再排空协议和 NativeIO。H2 保留 GOAWAY flush 的关闭顺序；取消最后一个
opening 请求会终止该 opening recovery。ManagedDial 和协议存储都必须活到真实 terminal/recycle。
episode deadline 引发的 close 在真实 terminal 后以超时交给 HTTP，不能被解释为成功响应 EOF。

## 验证与回滚

正式测试归属 `http_client/tests/` 和 `http_common/tests/`：

- `chttp_connection_recovery_test`：默认/配置、冷 H1/H2/HTTPS、owned headers 与 forward-only source、
  stale READY ticket、READY 丢失后不重放、退避中取消/停止、attempt/episode 限额、TLS 身份和 ALPN
  永久失败、阻塞重建、H2 sibling、blocking/RPC deadline。
- `chttp_pool_lifecycle_test`：暂扣真实 NativeIO 接收/发送完成，验证 SETTINGS 前不发应用 HEADERS、
  取消不复活、lease/payload/attachment 的释放不早于真实 terminal。
- `chttp_h2_proto_test`：无应用请求的 client preface/SETTINGS 启动；原有 HTTP/H2/TLS/RPC/S3 测试继续运行。
- 公开 C API 和既有 C++17 header/link 测试覆盖新增 setter，不创建独立 consumer 验证工程。

回滚须一起移除新增公开声明、实现与调用；保持原单次路径及其测试。没有运行时 fallback、第二实现
开关或协议降级。#231 的逻辑 HTTP retry 授权/编排仍未实现，不能据此关闭整个 issue。
