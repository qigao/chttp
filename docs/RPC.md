# CHttp RPC — integrated Server/Client

JSON-RPC 2.0 is a **protocol capability of `CHttp::Server` and `CHttp::Client`**, not an independently owned transport service. Both reuse the existing CNet connection/Owner graph. The former `CHttp::RpcService` shared-library target and `<chttp_rpc_service/service.h>` public API are intentionally removed, without a compatibility alias.

## Architecture

```text
Immutable DataBindRpcMethodPlan / CMeta ABI
                   |
       +-----------+------------+
       |                        |
       v                        v
CHttp::Server              CHttp::Client
chttp_server_rpc_mount     chttp_client_rpc_call
bounded per-call native    chttp_async_client_rpc_call
frame slots                 request-slot ID + deadline
       |                        |
       +-----------+------------+
                   v
             HTTP/1.1 or HTTP/2
                   v
          single CNet Owner/Pool
```

## Server

Include `<http_server/rpc.h>` for low-level JSON-RPC registration and `<http_server/rpc_service.h>` for generated-plan mounting.

```c
chttp_server server = {0};
chttp_server_rpc_config rpc = CHTTP_SERVER_RPC_CONFIG_INIT;
chttp_server_rpc_mount_options mount = CHTTP_SERVER_RPC_MOUNT_OPTIONS_INIT;

/* http_config and generated method_plan/native_binding/execution are supplied
   by the caller; configure/mount must precede server start. */
chttp_server_init(&server, &http_config);
chttp_server_rpc_configure(&server, &rpc);
mount.target = "/rpc";
mount.method_plan = method_plan;
mount.native_binding = native_binding;
mount.execution = execution;
chttp_server_rpc_mount(&server, &mount);
chttp_server_start(&server);
```

Registration is bounded by configured method capacity. Each generated native invocation leases an independent preallocated request/response/error frame and DataBind workspace; immutable MethodPlan descriptors are borrowed until Server drain/destroy. Native frame alignment and padding are charged to the configured frame capacity. Ingress uses canonical DataBind binding, and `DataBindBindingCallLifetime` is restored exactly once before the slot is returned. Unsupported generated ABI/ownership/layout is rejected during mount. Server stop/destroy controls the entire RPC registry; there is no independent RPC stop or teardown.

## Client

Include `<http_client/rpc.h>` for JSON-RPC envelopes and `<http_client/rpc_service.h>` for typed generated projection. Link **only** `CHttp::Client`. These APIs operate on an already initialized `chttp_client` or `chttp_async_client`:

- `chttp_client_rpc_request_reply()`: blocking JSON-RPC request/reply with explicit bounded envelope limits.
- `chttp_client_rpc_call()`: blocking generated MethodPlan/native response/error projection.
- `chttp_async_client_rpc_submit()`: direct asynchronous JSON-RPC, using the normal `chttp_request` handle.
- `chttp_async_client_rpc_call()`: asynchronous typed MethodPlan/native response/error projection, in the HTTP terminal callback.

The async calls use the same `chttp_async_client_poll()`, `chttp_async_request_cancel()`, `chttp_async_client_stop()` and `chttp_async_client_destroy()` as ordinary HTTP. Active JSON-RPC request IDs and absolute RPC deadlines are recorded in existing bounded HTTP request slots. Duplicate active IDs fail admission with `SALTS_EALREADY`, without publishing a request handle or modifying a rejected typed output. Nonzero `deadline_ms` is handled by the HTTP Owner poll budget and native cancellation. The callback reports timeout only after native terminal completion, rather than inventing a second timer or replaying requests.

Generated input binding uses the immutable DataBind plan: optional presence/null semantics, positional versus named selectors, native ABI, and typed-error mapping. Unsupported mixed or complex projections fail closed. Successful native response values follow the normal caller-owned DataBind lifecycle.

## Conformance and lifecycle

Tests include mixed HTTP GET / JSON-RPC POST on one Server; generated native success, defaulting and remote error; three HTTP Owners contending for two bounded generated frame slots; H1/H2 asynchronous request cancellation and absolute RPC deadline; duplicate active wire IDs; rejected typed-call output staying unchanged; and stop-timeout/drain before descriptor release.

The standalone C11 and C++17 public API/installed SDK conformance links only `CHttp::Server` and `CHttp::Client` (plus their declared transitive dependencies), never `CHttp::RpcService`.

The following are **not** provided: automatic settlement retry/replay, dynamic schema interpretation in the hot path, streaming JSON-RPC, HTTP/3 or a second CNet runtime. Exact cancellation, plugin generation and sanitizer checks remain explicit release gates.
