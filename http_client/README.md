# HTTP 与 RPC 客户端

`CHttp::Client` 包含 HTTP 请求、连接池、H1/H2、TLS、WebSocket client/pool、
文件上传下载和同步/异步 JSON-RPC 调用。S3 在独立的 `../s3/` 模块中，依赖此库。

公开入口为 `<http_client/http.h>`、`<http_client/rpc.h>`，位于本模块的 `include/`。
`src/` 管理 HTTP 实现，`rpc/` 管理 RPC 调用实现，`tests/` 管理客户端测试。
共享协议与 codec 位于 `../http_common/`；两端集成测试位于 `../http_common/tests/`。

客户端不链接服务端库。请求 owner、取消、完成回调和关闭协议保持原有语义。
详细接口见 [HTTP](../docs/HTTP.md)、[RPC](../docs/RPC.md)。

可选头文件 [`response_scope.h`](include/http_client/response_scope.h) 为
`chttp_response` 提供 C11/C++17 的 CMeta 静态生命周期：用 `cmeta_scope`
执行返回状态码的 body，结束时释放 response 拥有的内存。完整可运行用法见
[`chttp_response_scope_cpp_test.cpp`](tests/chttp_response_scope_cpp_test.cpp)。需要将结果带出作用域时，
先用 `chttp_response_move(&destination, &response)` 转移到不同的零初始化目标；
源随后归零，禁止按值复制拥有资源的 response。资源由单线程 owner 管理，转移本身不提供同步。

`cmeta_reflected_data(chttp_response)` 只描述协议版本、状态码、header 数量、body
字节数和 keep-alive 摘要，不暴露 payload 指针，不提供构造或反序列化权限。
反射读取期间 response 必须存活且不被并发修改；跨翻译单元使用语义比较，不比较描述符地址。
静态 scope 的无失败清理契约不适用于 client 的 stop/drain；这些操作仍须检查错误。

响应复制以临时 owner 保存部分结果，成功后一次性转移；失败释放已分配的所有资源，
输出保持零值。流式下载仍允许 `body == NULL` 且 `body_size` 为已传输字节数。
用 `chttp_response_owner_test` 验证每个分配失败点、提前返回、转移及重复清理，
用 `chttp_response_scope_cpp_test` 验证 C++ 消费入口；请求与文件下载回归归属
`chttp_requests_test`。

## 显式配置远端选择策略（#231 的 destination 部分）

`chttp_async_client_submit_to()` 和阻塞 `chttp_request_to()` 接受版本化的
`chttp_destination_options`。旧 `submit/get/post` 等入口保持直接 URI、单次请求语义。
策略选择由实际 `cnet_destination_choose()` 执行；CHttp 随后负责连接及 HTTP 容量接纳。

| `selection.kind` | 配置与行为 |
| --- | --- |
| `CNET_DESTINATION_ROUND_ROBIN` | 应用提供 `sequence`，在 eligible 端点间轮询 |
| `CNET_DESTINATION_WEIGHTED_RR` | 使用正整数 `weight` 和 `sequence` |
| `CNET_DESTINATION_LEAST_INFLIGHT` | 使用应用快照中的 `inflight`，不是自动测量或容量预留 |
| `CNET_DESTINATION_EXPLICIT` | 使用 `explicit_endpoint_id`，目标不可用即失败 |
| `CNET_DESTINATION_STRICT_KEY` | 设置 `key_known`、`key_hash`；按稳定 ID 选择，目标不可用不转邻居 |

以下片段用于已经初始化的阻塞 `client`，调用方负责检查状态并销毁拥有资源的响应：

