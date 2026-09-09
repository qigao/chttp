# CHTTP H1/H2 流式正文与文件传输设计

## 背景与目标

当前 CHTTP 的 H1/H2 客户端和服务端都把正文完整复制到有界内存后再交付。该模型适合小型 JSON/RPC，但文件上传、下载会让常驻内存随文件大小增长，并且无法在协议窗口或 socket readiness 允许时逐块推进。

本设计为 HTTP/1.1 与 HTTP/2 提供同一组公开正文 source/sink 接口，并在其上提供同步 requests-style 文件上传、原子下载和服务端文件响应。既有 `body/body_size`、普通 route、middleware 和 Session 行为保持不变。

不在本阶段实现 HTTP/3、multipart 表单解析、断点续传、Range、多文件事务或服务端协程式阻塞 reader/writer。

## 候选方案

### 方案 A：继续完整缓冲

优点是接口不变、实现最简单。缺点是内存复杂度为 O(body size)，无法承载受限内存下的大文件，也不能利用 H2 流量控制形成自然背压。

### 方案 B：按协议分别公开 H1/H2 API

可以直接暴露 chunked 与 DATA frame，但会让应用承担协议差异、H2 窗口和 H1 framing，破坏统一 API，也会把第三方协议引擎类型扩散到公开 ABI。

### 方案 C：统一 source/sink，协议适配器内部实现（采用）

公开 API 只描述“读取下一块”和“消费下一块”。H1 适配器选择 Content-Length 或 chunked framing，H2 适配器选择 Content-Length 或无长度 DATA 流。文件便利层复用 CFlow 的共享异步文件 runtime；路径查询、打开、关闭与原子 rename 仍通过 `salts_fs` 完成，不向下游暴露 llhttp、H2 引擎、CFlow runtime 或平台文件句柄。

## 公开接口

```c
typedef int (*chttp_body_read_fn)(void *user, void *buffer,
                                  size_t capacity, size_t *out_size);
typedef int (*chttp_body_write_fn)(void *user, const void *data, size_t size);
typedef void (*chttp_progress_fn)(void *user, size_t transferred, size_t total);

typedef struct chttp_body_source {
  chttp_body_read_fn read;
  void *user;
  size_t content_length;
  int content_length_known;
} chttp_body_source;

typedef struct chttp_body_sink {
  chttp_body_write_fn write;
  void *user;
} chttp_body_sink;
```

`chttp_request_options` 和 `chttp_options` 追加 `body_source` 与 `body_sink`。`body/body_size` 与 `body_source` 互斥；source 和 sink 的函数指针及 `user` 在请求终结前为 borrowed。异步 callback 返回后，response view 仍失效；sink 模式下 `response.body == NULL`，`response.body_size` 是已接受的正文总字节数。

同步文件便利接口为：

```c
int chttp_post_file(chttp_client *, const chttp_options *, const char *path,
                    chttp_progress_fn, void *, chttp_response *, chttp_error *);
int chttp_put_file(chttp_client *, const chttp_options *, const char *path,
                   chttp_progress_fn, void *, chttp_response *, chttp_error *);
int chttp_download_file(chttp_client *, const chttp_options *, const char *output_path,
                        chttp_progress_fn, void *, chttp_response *, chttp_error *);
```

服务端 route 可选择一个 request-body sink 工厂；普通 handler、middleware 与 Session 仍在完整 request 到达后执行。sink route 的最终 request view 使用 `body == NULL`、`body_size == 已接收总量`、`body_streamed == 1`。服务端响应 builder 接受 source，并提供文件 convenience：

```c
typedef int (*chttp_server_body_open_fn)(
    void *user, const chttp_server_request_view *request, chttp_body_sink *out_sink);
typedef void (*chttp_server_body_close_fn)(void *user, chttp_body_sink *sink, int status);

int chttp_server_response_source(chttp_server_response *, unsigned int,
                                 const char *content_type,
                                 const chttp_body_source *);
int chttp_server_response_file(chttp_server_response *, unsigned int,
                               const char *content_type, const char *path);
```

