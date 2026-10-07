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
