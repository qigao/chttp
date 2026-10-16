# Praktor 构建入口

本工程沿用 TurboAgent 的 Praktor → CMake preset 编排方式。Praktor 负责读取
`.env`、传递 SDK 根目录和按顺序执行命令；编译器、功能开关、triplet、安装路径
仍由版本化 presets 管理。直接使用 CMake 和现有 GitHub Actions 的方式继续有效。

## 环境准备

安装可运行的 Praktor CLI，并将 CMake、Ninja、Git 放在 `PATH`。
Praktor 自身依赖的 DLL/共享库必须在启动前可用，不向本工程复制运行库。
将 [`.env.example`](../.env.example) 的非密钥设置合并到已有 `.env`，或由父进程提供。
不要覆盖已有凭据。`.env` 已忽略，示例不含 token。

Praktor 的 dotenv 值是字面量，不展开 `$VAR`、`${VAR}`、`~` 或 CMake 宏。
使用绝对路径和正斜杠；`.env` 覆盖父进程同名变量。CMake 不自动读取 `.env`，
直接调用 preset 时须先在父环境设置这些变量。

- `PROJECT_ROOT`：保留现有 `PKG_ROOT=PROJECT_ROOT/external/pkgs` 规则。
- `SALTS_ROOT` / `SALTS_UTILS_ROOT`：所选 profile 对应的安装 SDK，Debug/Release 不混用。
- `VCPKG_ROOT` / `VCPKG_CACHE_REPOSITORY_ROOT`：vcpkg 与共享缓存工具链 checkout。
  Windows/Linux 本地 preset 现在都读取显式的缓存 checkout 环境变量。
  与 TurboAgent 共用时，将两个工程的 `VCPKG_CACHE_REPOSITORY_ROOT` 设为同一个绝对目录，
  toolchain 与本地/CI overlay ports 即来自同一 checkout（包括其未提交修改），
  无需另外设置 `VCPKG_OVERLAY_PORTS`。
  切换目录后先执行 `TASK=refresh`；各工程仍保留自己的 `vcpkg_installed`。
  这不会固定 GitHub Actions 的 `@master`，也不保证不同编译器/triplet 共用同一二进制包。
