# HTTP 与 RPC 客户端

`CHttp::Client` 包含 HTTP 请求、连接池、H1/H2、TLS、WebSocket client/pool、
文件上传下载和同步/异步 JSON-RPC 调用。S3 在独立的 `../s3/` 模块中，依赖此库。

公开入口为 `<http_client/http.h>`、`<http_client/rpc.h>`，位于本模块的 `include/`。
`src/` 管理 HTTP 实现，`rpc/` 管理 RPC 调用实现，`tests/` 管理客户端测试。
共享协议与 codec 位于 `../http_common/`；两端集成测试位于 `../http_common/tests/`。

客户端不链接服务端库。请求 owner、取消、完成回调和关闭协议保持原有语义。
详细接口见 [HTTP](../docs/HTTP.md)、[RPC](../docs/RPC.md)。
