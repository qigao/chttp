# CHTTP

**C11/C++17 HTTP, server-driven Web, JSON-RPC, S3, WebSocket, and OpenAPI infrastructure built on Salts.**

CHTTP is the HTTP/application-protocol layer of the Salts ecosystem. It reuses Salts transport, lifecycle, bounded execution, and typed semantics instead of embedding a separate networking runtime.

**Tags:** C11 · C++17 · HTTP · JSON-RPC · S3 · WebSocket · OpenAPI · TLS · networking · async-io

## Built on Salts

CHTTP depends on the installed [Salts](https://github.com/qigao/salts) SDK and selected [SaltsUtils](https://github.com/qigao/salts-utils) components.

That gives the library a shared foundation:

- **CNet / NativeIO** for transport, connection progress, async I/O, TLS/session ownership, and shutdown semantics.
- **CMeta / CFlow** for typed metadata and execution boundaries where required.
- **SaltsUtils parsers** for JSON and related higher-level formats.
- **SaltsUtils crypto helpers** where explicitly required by protocol features.
- **BoringSSL/OpenSSL-compatible package dependencies** for the low-level cryptographic provider selected by the build.

CHTTP owns HTTP, RPC, S3, WebSocket, and OpenAPI domain behavior. It does not own Salts transport/runtime semantics and does not introduce a second hidden event loop.

## Ecosystem role

```text
Salts
  ├── salts-utils
  ├── salts-net
  └── DataBind
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
| S3 client | `CHttp::S3` | `<s3/s3.h>` |

Client and Server each contain their own RPC-side implementation and link independently. S3 is a separate module built on `CHttp::Client`.

## Repository layout

```text
http_client/  include/http_client/  src/  rpc/  tests/
http_server/  include/http_server/  src/  rpc/  tests/  examples/
http_common/  include/http_common/  http/  rpc/  tests/
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
- matching installed Salts and SaltsUtils SDK profiles

Set `SALTS_ROOT`, `SALTS_UTILS_ROOT`, `PROJECT_ROOT`, and `VCPKG_ROOT` before configuration.

Windows Release:

```powershell
cmake --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user
cmake --build --preset install-win-release-user
```

Linux uses the corresponding `linux-*` presets.

The current presets retain the historical environment variable `HTTP_SERVICES_ROOT` as the install-prefix variable. It names the CHTTP package root; it should not be interpreted as a separate runtime or repository boundary.

## Using CHTTP from CMake

```cmake
find_package(Chttp CONFIG REQUIRED
  PATHS "$ENV{HTTP_SERVICES_ROOT}"
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
- [module layout decision](docs/plans/2026-09-09-server-client-layout.md)

---

**Salts provides the systems runtime. CHTTP provides the HTTP-family protocol layer.**
