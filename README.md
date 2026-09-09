# HTTPServices

基于 Salts 的 C11/C++17 HTTP 服务库，包含 CHTTP、S3 和 CRPC。

| 模块 | CMake target | 职责 |
| --- | --- | --- |
| [CHTTP](chttp/README.md) | `Salts::CHTTP` | HTTP/1.1、HTTP/2、TLS policy、WebSocket admission、JWT、文件传输与连接池 |
| [S3](s3/README.md) | `Salts::S3` | SigV4、对象/桶操作、multipart 与 S3 配置 |
| [CRPC](crpc/README.md) | `Salts::CRPC` | 基于 CHTTP 的 JSON-RPC client/server |

依赖方向为 `S3 / CRPC → CHTTP → Salts`。Salts 拥有 CNet、CFlow、NativeIO、
解析器与通用基础设施；HTTPServices 使用安装后的公开目标，不读取 Salts 源码树。
`vendor/cjwt`、`vendor/turbo_crypto` 为本仓库私有实现，不作为公共 API 或独立 SDK 导出。

## 构建、测试与安装

要求 CMake 3.25+、Ninja、C11/C++17 编译器、vcpkg，以及完成拆分的 Salts SDK。
设置 `PROJECT_ROOT` 和 `VCPKG_ROOT`；版本化的 `CMakeUserPresets.json` 从
`PROJECT_ROOT/external/pkgs` 推导 SDK 路径。Windows 命令在匹配架构的
`VsDevCmd.bat -arch=x64 -host_arch=x64` 开发环境运行：

```powershell
cmake --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user
cmake --build --preset win-release-user --target verify_installed_package
cmake --build --preset install-win-release-user
```

Linux 使用 `linux-release-user` 与 `install-linux-release-user`；Debug/ASan 使用
`win-dev-user` 或 `linux-dev-user`，并先安装匹配的 Salts Debug SDK。
Release 安装到 `$PKG_ROOT/http-services/release`，Debug 安装到对应 `debug` 目录。
`SALTS_ROOT` 在每个 profile 中明确指定；缺失、无效或仍含 HTTP 模块的旧 SDK 会在 configure 失败。

## 消费与迁移

消费工程为所选配置设置 `SALTS_ROOT` 和 `HTTP_SERVICES_ROOT`，两者分别指向对应 SDK 安装根。
在已有应用中增加包查找，原 target 和 include 路径保持：

```cmake
if(NOT DEFINED ENV{HTTP_SERVICES_ROOT}
   OR NOT IS_DIRECTORY "$ENV{HTTP_SERVICES_ROOT}")
  message(FATAL_ERROR "HTTP_SERVICES_ROOT must name the HTTPServices SDK")
endif()
file(TO_CMAKE_PATH "$ENV{HTTP_SERVICES_ROOT}" http_services_sdk_root)
find_package(HTTPServices CONFIG REQUIRED COMPONENTS CHTTP S3 CRPC
  PATHS "${http_services_sdk_root}" NO_DEFAULT_PATH)
target_link_libraries(my_app PRIVATE Salts::S3 Salts::CRPC)
```

HTTPServices 自动加载 Salts。运行时需要两个 SDK 的运行库；Windows 将二者 `bin` 放入 PATH，
Linux 使用相应安装路径的运行库搜索配置。CHTTP ABI 2、S3 ABI 1 和 CRPC archive ABI 2
及现有文件名保持不变。仅查找 Salts 的旧消费配置需要增加 HTTPServices 包。

安装验证从构建树的独立 staging prefix 编译并运行原 C/C++ 消费测试，同时禁用 llhttp、
OpenSSL、c-ares 的消费端查找，验证第三方实现依赖不泄漏。正常构建不要求访问在线 S3 服务。

## 来源与架构决策

源码来源为 Salts commit `65a16c66ddee3495d08fbea10b0901e7f9bf3720`，迁移目录：
`chttp/`、`s3/`、`crpc/`、`vendor/cjwt/`、`vendor/turbo_crypto/`。
CMake helpers 和安装消费测试亦来自该 revision。原模块算法、公开头文件与测试语义保持；
修改集中在包查找、导出、preset、安装验证和文档。

只迁移 CHTTP/S3 会让保留的 CRPC 形成 Salts → HTTPServices → Salts 循环；
另拆 CRPC transport 会扩大 API 迁移成本。因此选择三个模块整体迁移，保留运行时架构。
状态 owner、请求生命周期和错误传播没有迁移；没有业务数据或格式迁移。
回滚采用上述 Salts revision 的完整旧包及旧消费配置，避免同时部署两套实现。
已有模块 provenance、上游许可证和本地修改说明随源码保留；没有另行赋予新的许可证。

专项历史设计保留在 Salts 的 `docs/superpowers/`；当前 S3/HTTP 设计见本仓库 `docs/`。
