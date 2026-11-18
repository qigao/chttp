# 显式逻辑 HTTP 重试

`chttp_request_retry()` 为一个阻塞内存请求显式授权有限次 HTTP attempt。
与 [物理连接恢复](CHTTP_CONNECTION_RECOVERY.md) 独立配置。既有 GET/POST、async、RPC、
S3 和文件入口保持单次契约，不因方法为 GET 或收到 REFUSED_STREAM 自动重放。

```c
chttp_retry_options retry = CHTTP_RETRY_OPTIONS_INIT;
retry.max_attempts = 3;              /* 包括首次 HTTP attempt */
retry.retry_body_byte_budget = 8192; /* 所有额外 attempt 的累计 body 字节 */
retry.initial_backoff_ms = 25;
retry.maximum_backoff_ms = 250;
retry.jitter_seed = 7;
chttp_options options = {
    .connection_uri = "tls://example.com:443", .authority = "example.com",
    .target = "/item", .timeout_ms = 2000};
chttp_response response = {0};
chttp_error error = {0};
int status = chttp_request_retry(&client, CHTTP_METHOD_GET, &options,
                               &retry, &response, &error);
/* 检查 status；成功响应以 chttp_response_destroy() 释放。 */
```

## 授权边界

initializer 保持一次尝试；多次尝试要求正的 `timeout_ms`。下一次 attempt 必须同时满足：

- 旧请求已完成且真实 request/stream lease 与其拥有的存储已回收。
- 失败来自 transport 白名单，或 H2 的可靠未执行证据；没有取消、stop、安全/本地错误。
- 响应尚未开始，body 可从头重放，尝试数、累计 body 字节和固定 deadline 均有余量。
- GET/HEAD/PUT/DELETE/OPTIONS 方法幂等性、显式 `application_declares_idempotent`
  承诺、协议证明未执行三者至少满足一项。

HTTP 层记录 typed attempt facts，不根据 stage 字符串猜测。REFUSED_STREAM 或正常 GOAWAY
Last-Stream-ID 以上的 stream 可以证明未执行；错误 GOAWAY 不授权重放。普通 reset/EOF
不能证明 POST/PATCH 未执行。只有业务本身满足幂等契约时才可设置 application 声明；
这不代表远端恰好执行一次。

H1 任意响应字节、H2 已开始解码的响应 headers，或断链时无法归属的 partial frame/header
block，均终止重放资格。TLS 验证前的未知 reset/timeout 保守终止；证书、身份、ALPN、
协议错误不提供降级路径。429/5xx 是普通 HTTP 结果，不是本策略的触发条件。

阻塞 `chttp_request_retry()` 保持内存请求契约；`body_source`/`body_sink` 非 NULL 时
返回 ENOTSUP（包括 max_attempts=1），不消费它们。异步流/文件重试使用下面的显式入口；
RPC/S3 和目的地选择没有新增自动授权。

## 异步逻辑操作与回放

`chttp_async_client_submit_retry(client, request, timeouts, retry, replay, complete, user, out)`
复用同一 async client 的 poll/stop；`request.on_complete` 必须为 NULL，最终回调使用这里的
`complete`。`timeouts.total_timeout_ms` 必须为正；退避和所有 attempt 共用固定 deadline，
idle budget 每个 attempt 独立计时。URI/authority/target/headers/body 和 stream 描述符复制；
TLS profile retain，回调 user 与 stream/hook user 借用到最终回调返回。

每个 client 最多 `request_capacity` 条逻辑记录，物理 attempt 仍受原请求表约束；没有新增
线程、poller 或无界队列。`chttp_async_logical_request_cancel()` 取消 generation handle；
取消、stop、授权不足或预算耗尽都抑制下一次 admission，最终回调仅一次。poll 的 completion
计数只包含逻辑最终回调，不暴露内部失败 attempt。handle 的存储保留到实际物理资源排空，
旧 generation 不能取消新操作。所有 submit/cancel/poll/stop 都沿原单线程调用约束。

多次尝试的 source 要求已知 content length 和 `chttp_request_replay.rewind_source`；sink
要求 `rollback_sink`。缺少能力在 admission 前返回 ENOTSUP。旧 attempt 的网络发送、
文件操作和 stream lease 实际终止后，先 rewind、再 rollback，各执行一次，然后新 admission。
hook 运行于 poll 线程，不得重入或阻塞；任一失败以 `retry-replay` 终止。sink rollback 的
显式契约允许幂等请求重放部分 transport 响应；已开始响应不能再用“协议证明未执行”授权。
不提供 rollback 的内存响应继续沿原响应开始即停止重放的规则。

`chttp_async_client_submit_file_retry()` 接收 `chttp_retry_files`，upload/download path 恰选
一个，request body/source/sink 必须为空。path 在 `max_header_bytes` 内复制；文件打开在
poll 中进行，打开失败也有一次最终回调。上传每次使用既有异步 file transfer，关闭排空后
重开，核对 size/mtime/ctime；调用方必须保持路径和内容不变，metadata 核对不替代这一契约。
下载将每次输出写到同一私有相邻临时文件，旧文件关闭后 reopen/truncate；只有完整 2xx
才异步 FLUSH、close、rename。失败/取消移除临时文件，目标文件不变；非 2xx 是普通 HTTP
结果，不提交文件，也不触发重试。文件排空/提交可以晚于 HTTP 结果 deadline。

