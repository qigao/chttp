# CHTTP

**C11/C++17 HTTP, server-driven Web, JSON-RPC, S3, WebSocket, and OpenAPI infrastructure built on Salts.**

CHTTP is the HTTP/application-protocol layer of the Salts ecosystem. It reuses Salts transport, lifecycle, bounded execution, and typed semantics instead of embedding a separate networking runtime.

**Tags:** C11 · C++17 · HTTP · JSON-RPC · S3 · WebSocket · OpenAPI · TLS · networking · async-io

## Built on Salts

CHTTP depends on the installed [Salts](https://github.com/qigao/salts) SDK and selected [SaltsUtils](https://github.com/qigao/salts-utils) components.

This integration branch requires the Salts #1001 candidate exporting
`Salts::CNetManager`, `<cnet/manager.h>` and `<cnet/handoff.h>`; the latest published SDK alone does
not yet provide that target. Each server owner lane uses one fixed-capacity
manager for TCP/TLS adoption and terminal attachment retirement. HTTP/1 deferred
responses and HTTP/2 deferred streams keep their contexts until completion.
Each lane's bounded admission inbox and generation-checked connection credits
now use the optional CNet handoff helper. CHTTP still chooses the final owner,
owns its listener/threads/TLS policy and protocol state, and keeps its public
server API. Successful publication transfers the descriptor even if the host
wake fails; shutdown drains that inbox after the existing `listener_done`
barrier ensures no listener wake can race backend destruction. Deploy the matching `cnet_manager` shared library with
the candidate SDK. Reverting this adapter and its private link dependency
restores raw CNet adoption without a protocol or data migration.

For host integration acceptance, dispatch `native-sdk-release.yml` with both
`salts_candidate_run_id` and `salts_candidate_sha`. The run must be a successful
Salts CI dispatch with retained SDK artifacts. Linux, Windows and macOS use the
selected artifact and run the formal CTest suite; SaltsUtils still resolves from
the package feed. Candidate mode skips cross compilation, packaging and
publication. Omit both inputs to retain the published-SDK release workflow.
The macOS SDK profile inherits `GccMac` (GCC 15), matching the producer SDK's
thread-local runtime ABI; Apple Clang's native TLS cannot link the GCC-built
TinyTest runtime's emulated TLS symbols.
Native asynchronous file upload/download and static-file responses currently
require Windows IOCP or Linux io_uring. macOS kqueue/poll do not provide the
regular-file operations required by CFlow, so those paths remain unsupported
(`SALTS_ENOTSUP`); there is no implicit synchronous or thread-pool fallback.
See [HTTP file transfer semantics](docs/HTTP.md) and the
[CFlow backend contract](https://github.com/qigao/salts/blob/4ebdaf4003394ae481747b6087bd6e19e38c40f3/cflow/README.md#native-socket-byte-pipe-and-regular-file-io).

The manifest includes Lua and QuickJS because the installed SaltsUtils package
exports those dependencies; it does not introduce another HTTP or TLS provider.

That gives the library a shared foundation:

- **CNet / NativeIO** for transport, connection progress, async I/O, TLS/session ownership, and shutdown semantics.
- **CMeta / CFlow** for typed metadata and execution boundaries where required.
- **SaltsUtils parsers and DataBind** for JSON, related higher-level formats, and typed data binding through `Salts::DataBind`.
- **SaltsUtils crypto helpers** where explicitly required by protocol features.
- **Salts provider-neutral crypto APIs** for protocol hashing, HMAC, legacy compatibility digests, constant-time comparison, and secret wiping; CHTTP does not select or link a crypto provider directly.

CHTTP owns HTTP, server-driven Web, RPC, S3, WebSocket, and OpenAPI domain behavior. It does not own Salts transport/runtime semantics and does not introduce a second hidden event loop.

## Ecosystem role

```text
Salts
  ├── salts-utils (including DataBind/TBE)
  └── salts-net
        ↓
      CHTTP
        ↓
  application / workflow / service layers
```

CHTTP is domain infrastructure: higher-level projects can depend on it for HTTP-family protocols while still sharing the same Salts ownership, error, and async-I/O model.

## Modules

| Module | CMake target | Public entry points |
| --- | --- | --- |
| HTTP / RPC client | `CHttp::Client` | `<http_client/http.h>`, `<http_client/rpc.h>` |
| HTTP / RPC server | `CHttp::Server` | `<http_server/http.h>`, `<http_server/rpc.h>` |
| Server-driven Web | `CHttp::Web` | `<chttp_web/web.h>` |
| S3 client | `CHttp::S3` | `<s3/s3.h>` |

Client and Server each contain their own RPC-side implementation and link independently. S3 is a separate module built on `CHttp::Client`.

## Repository layout

```text
http_client/  include/http_client/  src/  rpc/  tests/
http_server/  include/http_server/  src/  rpc/  tests/  examples/
http_common/  include/http_common/  http/  rpc/  tests/
web/          include/chttp_web/  src/  tests/  examples/
s3/           include/s3/  src/  tests/
openapi/      OpenAPI generation support
vendor/       local third-party integration
```

`http_common/` owns shared HTTP/RPC protocol types and mechanisms only. It does not own client/server application state and is not exported as an independent public package target.

## HTTP server features

The server supports Cookie Session plus global and route-level middleware. RPC users can obtain the HTTP owner through `crpc_server_http()` and configure middleware before startup.

See:

- [HTTP server](http_server/README.md)
- [HTTP client](http_client/README.md)
- [S3 client](s3/README.md)

## CHttp::Web

[CHttp::Web](web/README.md) is the optional native-C server-driven web
application layer. It combines CHTTP routing, middleware, sessions, security,
deferred responses and streaming with typed CMeta models and Jinja CMeta
server-side rendering.

It supports full-page SSR, HTMX-style fragment responses, bounded form
handling, Session-backed CSRF and flash messages, browser security policy,
Session-regenerated browser principals, protected-route authorization, safe
local login return targets, static asset/application-shell mounts, deferred
worker rendering, and SSE. Credential verification, password hashing, MFA,
identity-provider protocols, and account persistence remain application or
integration concerns.

Reference applications live in `web/examples/`. The authenticated application
example demonstrates login, Session fixation defense, explicit authorization,
asset serving, ordinary/HTMX rendering, and logout end to end. OpenAPI UI is a
qualification application on the same generic Web layer. CHttp::Web remains
intentionally server-driven rather than a client-side framework or Wt-style
widget toolkit.

## OpenAPI

The optional [OpenAPI generator](openapi/README.md) produces OpenAPI 3.1 documents from C source annotations and supports standard schema constraints.

Enable it with:

```text
BUILD_OPENAPI=ON
```

## Build and test

Requirements:

- CMake 3.25+
- Ninja
- vcpkg
- C11/C++17 compiler
- matching SDK profiles from the latest published Salts and SaltsUtils packages
- .NET SDK 8 for the native SDK restore scripts

Set `SALTS_ROOT`, `SALTS_UTILS_ROOT`, `PROJECT_ROOT`, and `VCPKG_ROOT` before configuration.
DataBind/TBE are components of the SaltsUtils installation. CHTTP consumes the
concrete `Salts::DataBind` target from `find_package(SaltsUtils CONFIG REQUIRED)`.

Local and CI builds use the [shared vcpkg cache](https://github.com/qigao/vcpkg-cache).
On Windows its checkout belongs at `%LOCALAPPDATA%/qigao/vcpkg-cache`; on Linux,
`$HOME/.cache/qigao/vcpkg-cache` (NuGet binary restore also requires Mono).
The hidden `vcpkg-cache` preset owns the outer toolchain, overlay ports, read-only
GitHub Packages feed and writable local binary cache. Provide `GITHUB_TOKEN`
with `read:packages` in the parent environment; credentials are never stored in
presets. Existing vcpkg baseline and manifest dependencies remain authoritative.

CI restores the latest published Salts and SaltsUtils packages with floating
NuGet versions and `--no-cache --force-evaluate`. SDK roots come from that
restore's `project.assets.json`, so older cached payloads cannot select an SDK.
CI presets inherit the cache environment supplied by the setup action; native
packaging uses `ci-sdk-release-user` and its Android/iOS variants.

For a local Windows restore, use PowerShell 7, put .NET SDK 8 on `PATH`, set `GITHUB_TOKEN`, then
run `./cmake/ci/restore-native-sdks.ps1 -Rid windows-x64 -Local` in the same
PowerShell session used for the build. It sets `SALTS_ROOT` and
`SALTS_UTILS_ROOT` to the resolved SDKs without overwriting installed packages.

Windows Release (enter the Visual Studio `VsDevCmd.bat` environment first,
retaining the configured dependency roots and `VCPKG_ROOT`):

```powershell
cmake --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user
cmake --build --preset install-win-release-user
```

Linux uses the corresponding `linux-*` presets.

Local install presets derive their destination from
`$PROJECT_ROOT/external/pkgs/chttp/debug|release`; Android uses `chttp-android`.
`CHTTP_ROOT` remains the downstream consumer's explicit CHTTP SDK root. SDK
packaging retains `stage/sdk/<RID>`. After changing the toolchain or SDK roots,
reconfigure with `cmake --preset win-release-user --fresh` before building.

### CMeta reflection and plugin lifetime

The 2.1 migration also uses the published `cmeta_*` platform/file APIs,
`<cmeta_buffer.h>`, `<cmeta_fs.h>` and `<cmeta_uuid.h>`. Existing `mem_*`
buffer operations, `SALTS_*` error codes and `Salts::*` CMake targets keep their
published names. Typed declarations use `cmeta_type` and `cmeta_function`.

Service and RPC continue to use producer-owned CMeta Function/Data descriptors
and exact DataBind adapters. Service mount performs admission before publishing
a route; request workers use the cached execution binding under its retained
plugin lease. Salts 2.x plugin types/functions use `cmeta_plugin_*` and
`CMETA_PLUGIN_*`; the runtime headers remain `<salts/plugin.h>` and the target
remains `Salts::Plugin`.

Service method slots use the CMeta cleanup obligation and Plugin lease adapter
from `<salts/plugin_scope.h>`. Each successful acquisition arms exactly one
obligation. Failed mount and Service destruction share its discharge path;
cleanup runs after dependent CFlow state is destroyed. The registry must remain
at a stable address until Service destruction, after server/executor drain.
Lease release invariant violations fail fast instead of being silently ignored.

Rebuild Service consumers and generated plugins together against matching
SDKs; old `salts_plugin_*` source names and earlier reflection/plugin ABI epochs
are not accepted. HTTP/RPC formats, export IDs and route semantics are unchanged.
The Service HTTP test covers rejected export cleanup, successful mount retention,
stop/admission closure and final unload.

Always restore the latest Salts and SaltsUtils packages together, then regenerate
IDL bindings and rebuild consumers against those SDKs. CMake does not pin or
constrain their versions.

## Using CHTTP from CMake

```cmake
find_package(Chttp CONFIG REQUIRED
  PATHS "$ENV{CHTTP_ROOT}"
  NO_DEFAULT_PATH)

target_link_libraries(my_app PRIVATE CHttp::Client)
```

Use `CHttp::Server` for protocol/server applications, `CHttp::Web` for server-driven HTML applications, and `CHttp::S3` for S3 consumers.

The package resolves Salts and SaltsUtils through the explicitly configured matching profiles. The build is fail-fast and does not silently fall back to unrelated SDK roots.

## Runtime boundaries

CHTTP follows the same explicit system rules as Salts:

- connection/session ownership is explicit;
- async work is bounded;
- shutdown and drain are explicit operations;
- protocol errors propagate through stable domain/runtime boundaries;
- no hidden product/session state is inserted below the public HTTP/RPC layer;
- no alternate runtime is selected when a configured dependency fails.

## Relationship to downstream projects

CHTTP is intended to be reused by higher layers such as TurboFlow, Flowie, service applications, and product adapters. Those projects own their business/session/workflow semantics; CHTTP owns only the HTTP-family protocol and service infrastructure.

## Origin and migration

This repository was extracted from the earlier Salts-hosted HTTP services implementation. Existing request lifecycle, state ownership, protocol, and error semantics were preserved through that move while package boundaries were made explicit.

Additional technical references:

- [HTTP runtime notes](docs/HTTP.md)
- [RPC runtime notes](docs/RPC.md)
- [CHttp::Web product guide](web/README.md)
- [module layout decision](docs/plans/2026-09-09-server-client-layout.md)

---

**Salts provides the systems runtime. CHTTP provides the HTTP-family protocol layer.**
