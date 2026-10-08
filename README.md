# CHTTP

**C11/C++17 HTTP, server-driven Web, JSON-RPC, S3, WebSocket, and OpenAPI infrastructure built on Salts.**

CHTTP is the HTTP/application-protocol layer of the Salts ecosystem. It reuses Salts transport, lifecycle, bounded execution, and typed semantics instead of embedding a separate networking runtime.

**Tags:** C11 · C++17 · HTTP · JSON-RPC · S3 · WebSocket · OpenAPI · TLS · networking · async-io

## Built on Salts

CHTTP depends on the installed [Salts](https://github.com/qigao/salts) SDK and selected [SaltsUtils](https://github.com/qigao/salts-utils) components.

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

### CMeta reflection and Component generation lifetime

The 2.1 migration also uses the published `cmeta_*` platform/file APIs,
`<cmeta_buffer.h>`, `<cmeta_fs.h>` and `<cmeta_uuid.h>`. Existing `mem_*`
buffer operations, `SALTS_*` error codes and `Salts::*` CMake targets keep their
published names. Typed declarations use `cmeta_type` and `cmeta_function`.

Service and RPC continue to use producer-owned CMeta Function/Data descriptors
and exact DataBind adapters. Component-backed Service mount performs admission
before publishing a route: it acquires one `Salts::ComponentPlugin` generation
scope, resolves an explicitly named `chttp_service_operation_provider`, copies
the admitted native binding/execution into the mounted method record, and
performs no Component or Plugin lookup on request/deferred hot paths.

The Component generation is the outer module-lifetime authority. It retains the
underlying Plugin lease while mounted Service scopes and other admitted users
may still reach provider metadata/code. Service destruction first waits for
accepted deferred invocations, destroys dependent CFlow state, then releases
its generation scope. A draining generation cannot stop/release provider
instances or Plugin leases until that scope is gone.

The integration branch intentionally removes the former Service-private
Plugin-registry/catalog/lease mount path rather than keeping a fallback. HTTP
projection, MethodPlan/native ownership semantics, route behavior, and exact
execution ABI remain unchanged.

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

## Component runtime candidate qualification

The integration-only `Component runtime conformance` workflow checks out the
consumer event SHA and restores the exact Salts 3.0 prerelease
`3.0.0-cmeta.06fa2b10b418f997c38f123ddefa00e448897617` for `linux-x64`.
It verifies package SHA256
`682122da918658bf958fc409dd148157962e128884b21a91df18c5b7e94589ca`
and the SDK commit/RID/profile manifest before configuration. SaltsUtils source
`1c00cab3c5fe4722d4c8488a47f0ada6ec2f3e6b` is rebuilt against that SDK;
the workflow does not use stable first-party binaries for this candidate.

The HTTP test publishes a second DSO while old deferred work is queued. It checks different response bodies from the old/new mounted routes, BUSY destruction/drain/unload, and ordered retirement. Component scopes belong to mounted method records and survive until Service destruction; closing Component admission does not invalidate existing routes.

With the workflow's installed dependency roots and shared vcpkg/re2c environment,
run the complete configured build and CTest suites, then verify the installed
consumer independently:

```sh
cmake --preset ci-component-release-user
cmake --build --preset ci-component-release-user -j2
ctest --preset ci-component-release-user --no-tests=error --output-on-failure
cmake --build --preset install-ci-component-release-user -j2
cd service/tests/installed
cmake --preset ci-component-installed-user
cmake --build --preset ci-component-installed-user -j2
ctest --preset ci-component-installed-user --no-tests=error --output-on-failure
```

CI selects Component and adjacent business suites from the full configured graph
and uploads consumer/dependency identities, JUnit results, and CTest logs as
`component-acceptance-linux-x64`. The root CTest command above runs the broader
suite. A green Linux Release run establishes only this profile's acceptance;
sanitizer qualification and Windows/macOS downstream runs remain separate release
gates. This workflow neither merges nor publishes a stable release.