当前选用 [CMeta 0.1.8](https://github.com/qigao/cmeta/releases/tag/v0.1.8) 的 IOCP 后端
通过一个按需调度的 Windows work 对象执行 `FlushFileBuffers`。排队任务使用既有有界
request table，同一后端最多执行一个 flush；不阻塞 HTTP poll 或 IOCP dispatcher。
取消尚未执行的任务会跳过调用，已开始的刷盘必须实际返回后才能发布 terminal、释放句柄。
文件真实刷盘错误保留在 `status/native_status` 中。io_uring/Darwin AIO 使用其原生异步
FLUSH。HTTP 不切换后端或调用同步 fsync 兼容路径。回滚只需移除新增的异步声明/运行时/
调用与相关测试；既有阻塞重试及单次请求入口保持独立。

## 时间、内存与关闭协议

单个 blocking owner 顺序执行：固定 deadline → 输入快照 → attempt → terminal callback
复制 facts → 回收旧请求 → CNet gate → 有界退避 → 新 admission。同时最多一个 HTTP
attempt，不添加队列、线程或 timer。只向调用方返回一个最终 response/error。

进入调用时固定绝对 monotonic deadline，复制输入、池 admission、建连/TLS、HTTP 收发、
结果复制和退避都消耗它。CNet exponential jitter helper 使用显式 seed，等待期间驱动
同一个 async owner。退避无法装入剩余预算即超时。真实 terminal drain 仍可能晚于结果
deadline，不提前回收 NativeIO payload。H2 单 stream 失败保持正常 sibling，GOAWAY
按原排空规则换连接，不为逻辑重试重建整池。

私有 retry budget 保存固定 deadline、剩余 body 字节、尝试数和退避状态，时间由 owner
显式传入。`prepare` 在旧 attempt 排空后检查授权并计划一次退避；`advance` 在所有 CNet
gate 放行时仅提交一次尝试/字节扣减。等待中的重复 prepare 返回 EBUSY，已提交后的重复
advance 返回 EALREADY。组件本身不读时钟、不 poll、不持有协议存储或配置测试开关。

配置和 URI/authority/target/headers/flat body 在首次 admission 前复制一次。字符串使用
Base tstr；URI/authority 各受 max_header_bytes、target 受 max_start_line_bytes 限制，
header 数量/累计名称和值字节、body 分别使用现有上限。分配、算术和 admission 失败明确
传播。TLS profile 保持原借用契约，调用返回前有效。调用拥有快照直到所有 attempt 排空；
返回时释放快照，成功响应的唯一所有权交给调用方。

| 条件 | 结果 |
| --- | --- |
| 无效 size/attempts/backoff，或多 attempt 没有总 timeout | EINVAL |
| source/sink 非 NULL | ENOTSUP，不 admission |
| 上限、算术、分配、本地配置错误 | 原错误，停止 |
| 尝试耗尽、无授权、安全或响应已开始 | 最后原始 status/native/stage |
| 额外 body 字节预算不足 | ENOBUFS / retry-byte-budget，保留最后 native error |
| deadline 到期或退避无法装入 | ETIMEDOUT / retry-deadline，保留最后 native error |

## 设计选择与验证

按调用授权避免全 client 透明策略改变 RPC/S3 语义；不扩展现有无协商 options struct，
保持原布局与函数 ABI。只有使用新入口的消费方需要升级 CHttp SDK。不新增版本分发层。

正式 `chttp_h2_client_test` 使用真实 H1/H2 peer 验证 method/application/protocol 授权、
body 与精确请求/连接次数、partial response/frame、错误 GOAWAY、503、attempt/byte/
deadline 上限、原接口无重放及失败后复用。`chttp_connection_recovery_test` 验证显式
幂等声明仍不能绕过 TLS 身份/ALPN 拒绝；C API 和 C++ header/link 测试覆盖新入口。
`chttp_retry_budget_test` 用传入时间和固定 seed 验证精确退避时刻、deadline 不重置、
截止点拒绝、attempt/body credit 仅提交一次，以及取消、安全、零 body 和溢出边界；
不使用 sleep、环境时钟替换或生产测试 hook。

异步正式用例另覆盖 H1/H2 精确重放次数、输入复制、source rewind、partial sink rollback、
hook 失败、固定 deadline、累计字节、退避取消/stop、文件重开及源变化拒绝。
正式用例还覆盖 IOCP 两个 sink 共享运行时并发 flush、H1 部分下载重开后完整提交、H2
REFUSED_STREAM 后下载提交与失败后的原目标保留。其他平台的 HTTP 下载运行验收仍需在
对应平台执行。不宣称整个 #231 的全部验收完成。
