# CHttp

[![CI](https://github.com/qigao/chttp/actions/workflows/ci.yml/badge.svg)](https://github.com/qigao/chttp/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/qigao/chttp)](LICENSE)

C11/C++17 HTTP clients and servers, JSON-RPC, S3, WebSocket, server-driven HTML,
and OpenAPI tooling built on the Salts libraries.

CHttp owns HTTP and application protocol policy. CNet owns connection progress
and TLS; SaltsBase owns NativeIO and platform primitives; CMeta owns metadata,
plugins and CFlow execution; SaltsUtils supplies DataBind, IDL and Jinja.

## Upstream dependency versions

| Release input | CMake package | SDK root | Responsibility |
| --- | --- | --- | --- |
| `SALTS_BASE_VERSION=0.1.4` | `SaltsBase` | `SALTS_BASE_ROOT` | Core, NativeIO, parsers, TinyTest |
| `CMETA_VERSION=0.1.4` | `CMeta` | `CMETA_ROOT` | CMeta, CFlow, CSTL, plugins |
| `SALTS_CORE_VERSION=0.1.3` | `SaltsCore` | `SALTS_CORE_ROOT` | CNet and Crypto |
| `SALTS_UTILS_VERSION=0.1.0` | `SaltsUtils` | `SALTS_UTILS_ROOT` | DataBind, IDL, Jinja |
| `VCPKG_CACHE_VERSION=0.1.3` | preinstalled vcpkg profile | `VCPKG_INSTALLED_DIR` | Third-party packages and compiler contract |

Versions come from the parent environment or a locally loaded `.env`, without
the `v` prefix. CMake does not load dotenv files. Merge [`.env.example`](.env.example)
into existing machine settings and keep credentials private. Each SDK root must
match the compiler, architecture, configuration and selected release. Package
discovery is exact and restricted to those roots; SDK roots are never added to
`CMAKE_PREFIX_PATH`.

**Migration status:** published SaltsUtils 0.1.0 omits its required `IDL.cmake`
and compiler templates. The released split Crypto SDK also lacks the general
JOSE/digest interfaces currently used by CHttp. Configuration/build must fail
until the upstream packages are repaired. Do not copy missing files into a
release SDK, disable authentication, substitute an older monolithic package,
or introduce a private crypto implementation to make the build pass.

See [the migration decision and validation record](docs/SPLIT_SDK_MIGRATION.md)
and [build instructions](cmake/BUILD.md).

## Modules

| Module | CMake target | Public entry points |
| --- | --- | --- |
| App (generated services + server-driven Web) | `CHttp::App` | `<chttp_app/app.h>` |
| HTTP / RPC client | `CHttp::Client` | `<http_client/http.h>`, `<http_client/rpc.h>` |
| HTTP / RPC server | `CHttp::Server` | `<http_server/http.h>`, `<http_server/rpc.h>` |
| S3 client | `CHttp::S3` | `<s3/s3.h>` |

Client and Server each contain their own RPC-side implementation and link independently. S3 is a separate module built on `CHttp::Client`.

### App entry point

Applications combining generated IDL services and Jinja pages can include
`<chttp_app/app.h>` and link only `CHttp::App`. This installed C11/C++17
shared library contains the Service and Web implementations. The HTTP server
example and the App behavior tests use this entry point.

Service and Web are fully merged into `app/` and one `chttp_app` shared library.
The earlier interface-only facade left two deployment units and duplicate
module entry points; the shared library gives applications one build, include
and deployment boundary. `CHttp::Server` remains independent of Jinja.
App consumers acquire both the generated-service and rendering dependencies.

Within App, Service still owns MethodPlan admission, invocation storage,
executor work and plugin leases; Web owns rendering and browser adapters.
Their existing function names, struct layouts, configuration, error codes and
stop/drain/destroy contracts remain intact. Merging libraries creates no global
runtime, extra dispatch, threads or allocations, and makes no CPU-speed claim.

Migration requires rebuilding applications: replace the old Service/Web link
targets with `CHttp::App`, and includes with `<chttp_app/app.h>` (or the focused
`<chttp_app/service.h>` / `<chttp_app/web.h>`). Deploy `chttp_app` instead of the
two old shared libraries. Old binaries cannot use the new library without
relinking. Install into a clean SDK prefix so stale libraries are not shipped.
Rollback restores the preceding SDK and rebuilds consumers against its targets.

## Repository layout

```text
http_client/  include/http_client/  src/  rpc/  tests/
http_server/  include/http_server/  src/  rpc/  tests/  examples/
http_common/  include/http_common/  http/  rpc/  tests/
app/          include/chttp_app/  src/  tests/  examples/  benchmarks/
s3/           include/s3/  src/  tests/
openapi/      OpenAPI generation support
vendor/       local third-party integration
```

`http_common/` owns shared HTTP/RPC protocol types and mechanisms only. It does not own client/server application state and is not exported as an independent public package target.

## HTTP server features

The server supports Cookie Session plus global and route-level middleware. RPC shares the same initialized `chttp_server`: attach its bounded registry with `chttp_server_rpc_configure()` and register methods before startup.

See:

- [HTTP server](http_server/README.md)
- [HTTP client](http_client/README.md)
- [S3 client](s3/README.md)

## CHttp::App

[CHttp::App](app/README.md) is the optional native-C server-driven web
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

Reference applications live in `app/examples/`. The authenticated application
example demonstrates login, Session fixation defense, explicit authorization,
asset serving, ordinary/HTMX rendering, and logout end to end. OpenAPI UI is a
qualification application on the same generic Web layer. CHttp::App remains
intentionally server-driven rather than a client-side framework or Wt-style
widget toolkit.

## OpenAPI

The optional [OpenAPI generator](openapi/README.md) produces OpenAPI 3.1 documents from C source annotations and supports standard schema constraints.

Enable it with:

```text
BUILD_OPENAPI=ON
```

## Build and test

After restoring matched SDKs and loading their environment variables:

```powershell
cmake/WithVs.cmd cmake --fresh --preset native-release-user
cmake/WithVs.cmd cmake --build --preset native-release-user
cmake/WithVs.cmd ctest --preset native-release-user --output-on-failure --no-tests=error
cmake/WithVs.cmd cmake --build --preset install-native-release-user
```

Windows requires `VSDEVCMD_BAT` and the selected compiler SDK. Linux/macOS use
`llvm-native-release-user` (or Linux `gcc-release-user`) without the Windows
wrapper. Every module owns its tests. `BUILD_TESTS` and `BUILD_BENCHMARK` select
the corresponding complete graphs; examples retain `BUILD_EXAMPLES`.

The [split SDK CI](.github/workflows/ci.yml) downloads release inputs once,
verifies their manifests and SHA-256 digests, and shares locked original blobs
with the matrix workers. Workers restore the selected profile, build the full
graph, run native tests through CTest, and install/archive the SDK. Android
uses a separately restored Linux host `salts-idlc` and its Base/CMeta runtime;
cross-building and installation do not prove device execution.

The current published SDK intersection contains eight profiles: Windows MSVC
and LLVM-MinGW x64, Linux Clang and GCC x64, Linux Clang x64 TSan, macOS Clang
ARM64, Android ARM64 and x64. CNet 0.1.3 has no published Linux ARM64 SDK, so
the Base/CMeta/Utils ARM64 Linux lanes cannot be used for this composition.
Missing assets and failed jobs stop their flows. There is no source-build
fallback, floating NuGet selection or automatic retry.

## Using CHTTP from CMake

```cmake
find_package(Chttp CONFIG REQUIRED
  PATHS "$ENV{CHTTP_ROOT}"
  NO_DEFAULT_PATH)

target_link_libraries(my_app PRIVATE CHttp::Client)
```

Use `CHttp::Server` for protocol/server applications, `CHttp::App` for server-driven HTML applications, and `CHttp::S3` for S3 consumers.

The package resolves SaltsBase, CMeta, SaltsCore and SaltsUtils through the explicitly configured matching profiles. The build is fail-fast and does not silently fall back to unrelated SDK roots.

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

## Contributing

Read [AGENTS.md](AGENTS.md), keep protocol and ownership contracts explicit,
and add regression coverage to the owning module's existing test suite.

## License

See [LICENSE](LICENSE), [NOTICE](NOTICE), and
[third-party notices](THIRD_PARTY_NOTICES.md).