`chttp_server_route_options` 追加 `body_open/body_close`。二者均为空表示原有完整缓冲 route；二者必须成对出现。

## 数据与生命周期协议

### 数据单元和事实源

- source 的事实源是调用方拥有的 reader 状态；CHTTP 只复制回调描述符，不复制该状态。
- sink 的事实源是调用方拥有的 writer 状态；只有 sink 返回 `SALTS_OK` 后，CHTTP 才计入已消费字节并恢复 H2 flow-control credit。
- 文件上传的事实源是打开后 stat 得到的固定长度文件；读取不足或超出声明长度均为协议错误。
- 文件下载的事实源是同目录唯一临时文件；仅在 2xx、正文完整、`fsync` 和 close 成功后通过 rename 一次性成为目标文件。
- 每个 async client 或 server owner 懒创建一个共享 CFlow file runtime；runtime 是 native backend、Actor、Executor、operation slot 与 completion lane 的唯一事实源。单个 transfer 只拥有文件句柄、一个有界 chunk lease 和传输状态。

### 所有权与失效点

- 每次 source 回调接收 CHTTP 拥有的可写临时 buffer，只在回调期间有效。
- 每次 sink 回调接收 parser/H2 input 的 borrowed view，只在回调期间有效；不得跨 callback、poll、协程挂起、append 或 slot 复用保存。
- 异步 source/sink 描述符和 `user` 必须活到 terminal callback 返回。
- 服务端 response source 必须能在 handler 返回后继续读取，因此 builder 复制描述符；其 `user` 必须活到该响应发送完成或连接终止。
- 服务端文件 response 由 CHTTP 打开和关闭文件，不向应用暴露文件句柄。

### 线程拓扑与可重入性

- 异步客户端 source/sink 在 `chttp_async_client_poll()` 的 owner thread 上调用。
- 服务端 source/sink 在 server owner thread 上调用。
- NativeIO worker 只发布 terminal completion 并唤醒 CHTTP owner；协议状态、进度回调、临时文件提交与用户 callback 只由 owner 推进。
- 回调不得执行无界阻塞工作、递归进入同一 client/server、提交新请求或调用 stop/destroy。
- requests-style 文件 API 对调用方保持阻塞，因为调用线程显式拥有整个同步调用；正文 read/write 本身通过 CFlow/NativeIO 异步提交，等待期间同一 owner 继续推进 CNet 与文件 completion。
- server file helper 的 handler 只完成 stat、open 与响应配置。正文读取在 handler 返回后异步提交，read completion 通过 CNet wake 恢复匹配的 H1 connection 或 H2 stream，不在 server owner 上执行阻塞 read。

### 容量、背压与复杂度

- `stream_chunk_bytes` 是 client/server 配置中的可调硬上限；零选择不超过 transport
  单次发送能力的 64 KiB 默认值。
- 每个活跃 source 最多拥有一个 chunk buffer；额外内存为 O(active streams × stream chunk bytes)。
- `max_request_body_bytes` 和 `max_response_body_bytes` 仍限制传输总量，不能借 streaming 绕过。
- server 的 `max_buffered_response_body_bytes` 单独限制 memory reply copy；零从总量上限与
  单次 transport send 预算推导，因此大文件上限不再迫使每条连接预分配同等 body buffer。
- H1 同一连接只有一个 send in flight；`on_send` 完成后才读取下一块。
- H2 上传仅在 stream/connection window 和有界 output buffer 可接受时读取下一块；下载在文件写完成前保留 DATA credit，并暂停后续 inbound frame dispatch。每个 transfer 始终只有一个 write lease，避免同一文件的乱序完成；代价是该短暂写等待窗口内同连接稍后的 sibling frame 也会留在 H2 的有界 input buffer。
- source/sink 返回失败立即终结：H1 关闭独占连接，H2 只 RST 当前 stream，保留 sibling streams。

### 长度与 framing