- `VSDEVCMD_BAT`：Windows Visual Studio 开发环境入口。每个 CMake/CTest 命令
  在同一 `cmd.exe` 中先调用它，并保留配置的 `VCPKG_ROOT`。
  从其 `VSINSTALLDIR` 设置
  [`VCPKG_VISUAL_STUDIO_PATH`](https://learn.microsoft.com/en-us/vcpkg/users/config-environment#vcpkg_visual_studio_path)，
  让 vcpkg 使用同一个 VS 实例；triplet 的显式 toolset 约束仍然有效。
- `GITHUB_TOKEN`：共享 NuGet feed 的 `read:packages` 凭据，仅放在 `.env` 或父环境。
- `RE2C_ROOT`：`qigao/vcpkg-cache` 发布的 `Qigao.Re2c.Binary` host 包目录，
  本地和 CI 都从父环境读取，不再由 Windows preset 改写。

Linux/macOS 使用自己的本机路径。Unix 共享缓存按其安装要求提供 NuGet runtime。
SDK restore 需要 .NET SDK 8 与 Python 3（Unix 的 `python3`，Windows 的 `python`）。
Windows 还需设置 `BASH_EXECUTABLE` 为 Git for Windows 的 `bin/bash.exe`，避免选中 WSL。
GitHub Actions 的 [`shell: bash`](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#jobsjob_idstepsshell)
也使用 Git for Windows。

### 共享 re2c 包

`Qigao.Re2c.Binary` 包含 host 可执行文件及 `share/re2c/stdlib`，独立于 vcpkg
库的二进制缓存和 `PKG_ROOT` 下的第一方 SDK。两个工程可以直接共用同一份 NuGet
payload。在两个源码目录之外创建可复用的 `re2c.csproj`：

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <RestorePackagesWithLockFile>false</RestorePackagesWithLockFile>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Qigao.Re2c.Binary" Version="*" />
  </ItemGroup>
</Project>
```

在当前工程根目录执行下面的 restore；两个绝对路径换成本机路径，父进程环境须有
.NET SDK 8 和 `GITHUB_TOKEN`（此命令不会读取 `.env`）：

```sh
dotnet restore /absolute/re2c-restore/re2c.csproj --packages /absolute/shared/nuget-packages --configfile cmake/vcpkg-cache.nuget.config --no-cache --force-evaluate
```

从该 restore 工程的 `obj/project.assets.json` 读取实际解析的
`Qigao.Re2c.Binary/<version>`。两个工程的 `.env` 使用相同的字面量绝对路径：
`RE2C_ROOT=<packages>/qigao.re2c.binary/<version>/tools/<host-rid>`。
`.env.example` 的版本仅为示例；Android/iOS 交叉编译也必须选择 host RID。
切换后执行 `praktor -f build.yml -i TASK=refresh`，清除旧工具路径的 CMake cache。
普通构建复用已恢复的包，更新版本是显式环境准备操作。

Native 和 cross CI 均使用
`qigao/vcpkg-cache/.github/actions/setup-re2c-tools@master`，由共享 action 安装 .NET、
恢复最新稳定包并设置 `RE2C_ROOT`/`PATH`。

## 使用已安装 SDK

```sh
praktor -f build.yml
praktor -f build.yml -i TASK=configure
praktor -f build.yml -i TASK=build
praktor -f build.yml -i TASK=test
praktor -f build.yml -i INSTALL=1
praktor -f build.yml -i HOST=unix -i PRESET=linux-release-user
```

默认 `HOST=windows`、`PRESET=win-release-user`、`TASK=all`、`TEST=1`、`INSTALL=0`。
`all` 依次配置、构建完整 graph、运行对应 CTest。`build` 先配置，`test` 先配置并构建，
`install` 构建并按 `TEST` 选择测试后安装。任何失败都会停止后续操作。

`TASK=refresh` 仅执行 `cmake --fresh --preset`；`FRESH=1` 为其他任务显式请求相同的
cache 恢复。普通构建不清理 cache。安装统一使用 `install-<preset>` build preset。
`TEST=1` 不会启用 CMake 测试选项；所选 configure preset 必须启用测试且提供同名
test preset。package-only 或交叉编译 profile 使用 `TEST=0`，不在主机运行目标测试。

`CACHE_ACTION=clone` 在指定路径创建共享缓存 checkout；已有目标会让 Git 报错。
`CACHE_ACTION=sync` 执行 `git pull --ff-only`；默认 `none`，不会自动改动该 checkout。
局部 target 和 CTest 过滤仍可直接使用已有 user presets。

## Restore 后构建

```sh
# 默认沿用 ci-sdk-release-user 的 package-only 配置，不运行测试。
praktor -f sdk-build.yml -i INSTALL=1

# Restore 后使用 Windows 本地 Release 测试配置。
praktor -f sdk-build.yml -i PRESET=win-release-user -i TEST=1

# Restore 后使用 native CI 的完整测试配置。
praktor -f sdk-build.yml -i PRESET=ci-sdk-release-tests-user -i TEST=1

# Linux SDK 打包。
praktor -f sdk-build.yml -i HOST=unix -i RID=linux-x64 -i TARGET_TRIPLET=x64-linux -i HOST_TRIPLET=x64-linux -i INSTALL=1
```

设置 `SDK_RESTORE_ROOT` 为本工程的 `build/native-sdk` 等专用目录；CI 未设置时继续
使用 `RUNNER_TEMP`。目录保存 restore 工程和 assets，不删除用户安装 SDK。
`QIGAO_NUGET_PACKAGES` 可指定既有 NuGet payload 缓存。并行 restore 使用不同 restore 目录。

脚本仍从 GitHub Packages 按本工程的 `Salts.Native 2.3.0-rc.16`、
`SaltsUtils.Native 4.3.0-rc.11` 精确版本解析并校验解析结果，使用 `--no-cache --force-evaluate`，
保留 `SALTS_CANDIDATE_ROOT` 覆盖及可选 TurboWasm。这里未采用 TurboAgent 的
GitHub Releases 下载策略，也未改变包版本约束。

SDK 根从本次 `project.assets.json` 解析并检查后一次性输出。GitHub 调用默认追加
`GITHUB_ENV`；Praktor 调用第三个参数 `json`，从 stdout 接收根目录并显式传入
子 workflow。诊断输出到 stderr。Windows 在 Git Bash 与原生工具边界转换路径，
输出 `C:/...` 供 CMake 使用。

原 `restore-native-sdks.ps1 -Rid windows-x64 -Local` 继续可用，现在只转发到 Bash，
并将 JSON 根目录写入当前 PowerShell 进程；需要同样的 Git Bash/Python 环境。

`sdk-build.yml` 的 `RID`、`TARGET_TRIPLET`、`HOST_TRIPLET` 必须与 SDK、编译器一致。
Windows 默认均为 x64 Windows。macOS 使用 `ci-macos-sdk-release-user`；Android 使用
`ci-android-sdk-release-user` 和 `ANDROID_NDK_HOME`；iOS 使用
`ci-ios-sdk-release-user` 和 `QIGAO_IOS_SYSROOT`。这些 profile 保留现有安装布局
`stage/sdk/<RID>`。CI SDK preset 保留 `--only-binarycaching`，缺失二进制包会明确失败。

Native CI 使用 `ci-sdk-release-tests-user`（Windows/Linux）和
`ci-macos-sdk-release-tests-user`（macOS），在 preset 内启用 `BUILD_TESTS` 与
`BUILD_TESTING`，构建目录为 `build/native-sdk-tests`。对应 build/test/install 入口
使用同名 profile；workflow 不再通过 `-D` 改写测试开关。原 package-only 入口继续
使用 `build/native-sdk`，Android/iOS 的测试策略不变。
本工程测试构建选项是 `BUILD_TESTS`，CTest 注册由 `BUILD_TESTING` 控制；
`ENABLE_TESTS` 不是本工程选项。

## 配置归属与回滚

`presets/BasePresets.json` 提供共享 Ninja/vcpkg 组合，`CMakePresets.json` 保存
SDK 功能与交叉编译策略，`CMakeUserPresets.json` 保存环境路径和公开入口。
安装入口继承匹配的 build preset 和单一 `base-install`；遵循
[CMake preset 继承规则](https://cmake.org/cmake/help/v3.25/manual/cmake-presets.7.html)。
公开 preset 名、SDK 根选择和安装布局保持不变。

这次选择复用 Bash restore，避免两套依赖解析逻辑漂移；代价是本机 Windows restore
也需要 Git Bash 和 Python。Praktor 只是可选构建工具，不加入 CHTTP 的库依赖。
现有 GitHub Actions 仍直接调用 CMake，没有新增 Praktor 的 CI 引导依赖。
回退编排时可直接调用既有 presets；回退配置改动时须同时还原 preset 定义与文档，
并按原规则设置缓存 checkout。SDK payload 和用户 `.env` 无需迁移。
