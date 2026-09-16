# Chttp

基于 Salts 的 C11/C++17 HTTP、JSON-RPC 与 S3 库。

| 模块 | CMake target | 公开入口 |
| --- | --- | --- |
| HTTP/RPC 客户端 | `CHttp::Client` | `<http_client/http.h>`、`<http_client/rpc.h>` |
| HTTP/RPC 服务端 | `CHttp::Server` | `<http_server/http.h>`、`<http_server/rpc.h>` |
| 独立 S3 客户端 | `CHttp::S3` | `<s3/s3.h>` |

`Client` 和 `Server` 分别包含本侧 RPC 实现，两端独立链接。S3 位于独立的 `s3/` 模块，
链接 `CHttp::Client`。底层连接、执行器等能力依赖安装后的 Salts SDK；JSON parser 和
Ed448 能力来自 SaltsUtils。通用密码学能力由 vcpkg 的 BoringSSL 提供。

## 目录归属

```text
http_client/  include/http_client/、src/、rpc/、tests/
http_server/  include/http_server/、src/、rpc/、tests/、examples/
http_common/  include/http_common/、http/、rpc/、tests/
s3/           include/s3/、src/、tests/
vendor/       cjwt/
```

每个模块的 `include/` 管理公开声明，`src/` 或协议子目录管理实现，`tests/` 管理模块测试。
`http_common/` 仅存放 HTTP/RPC 共享协议类型和机制，不持有客户端或服务端的业务状态。
其对象代码编译一次，分别链接进两端库，不导出独立 CMake target。
共享 fixture 和两端集成测试放在 `http_common/tests/`。后续模块按能力增加自己的目录。
第三方库及其测试、benchmark target 均归入对应 `vendor/` 分组。

Server 支持 Cookie Session、全局/路由级 middleware；RPC 可通过 `crpc_server_http()`
取得 HTTP owner，在启动前配置中间件。详见 [服务端](http_server/README.md)、
[客户端](http_client/README.md)、[S3](s3/README.md)。

可选 [OpenAPI 生成器](openapi/README.md) 从 C 源码注释生成 OpenAPI 3.1 文档，
支持标准 schema 约束；使用 `BUILD_OPENAPI=ON` 启用。

## 构建与使用

要求 CMake 3.25+、Ninja、vcpkg、C11/C++17 编译器，以及匹配配置的 Salts 和
SaltsUtils SDK。构建 Chttp 前设置 `SALTS_ROOT`、`SALTS_UTILS_ROOT`、`PROJECT_ROOT`
和 `VCPKG_ROOT`；路径使用 `/`。Windows 在 MSVC 开发环境中运行：

```powershell
cmake --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user
cmake --build --preset install-win-release-user
```

`CMakePresets.json` 与 `presets/` 完整复制 Salts。Windows 默认入口
`win-dev-user` / `win-release-user` 使用 MSVC，C/C++ 编译器为 `cl`。
`CMakeUserPresets.json` 定义本仓库安装路径和基础依赖根；`SALTS_UTILS_ROOT` 可由调用环境
显式提供。CMake 对 Salts 和 SaltsUtils 都使用 fail-fast 的配置包查找，不回退到其他 profile。

消费工程设置 `SALTS_ROOT`、`HTTP_SERVICES_ROOT` 后按需链接：

```cmake
find_package(Chttp CONFIG REQUIRED
  PATHS "$ENV{HTTP_SERVICES_ROOT}" NO_DEFAULT_PATH)
target_link_libraries(my_app PRIVATE CHttp::Client)
```

服务端选择 `CHttp::Server`，S3 应用选择 `CHttp::S3`。Windows 运行环境需要已安装的运行库
及 vcpkg 运行库路径。头文件路径和库名已变更，调用方需要更新 include、链接配置并重新编译。

## 来源

源码由 Salts commit `65a16c66ddee3495d08fbea10b0901e7f9bf3720` 迁移。
原有请求生命周期、状态归属、协议和错误语义保留。上游来源、许可证及本地修改说明随模块保留。
运行时说明见 [HTTP](docs/HTTP.md)、[RPC](docs/RPC.md)，结构决策见
[模块组织](docs/plans/2026-09-09-server-client-layout.md)。