```c
cnet_destination_hint endpoints[] = {
    {.endpoint_id = 10u, .weight = 1u, .eligible = true},
    {.endpoint_id = 20u, .weight = 2u, .eligible = true}};
const char *uris[] = {"tcp://127.0.0.1:8080", "tcp://127.0.0.1:8081"};
chttp_destination_options policy = CHTTP_DESTINATION_OPTIONS_INIT;
policy.selection.kind = CNET_DESTINATION_WEIGHTED_RR;
policy.selection.endpoints = endpoints;
policy.selection.endpoint_count = 2u;
policy.selection.snapshot_generation = 1u;
policy.selection.sequence = 0u; /* 应用为后续选择提供新的 ticket。 */
policy.connection_uris = uris;
chttp_options request = {.authority = "service.example", .target = "/",
                         .timeout_ms = 5000u};
cnet_destination_result selected;
chttp_response response = {0};
chttp_error error;
int status = chttp_request_to(&client, CHTTP_METHOD_GET, &request, &policy,
                              &selected, &response, &error);
/* status == SALTS_OK 才读取 response；失败时检查 error.status/stage。 */
chttp_response_destroy(&response);
```

异步调用使用相同 policy；请求的 `connection_uri` 必须为 NULL。端点 ID 必须非零、
严格递增，URI 数组与 hint 数组等长且逐项对应。宿主先按 authority、TLS 身份、协议和
健康条件过滤，所有 URI 必须采用同一种 TCP/TLS 传输；不接受 Pipe 或混合明文/TLS。
每次调用校验快照；`expires_at_ms` 使用单调毫秒时钟，默认 `UINT64_MAX` 不过期，
输入 `now_ms` 被实际单调时钟替换。相同 client/origin 内保持 ID/代际命名空间一致，
成员或身份变化必须发布新的非零 `snapshot_generation`。新代际隔离后续复用，
已经接纳的请求继续使用旧连接完成；发布快照不会主动取消或关闭旧代际。

异步 submit 返回后可释放快照和 URI；已接纳的请求拥有 URI 副本及选中身份。
阻塞调用的输入存活至返回。端点 ID、代际、URI、authority、保活 TLS profile 和协议
共同约束 H1/H2 复用，也覆盖 H2 等待初始 SETTINGS 时的 stream 接纳。
选择失败将输出 ID 清零、index 设为 `SIZE_MAX`；选择成功而接纳失败仍输出选中身份，
不交付完成 callback。`ENOBUFS` 不等待、不换端点；底层可以启动既有空闲连接驱逐，
后续再次调用由应用明确决定。阻塞总 deadline 从选择前开始计算；超时后的 terminal
drain 仍按既有协议完成，不能提前回收借用资源。

采用逐次调用的宿主快照，避免在客户端内复制一套服务发现、健康和负载事实源。
替代方案是客户端保有可变端点 registry，但它需要新增更新、同步和失效协议；本阶段
无需承担这些状态。迁移只需将需要选择的调用改用新入口；回退到直接 URI 入口即可
停用策略，两类连接身份隔离。没有增加 SDK 依赖或修改旧配置结构布局。
完整可运行用例在 [chttp_destination_test.c](tests/chttp_destination_test.c)，覆盖五种策略、
H1/H2 复用与代际隔离、相同 URI 的不同 ID、输入释放、失效快照、容量拒绝及阻塞入口。

这里完成的是远端选择接入。共享宿主 SG（#230）、managed reconnect、显式请求 retry
及其 deadline/rewind/not-executed 证明（#231 其余部分）仍未接入；本入口不启用这些行为。

## RC2 WS composition (#238)

