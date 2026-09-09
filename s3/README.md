# Salts S3

`Salts::S3` 是建立在 CHTTP 之上的有界 S3 客户端协议层。它提供同一套 HTTP/1.1、HTTP/2
API，包括 SigV4、path/virtual-hosted addressing、bucket/object CRUD、分页 list、copy、
presigned URL、SSE、文件上传下载、可恢复 multipart，以及 lifecycle、notification、
replication 子资源。HTTP/3 不在模块范围内，也不会从 H2 静默回退到 H1。

## 依赖与所有权

S3 client 借用调用方创建的 CHTTP client；S3 不初始化、不轮询、不停止、也不销毁 CHTTP。
普通 `s3_client` 使用同步 request/reply，用户无需调用 poller；高级 `s3_async_client` 复用
CHTTP async client，适合 Executor、Actor 或批量事件循环。

```text
application -> Salts::S3 -> Salts::CHTTP -> Salts::CNet -> NativeIO
```

`connection_uri` 选择连接目标，`authority` 同时用于 HTTP authority 与 SigV4 host，`protocol`
显式选择 H1 或 H2。CHTTP 负责 TLS、连接池、HTTP framing、流控和文件 source/sink；S3 负责
对象存储语义。virtual-hosted addressing 会把 bucket 加到 authority，TLS 证书必须覆盖最终
hostname。

## 同步请求示例

```c
#include <s3/s3.h>
#include <s3/s3_credentials.h>
#include <s3/s3_object.h>

s3_static_credentials keys = {"access-key", "secret-key", NULL};
chttp_client http = {0};
s3_client s3 = {0};
s3_response response = {0};
s3_error error = {0};

chttp_client_config http_config = {/* bounded CHTTP capacities */};
s3_client_config s3_config = {
    .size = sizeof(s3_config),
    .connection_uri = "tcp://127.0.0.1:9000",
    .authority = "127.0.0.1:9000",
    .region = "us-east-1",
    .addressing_style = S3_ADDRESSING_PATH,
    .protocol = CHTTP_HTTP_1_1,
    .credentials = s3_credentials_provider_static(&keys),
    .timeout_ms = 30000u,
};

if (chttp_client_init(&http, &http_config) == SALTS_OK &&
    s3_client_init(&s3, &http, &s3_config) == SALTS_OK) {
  int status = s3_put_object(&s3, "bucket", "key.txt", "hello", 5u,
                             "text/plain", &response, &error);
  /* status == SALTS_EPROTO preserves a non-2xx HTTP response and parsed S3 error. */
  (void)status;
  s3_response_destroy(&response);
  (void)s3_client_destroy(&s3);
  (void)chttp_client_destroy(&http, 30000u);
}
```

生产 HTTPS 将 `connection_uri` 改为 `tls://...:443`，并在 `s3_client_config.tls` 中传入通过
`chttp_tls_profile_init()` 创建、且 ALPN 与所选 H1/H2 一致的 verified TLS profile；S3 不提供
关闭证书或 hostname 校验的开关。

`s3_response` 是 owning result，必须用 `s3_response_destroy()` 释放。非 2xx 响应返回
`SALTS_EPROTO`，同时保留 HTTP status/body，并在边界内解析 `Code`、`Message`、`RequestId`
和 `HostId`。async callback 收到的 `s3_response_view` 仅在 callback 返回前有效。

blocking client 是 single-owner。文件传输和 multipart progress callback 在 owner thread 上
执行，不得重入或销毁同一个 S3/CHTTP client。

## 文件与 multipart

`s3_put_object_file()` 先以有界 buffer 计算 SHA-256，再由 CHTTP 的异步文件 source 完成第二遍
读取；调用方必须在两遍期间保持源文件不变。`s3_get_object_file()` 写入同目录临时文件，成功
后 fsync 并原子替换目标，HTTP 或 I/O 失败不会破坏原文件。

超过单请求策略限制的文件可用 `s3_put_object_multipart_file()`。它顺序复用固定大小 part
buffer，并通过同目录临时文件、fsync、原子 rename 保存版本化 checkpoint。checkpoint 绑定
endpoint、authority、region、addressing style、bucket、key、源路径、size、mtime、ctime、
part size、SSE-C key MD5 fingerprint 与 upload id；任一不匹配都会在网络请求前失败。恢复
SSE-C upload 时必须重新提供相同的 `put_options`，checkpoint 不保存 raw/base64 key。成功 part
的 ETag 持久化后才允许
跳过，成功 complete 后才删除 checkpoint。
如果 S3 已接受 complete、但最后删除 checkpoint 失败，函数以
`multipart-checkpoint-remove` 阶段返回 `SALTS_EIO`；对象已经提交，调用方应删除过期
checkpoint，不能用它继续 resume。

手动 multipart API 要求 part number 为 1..10000，除最后一块外每块至少 5 MiB，单块最多
5 GiB。高层文件 API 的 part buffer 还受 `max_multipart_part_bytes` 限制，默认 64 MiB。
SSE-C handle 会持有后续 `UploadPart` 所需的派生 header，destroy 时清零。即使 HTTP status 是
200，CompleteMultipartUpload body 为空、格式错误或根节点为 `<Error>` 时仍返回
`SALTS_EPROTO` 并保持 ACTIVE 状态。
当前 multipart handle 与 blocking client 均为 single-owner，不能跨线程并发上传 parts。

## 头文件与构建

公开能力按职责拆分：

- `<s3/s3.h>`：client、generic request、response/error、async
- `<s3/s3_credentials.h>`、`<s3/s3_signer.h>`：credential provider 与 SigV4
- `<s3/s3_bucket.h>`、`<s3/s3_object.h>`：bucket/object 操作与文件传输
- `<s3/s3_multipart.h>`：manual/high-level multipart
- `<s3/s3_bucket_config.h>`：lifecycle、notification、replication XML 子资源

安装后使用：

```cmake
find_package(Salts CONFIG REQUIRED)
target_link_libraries(app PRIVATE Salts::S3)
```

BoringSSL、XML parser 和 llhttp 等实现依赖不会以第三方类型出现在 S3 公开 API 中。

实现边界、容量、错误与 rollback 设计见
[`docs/S3_CHTTP_DESIGN.md`](../docs/S3_CHTTP_DESIGN.md)，迁移来源见
[`PROVENANCE.md`](PROVENANCE.md)。
