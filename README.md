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
| `SALTS_BASE_VERSION=0.1.8` | `SaltsBase` | `SALTS_BASE_ROOT` | Core, NativeIO, parsers, TinyTest |
| `CMETA_VERSION=0.1.6` | `CMeta` | `CMETA_ROOT` | CMeta, CFlow, CSTL, plugins |
| `SALTS_CNET_VERSION=0.1.12` | `SaltsCNet` | `SALTS_CNET_ROOT` | CNet and Crypto |
| `SALTS_UTILS_VERSION=0.1.4` | `SaltsUtils` | `SALTS_UTILS_ROOT` | DataBind, IDL, Jinja |
| `VCPKG_CACHE_VERSION=0.1.3` | preinstalled vcpkg profile | `VCPKG_INSTALLED_DIR` | Third-party packages and compiler contract |

Versions come from the parent environment or a locally loaded `.env`, without
the `v` prefix. CMake does not load dotenv files. Merge [`.env.example`](.env.example)
into existing machine settings and keep credentials private. Each SDK root must
match the compiler, architecture, configuration and selected release. Package
discovery is restricted to those roots. CMake does not take upstream version
arguments; version variables select SDK restoration only.

The selected vcpkg profile alone supplies `CMAKE_PREFIX_PATH`, using the existing
`VCPKG_INSTALLED_DIR`, preset-selected `VCPKG_TARGET_TRIPLET`, and configuration
subdirectory. First-party packages use explicit required `find_package` calls
with the SDK roots above. After changing an SDK, compiler or triplet,
configure explicitly with `cmake --fresh --preset <user-preset>`; normal configure
does not clear package caches or select another dependency source.

SaltsUtils 0.1.4 declares CMeta 0.1.6 source SDKs from immutable ref
`679710f7e3312811efdd92494744a2499286a6a5`, built against Base 0.1.8.
CI reuses that release's `build-cmeta-sdk.ps1` for target and Android host SDKs.
The older published CMeta binary targets Base 0.1.6 and is a different composition.

SaltsUtils 0.1.4 supplies IDL generation. `vendor/cjwt` owns JWT/JWE and its
private GmSSL/libecc adapter. HTTP and S3 use internal GmSSL digest helpers.
Provider provenance and ownership are documented in
[`vendor/cjwt/crypto/README.md`](vendor/cjwt/crypto/README.md).

See [the migration decision and validation record](docs/SPLIT_SDK_MIGRATION.md)
and [build instructions](cmake/BUILD.md).

## Modules

| Module | CMake target | Public entry points |
| --- | --- | --- |
| App (generated services + server-driven Web) | `CHttp::App` | `<chttp_app/app.h>` |
| HTTP / RPC client | `CHttp::Client` | `<http_client/http.h>`, `<http_client/rpc.h>` |
| HTTP / RPC engine primitives | `CHttp::Server` | `<http_server/http.h>`, `<http_server/rpc.h>` |
| S3 client | `CHttp::S3` | `<s3/s3.h>` |

Client and Server each contain their own RPC-side implementation and link independently. S3 is a separate module built on `CHttp::Client`.

### App entry point

Server applications include `<chttp_app/app.h>` (C11) or
`<chttp_app/application.hpp>` (C++17) and link `CHttp::App`. Application owns
the HTTP engine and a CMeta ACE Component graph. Controllers declare reflected
actions or generated DataBind MethodPlans; Services are injected through
canonical CMeta interfaces; ordered Interceptors surround HTTP dispatch.
Application owns a bounded worker pool for Controller/Service methods, DTO
conversion, Web rendering, upload/source/WS callbacks and terminal observers.
Actions receive a scoped Invocation with copied response staging and detached
Session intents while native owners continue protocol progress.

The public programming model is Application / Controller / Service. Manual
`chttp_service` mounting and Server route/middleware/RPC registration are now
internal implementation APIs. This is a source-breaking migration: rebuild
applications, install into a clean SDK prefix and replace manual registration
and Exchange/native response access with Controller/Invocation descriptors.
There are no compatibility aliases. Server remains the protocol engine and
retains HTTP/1.1, HTTP/2, TLS, sessions, streaming, WebSocket and RPC semantics.

Start with the buildable [C example](app/examples/application.c),
[C++ RAII example](app/examples/application.cpp), or the
[OpenAPI UI Controller](openapi/examples/ui_server.c).
[Application contracts](app/README.md#application-controller-and-service) and
the [design decision](docs/HTTP_APPLICATION_DESIGN.md) describe ownership,
reflection admission, interception and migration. Existing engine fixtures live
under their owning modules' tests; they are not public application examples.

Install into a clean SDK prefix so removed public headers are not shipped.
Rollback restores the preceding SDK and rebuilds consumers against it.

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

Native TLS interoperability tests require an independent OpenSSL 3 executable
on `PATH`. Linux sanitizer presets select `ENABLE_ASAN`, `ENABLE_UBSAN` and
`ENABLE_TSAN`; TSan is exclusive with ASan/UBSan. Refresh existing build caches
with the configure command above after migrating sanitizer option names.

The [split SDK CI](.github/workflows/ci.yml) downloads release inputs once,
verifies their manifests and SHA-256 digests, and shares locked original blobs
with the matrix workers. Workers restore the selected profile, build the full
graph, run native tests through CTest, and install/archive the SDK. Android
uses a separately restored Linux host `salts-idlc` and its Base/CMeta runtime;
cross-building and installation do not prove device execution.

The checked-in matrix selects eight profiles: Windows MSVC and LLVM-MinGW x64,
Linux GCC/Clang x64 plus Clang TSan, macOS ARM64, and Android ARM64/x64. Native
profiles run formal CTest; Android profiles cross-build and install.

## Using CHTTP from CMake

```cmake
find_package(SaltsBase CONFIG REQUIRED PATHS "$ENV{SALTS_BASE_ROOT}" NO_DEFAULT_PATH)
find_package(CMeta CONFIG REQUIRED PATHS "$ENV{CMETA_ROOT}" NO_DEFAULT_PATH)
find_package(SaltsCNet CONFIG REQUIRED PATHS "$ENV{SALTS_CNET_ROOT}" NO_DEFAULT_PATH)
find_package(SaltsUtils CONFIG REQUIRED PATHS "$ENV{SALTS_UTILS_ROOT}" NO_DEFAULT_PATH)
find_package(Chttp CONFIG REQUIRED
  PATHS "$ENV{CHTTP_ROOT}"
  NO_DEFAULT_PATH)

target_link_libraries(my_app PRIVATE CHttp::Client)
```

Use `CHttp::Server` for protocol/server applications, `CHttp::App` for server-driven HTML applications, and `CHttp::S3` for S3 consumers.

Consumers resolve the selected upstream packages before loading Chttp targets.
The SDK roots must belong to the same compiler/profile composition.

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

## CMake installation

Shared libraries use plain filenames without target VERSION/SOVERSION properties.
Installation uses GNUInstallDirs and exports `CHttp::` targets. The package config
loads those targets; consumers explicitly resolve their selected upstream CMake
packages first, as shown in [App package use](app/README.md#package-use).
After replacing an SDK or library naming scheme, install into a clean prefix and
rebuild dependent applications. Historical CHttp NuGet publication is retired.