- 已知长度 source：H1/H2 均发送 Content-Length，并要求 EOF 与声明长度精确一致。
- 未知长度 source：H1 使用 Transfer-Encoding: chunked；H2 不发送 Content-Length，以 END_STREAM 结束。
- 用户不得同时提供 Content-Length/Transfer-Encoding 与 source；由 CHTTP 维护唯一 framing 事实源。
- HEAD response 不把正文交给 sink，也不发送 response source/file bytes。

## 错误语义

- 参数冲突、缺失回调、零 chunk 配置或声明长度超限在 admission/init 阶段 fail fast。
- source 失败使用其负错误码；长度不一致返回 `SALTS_EPROTO`，stage 为 `request-source` 或 `request-source-length`。
- sink 失败使用其负错误码，stage 为 `response-sink`。
- 文件 open/stat/read/write/fsync/rename 失败保留 native Salts 状态，stage 分别为 `file-open`、`file-stat`、`file-read`、`file-write`、`file-sync`、`file-commit`。
- Windows IOCP 没有异步 flush；下载正文仍为异步 write，完成并关闭句柄后仅用同步 `salts_fs_fsync` 作为原子 rename 前的 durability barrier。Linux io_uring 使用原生异步 flush。其他不支持异步 regular-file read/write 的 backend 在打开前 fail fast，不降级成阻塞数据路径。
- 非 2xx 下载不是 transport 失败：返回 `SALTS_OK` 和 owning HTTP response，删除临时文件且保持原目标不变。
- server request sink 失败时，H1 关闭该连接；H2 RST 该 stream。`body_close` 恰好调用一次并接收终态。

## 架构与现有行为影响

- 公开 facade 保持单套 H1/H2 API；内部以 strategy adapter 将 source/sink 映射到 H1 parser/framing 或 H2 DATA/flow control。
- CHTTP client/server 分别持有一个按 request/stream capacity 定容的共享文件 runtime；file transfer registry、runtime operation slots 与 retained chunk 总量都有硬上限。文件完成只通过 readiness wake 通知原 owner，不建立第二套 HTTP 状态机。
- 既有 memory-body 路径不经 source/sink，响应仍由 owning copy 返回，因而现有调用者无行为变化。
- 普通 server route 仍完整缓冲；只有显式配置 `body_open/body_close` 的 route 使用流式上传。
- middleware 和 Session 保持原来的最终 dispatch 顺序。由于正文已经流入 sink，鉴权若必须在接收正文前完成，应放在 `body_open` 中；普通 middleware 不承担前置 spool admission。
- CMeta 只适合记录 source/sink strategy 的类型与 callable 契约，不进入逐 chunk 状态机；热路径保留直接函数指针，避免 metadata lookup 与额外间接层。

## 迁移、兼容性与回滚

字段仅追加到 ABI major 2 的结构尾部，零初始化调用者继续走完整缓冲。安装导出只新增 CHTTP 自有类型，llhttp 和 H2 私有类型保持 PRIVATE。

若 streaming 适配器出现回归，可删除新增 route/file/source API 并保留原 memory-body 路径；数据格式、Session cookie 和 wire protocol 无需迁移。下载失败不会覆盖已有文件，因此回滚不需要数据修复。

## 验证范围

- H1 已知/未知长度上传、chunk 边界、提前 EOF、source 失败、sink 失败、HEAD 和 connection reuse/close。
- H2 已知/未知长度上传、窗口背压、sink credit、单 stream 失败与 sibling 隔离。
- CFlow shared runtime 多文件共用、容量拒绝、关闭取消、回调线程归属、reentrant/concurrent drive 拒绝与 destroy-before-quiescence。
- 同一公开 API 分别指定 H1/H2 的文件上传下载；下载成功原子替换、非 2xx/中断/写失败保持原文件。
- H1/H2 server 流式 route、普通 route/middleware/Session 回归、文件 response、HEAD 抑制正文。
- C/C++ header、installed consumer、Release focused tests、ASan focused tests 和完整 CTest。