[联合验收 #238](https://github.com/qigao/chttp/issues/238) 的当前阶段接入专用 H1
WS/WSS 客户端 writer 和可配置的服务端 writer；它不是整个 issue 的完成声明。依赖为配套发布的
Salts `2.3.0-rc.2`（`081bdff4da141c8042faa83aff0d25eae1077d5e`）和
SaltsUtils `4.3.0-rc.2`（`c96a47b05292e3bbebba0b016c0b18c71bce8cde`），
来源为 GitHub NuGet 的 installed Windows x64 Release SDK，manifest 已核对。
CHttp 基线为 `e46187214dca7d4572811704c159b9a29ad26b4a` 加当前未提交改动。

`chttp_websocket_client_connect()` 先等待真实 Upgrade 请求写完成，再接收并校验
HTTP 响应、accept 和 subprotocol，最后绑定 `cnet_websocket_transport`。
桥接后的连接由 CNet 独占写入；普通 HTTP `on_send` 不再解释 WS 输出，即使握手
与首帧长度相同也不会共享完成状态。TCP/TLS CONNECTED 不表示 WS OPEN；WS OPEN
也不表示应用已经完成 WS 内认证。本接口未引入业务 READY、自动重连或消息重放。

客户端仍由一个调用线程推进。握手的外部 retained buffer 在写完成后转为 bridge
拥有的 frame 存储；client 保有底层分配直到 native terminal、engine 和 bridge
全部释放。H1 text/binary 使用一个在途 logical tag，消息复制上限为
`max_message_bytes`，每帧上限为 `max_frame_bytes`；native 完成后通过有界
`advance` 交付一次 terminal，才允许下一次发送。成功仅证明本地传输完成。
已接纳消息超时后仍需推进或 destroy，不自动重发。

背压时，未消费的 receive view 被复制到 client 自有的一个接收块，容量固定为
`network.receive_buffer_bytes`；恢复前暂停 receive demand，不保留 callback 裸指针。
engine input 和事件队列仍分别受配置上限约束，超限明确失败。destroy 先关闭并
drain CNet，再推进 logical terminal、销毁 bridge，最后释放 network 和存储。
任何未结算导致的 destroy 失败都保留 owner，不能提前释放 frame。

选择 SDK bridge 是为了复用实际 pending-write 和 native terminal 的事实源。
继续维护手写 H1 writer 会重复这些状态；直接把 bridge 绑定 H2 则违反其独占物理
连接约束。因此 H2 保留现有 RFC 8441 stream adapter，本次不改其完成关联协议。
没有新增库或改变公开结构布局；H1 新增有界自动分片，H2 仍遵循原有单帧发送限制。
回滚须同时回退本次 bridge 改动和两个 SDK，不能混用 Unicode 归属不同的 rc.1/rc.2。

当前能力与剩余工作：

| 边界 | 当前状态 |
| --- | --- |
| H1 client / WS / WSS | 已接入专用 bridge、tagged completion、背压存储和终态清理 |
| H1 server | 默认保留既有 writer；可在启动前启用专用 bridge，保留 `on_open` 首条操作及握手后交接，见[服务端策略](../http_server/README.md) |
| H2 client/server、WS pool | 已有 RFC 8441 与真实 stream 限制；本次仅回归，未接入专用 bridge |
| ManagedDial / Pool lease、业务认证 attempt ticket | 未与 WS 组合；需按 #231/#238 单独接入 |
| UDP mixed SG | rc.2 SDK 提供 opt-in；CHttp 尚无共享 SG 宿主或 HTTP-over-UDP 入口 |
| 联合资格 | 已有等长 101/首帧与专用 WS 1/2/4 Owner；仍需确定性 native queue-full/晚到 terminal、共享 SG、H2 HTTP/WS 兄弟流隔离及跨平台验收 |

Windows 验证通过正式 CTest 执行。`chttp_websocket_test` 新增 text/binary 分片、
重复接纳和关闭回归；既有用例覆盖认证、拒绝、subprotocol、双向控制帧、WS/WSS、
H2 stream 容量及多 session。`chttp_websocket_handshake_test` 覆盖握手解析。
Clang Release 全量 **94/94** 通过（`build/rc2-clang-full.log`），MSVC Release
全量 **90/90** 通过（`build/rc2-msvc-tests-full.log`），包括 IDL/Jinja
示例与 DSO 服务测试；这不替代 SaltsUtils 自身的全套测试。MSVC 使用
`ci-sdk-release-user`，configure 显式指定 `-DBUILD_TESTING=ON -DBUILD_TESTS=ON`，
确保测试与 IDL fixture 都在当前 build graph 中。

复现时先用 `cmake/ci/restore-native-sdks.ps1 -Rid windows-x64 -Local` 恢复包，
核对实际解析版本，再将 `SALTS_ROOT` / `SALTS_UTILS_ROOT` 指向对应 rc.2 SDK。
在 `VsDevCmd.bat -arch=x64 -host_arch=x64` 环境中运行：

```text
cmake --preset win-clang-release-user
cmake --build --preset win-clang-release-user --parallel 4
ctest --preset win-clang-release-user --output-on-failure
```

macOS preset 已随发布包切换到 AppleClang；本地未执行 macOS/Linux/移动端运行、
rc.2 Debug/ASan 或 TSan，不能沿用此前 rc.1 的 sanitizer 结果作为本轮证据。

## WS 的 Manager 接纳与退休

2026-10-10 的 rc.3 源码审查后，单 WS client（H1/H2）及 H2 WS pool 共用私有
[managed stream 适配器](src/chttp_managed_stream.h)。每个独占物理 client 初始化一个
单记录 Manager，通过 reserve/connect 接纳并保留 context hold。CNet 的真实终态先释放
物理额度；WS native bridge 的 tags/output 和 H2 session 清理完成后，宿主才释放 hold、
推进 recycle、销毁 Manager，最后销毁借用的 CNet client。同步 connect 拒绝立即退休，
不伪造 callback；握手失败和 destroy 使用 Manager 的保留关闭义务。停止或协议清理失败、
超时仍保留对象供原 Owner 继续清理；没有 quiescence 证明时不释放协议存储。
native bridge 保留原 state/receive observers，Manager 的
callback guard 和终态路径不会被绕开。

当前 WS ABI 是显式一次连接，H2 pool 只复用一个 transport/authority/TLS profile 的
物理会话，并通过真实 session slots 预留 stream。选择复用 Manager 的接纳/退休协议；
没有新增第二套 ClientPool 或 ManagedDial 状态，也没有自动重连、重放或跨 Owner 复用。
这些附加策略在共享连接宿主或授权恢复场景下才有价值；接入时须用完整身份 key、真实
stream 预留和旧 WS/lease 结算 gate，CONNECTED 不能代替协议 READY。共享 SG 还须
统一唯一 observe authority，不能把外部 progress 与独立 poll 混用。

改动只涉及不透明 impl 和私有适配器，公共函数、配置及会话句柄保持兼容，不需要调用方
迁移。回滚须同时恢复两个 WS 实现的直接 connect 与原销毁顺序，并删除该适配器，
不能保留已安装 Manager observer 而省略 recycle。[正式测试](tests/chttp_managed_stream_test.c)
连接真实 TCP，验证拒绝后的再次接纳、回调防重入、延迟退休、重复连接不丢失原身份及
邻居连接隔离；现有 WS/WSS H1/H2 集成覆盖 handshake、发送、拒绝与池容量。

该集成验证使用配套 Salts `2.3.0-rc.2` / SaltsUtils `4.3.0-rc.2` SDK。
ManagedDial 的队列满关闭重试修复另在 Salts rc.3 源码工作树验证，尚未进入这些预编译 SDK。
本阶段没有吞吐量或 CPU 改善的测量结论。

最终 Chttp 全量回归：Clang Release 99/99、MSVC Release 95/95、Debug/ASan
99/99；后者使用重新构建的 Salts rc.3 + SaltsUtils rc.2 Debug SDK，无 ASan
报告。依赖源码 commit、隔离构建步骤和结果日志见
[服务端的验证记录](../http_server/README.md#本次重构的职责和生命周期)。

## Owner-local Client Pool（#229）

H1 逻辑请求槽与物理 session 分别预分配，容量分别来自 `request_capacity`
和 `network.connection_capacity`。单一 client Owner 负责提交、协议回调、Pool
lease 和 Manager drain；不增加线程、等待队列、自动重试或请求重放。

请求槽拥有 generation、完成回调、parser、body/source/sink 状态；session 拥有
CNet 连接、Manager attachment、保活身份和传输回调。回调绑定 session，只有
当前独占操作可以访问请求槽。调用方结果只交付一次；发送或异步 sink 未结束时，
请求槽和 lease 仍须保留。取消和错误先禁止复用，真实传输 terminal 后才清理。

H1/H2 TCP/TLS 共用一个 `cnet_client_pool` 和一个 `cnet_manager`，物理接纳总量
由 `network.connection_capacity` 限制，lease 总量由 `request_capacity` 限制。
TCP/TLS 通过 `cnet_pool_reserve_connecting` 与 Manager 接纳连接；首次完整 H1
响应、全部发送和 sink 结束后才 `bind_ready`。后续 acquire 的协议回调实际保留
一个 H1 操作，容量不足立即返回 `SALTS_ENOBUFS`。Pool 不负责连接或等待。
`pipe://` 保持已有独立 IPC 路径，因为 Manager 不接纳该传输。

H2 在接受对端初始 SETTINGS（TLS 还须通过既有 h2 ALPN 校验）之后才发布 READY。
READY acquire 直接调用现有协议引擎申请真实 stream，遵守最新对端容量；GOAWAY
立即禁止新 stream，RST 仅结束相应请求。完成的 registry slot 保留至 lease release，
session 保留至所有 stream、文件操作和传输终态结束。首次连接在 SETTINGS 到达前
保留原有有界 stream 接纳，由协议表约束；这不构成 Pool READY，也不增加等待队列。

身份按完整 URI、authority、保活的不可变 TLS profile 和协议精确比较，然后映射为
client 内不回绕的数值 ID；Pool key 不存凭据或指针。TLS profile 覆盖 trust、SNI、
客户端凭据与 ALPN，销毁公开 wrapper 不使已保活的身份失效。不同 client 的 Pool
独立，不做跨 Owner、跨 authority 或跨 profile 合并。直接 URI 入口使用内部身份；
显式策略入口另纳入上文的 endpoint ID 和 snapshot generation。URI/profile 身份在
session 存活期内不可变。

关闭顺序为封闭接纳及 Pool、完成已有回调和文件操作、真实 CNet terminal、释放
协议 lease、Pool terminal、释放 Manager context hold、Manager recycle，最后销毁
Pool、Manager 和 CNet。任何失败都保留尚未解除的清理义务。

采用独立 session 是为了解除请求容量与保活连接的绑定；仅在旧请求槽上叠加 Pool
会继续混淆两种生命周期。H2 则保留已有独立 session，将协议 stream 的保留与释放
接到 Pool，避免把多路复用连接错误地变成独占 socket。回滚须整体撤回 session/Pool
改动；不提供双运行时开关。公开配置与默认单次请求语义保持不变；不同 origin 可以
使用独立的空闲物理容量，不再因为唯一逻辑请求槽被保活连接占用而提前驱逐。
`chttp_api_test` 覆盖单请求槽复用两个隔离 authority、两个 Owner 的同 origin 隔离、
取消 warm lease、未读完响应阻止复用以及截断 EOF 后恢复容量；
`chttp_h2_client_test` 覆盖 READY stream 取消、兄弟 stream 继续完成及后续复用。
`chttp_pool_lifecycle_test` 编译生产 Owner 单元，连接真实 CNet/Manager/Pool，
在测试 observer 中暂缓交付发送和终止通知，验证请求槽、lease、context 的释放顺序，
以及物理 generation 已复用后的旧通知隔离。它还覆盖容量为 1 时 H2 session 整体
失效后的回收与重新连接。另一个用例在空连接池上切换为测试驱动的真实 NativeIO
外部进度源，分开发送提交和完成事件观察：确认 NativeIO 请求在途且 retained payload
引用仍被持有，再取消请求，验证缓冲区释放返回 `SALTS_EBUSY`、lease 和请求槽继续
占用；真实完成事件处理后才交付取消结果、回收容量并允许后续请求成功。
写超时用例先观察首块 64 字节的真实 NativeIO 成功事件，再延迟交给 CNet，直到其
写期限到达：lease 仍被保留，迟到成功最终交付一次 `SALTS_ETIMEDOUT`，剩余请求体
不再读取，NativeIO 提交计数不增加。完成事件排空后恢复容量，后续请求成功。
已观察句柄的取消按该 SDK 契约返回 `SALTS_ENOENT`，不提前释放 CNet 持有的 payload。
这些用例不增加 CHttp 公开 external-owner API。既有 TLS、文件传输、GOAWAY、RST
和 shutdown 测试继续回归。

2026-10-10 本地工作树验证使用安装后的 Salts `2.3.0-rc.1`
（`58ff08fc95b4aa1dc493c0b7080426b2c11d4959`）和 SaltsUtils `4.3.0-rc.1`
（`049f8e39e1d19ff21e9825df7c497e40b75a8ddf`）：

| 入口 | 结果 |
| --- | --- |
| `cmake --build --preset win-clang-release-user`；`ctest --preset win-clang-release-user --output-on-failure` | Windows Clang Release 全量 91/91，23.38 秒 |
| `ci-sdk-release-user`，按现有 CI 启用 `BUILD_TESTING`、`BUILD_TESTS` 后完整 build/CTest | Windows MSVC Release 全量 87/87，17.96 秒；该 preset 关闭示例构建 |
| `cmake --build --preset install-ci-sdk-release-user` | 安装到工作树 `stage/sdk/windows-x64` |
| `win-dev-user`、`ci-sdk-release-user`、`win-clang-release-user` 的 `-R '^chttp_pool_lifecycle_test$' --repeat until-fail:3` | 包含真实 NativeIO 在途取消、写超时迟到成功的 6 个生命周期用例，各配置连续通过 3 次 |
| `cmake --build --preset win-dev-user`；`ctest --preset win-dev-user --output-on-failure` | Windows MSVC 19.44.35217 Debug/ASan 全量 91/91，41.57 秒，无 ASan 报告 |
| 对应 Salts checkout 的 `win-dev-user`，CTest 匹配 `^(cnet_owner_test\|cnet_owner_profile_test\|cnet_write_queue_test\|cnet_client_pool_test)$` | Debug/ASan 4/4；其中 profile suite 的 17 个用例全部通过，含 retained vector 短写重提交及短写后超时 |

上表全量结果包含 6 个生命周期用例和 12 个远端策略用例，以及 C11/C++ 公开头编译。

ASan 使用上述两个源码提交重新构建并安装的 Debug SDK，不混用 Release SDK 或本机
旧版 Debug SDK。在各自独立 checkout 中，以 `win-dev-user` configure/build，关闭
`BUILD_TESTS`、`BUILD_EXAMPLES`、`BUILD_BENCHMARKS`；Salts 另关闭 `ENABLE_TESTS`，
SaltsUtils 关闭本任务不使用的 `SALTS_UTILS_ENABLE_CAPTURE`，再用各自
`install-win-dev-user` 安装。依赖源码、产物和安装根都位于本工作树的
`build/asan-qualification`；源码未修改，也不把这份本地 Debug 构建称为已发布候选包。
CHttp 的 `win-dev-user` 沿用既有 ASan 配置，父进程提供以下路径，然后在
`VsDevCmd.bat -arch=x64 -host_arch=x64` 环境内执行上表 build/CTest：

```powershell
$env:PROJECT_ROOT = "$PWD/build/asan-qualification"
$env:SALTS_ROOT = "$env:PROJECT_ROOT/external/pkgs/salts/debug"
$env:SALTS_UTILS_ROOT = "$env:PROJECT_ROOT/external/pkgs/salts-utils/debug"
cmake --preset win-dev-user
```

随后在同一 Salts checkout 启用 `BUILD_TESTS`、`ENABLE_TESTS`，保留示例和 benchmark
关闭，通过本地 build target 构建上表四个正式测试，再以 CTest 执行；不修改或重装
依赖源码。`cnet_owner_profile_test` 使用项目既有的内部测试库，将单次 NativeIO 发送
限制为 1/2 字节，核对真实字节内容、剩余字节重提交计数、超时后不重提交和引用释放。

上述证据分别覆盖 CNet 的原生短写和 CHttp 的部分请求体、在途取消与迟到成功。
安装后的 CNet SDK 不导出内部短写注入入口，尚未在同一个 CHttp 用例中强制原生短写；
不能把分层验证写成该端到端用例已经完成。跨平台门禁、安装后独立 C11/C++17 消费测试、
TSan/其他平台 sanitizer 及原始性能数据也未完成；本地 Windows ASan 和安装成功
不能作为 #229 已全部交付的依据。
