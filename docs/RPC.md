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

Server applications include `<chttp_app/app.h>` and link `CHttp::App`.
The low-level registration and generated mount APIs are internal. Declare a
`chttp_rpc_method` on an ACE-owned Controller, supplying the endpoint path,
immutable `DataBindRpcMethodPlan`, canonical native binding and execution.
Application binds the execution context to the Controller receiver, validates
the generated ABI, and configures the protocol registry before listener start.
Set `chttp_application_config.rpc` capacities explicitly and use
`chttp_application_init/start/close` for the complete lifetime. Plans and
provider metadata remain alive until close settles. See
[Application contracts](../app/README.md#application-controller-and-service).

Registration is bounded by configured method capacity. Each generated native invocation leases an independent preallocated request/response/error frame and DataBind workspace; immutable MethodPlan descriptors are borrowed until Server drain/destroy. Native frame alignment and padding are charged to the configured frame capacity. Ingress uses canonical DataBind binding, and `DataBindBindingCallLifetime` is restored exactly once before the slot is returned. Unsupported generated ABI/ownership/layout is rejected during mount. Server stop/destroy controls the entire RPC registry; there is no independent RPC stop or teardown.

Generated RPC ingress admits numeric/boolean/enum scalar fields and **owned strings with complete CMeta v2 buffer operations** (`init_zero`, `restore_zero`, `move`). The owned native string is initialized inside a bounded per-call frame and released by canonical DataBind rollback on partial decode failure or by the terminal `DataBindBindingCallLifetime` restoration on success/native failure. Borrowed and custom buffer providers do not gain ingress admission; output remains a single scalar result or supported scalar typed error, not arbitrary structured/string egress. `native_max_owned_bytes`, `native_max_items`, `native_workspace_bytes`, and `max_inflight_calls` remain independent hard bounds. A provider's descriptor and callback pointers must outlive Server drain/destroy, with an explicit Plugin generation lease when DSO-owned.

## Client

Include `<http_client/rpc.h>` for JSON-RPC envelopes and `<http_client/rpc_service.h>` for typed generated projection. Link **only** `CHttp::Client`. These APIs operate on an already initialized `chttp_client` or `chttp_async_client`:

- `chttp_client_rpc_request_reply()`: blocking JSON-RPC request/reply with explicit bounded envelope limits.
- `chttp_client_rpc_call()`: blocking generated MethodPlan/native response/error projection.
- `chttp_async_client_rpc_submit()`: direct asynchronous JSON-RPC, using the normal `chttp_request` handle.
- `chttp_async_client_rpc_call()`: asynchronous typed MethodPlan/native response/error projection, in the HTTP terminal callback.

The async calls use the same `chttp_async_client_poll()`, `chttp_async_request_cancel()`, `chttp_async_client_stop()` and `chttp_async_client_destroy()` as ordinary HTTP. Active JSON-RPC request IDs and absolute RPC deadlines are recorded in existing bounded HTTP request slots. Duplicate active IDs fail admission with `SALTS_EALREADY`, without publishing a request handle or modifying a rejected typed output. Nonzero `deadline_ms` is handled by the HTTP Owner poll budget and native cancellation. The callback reports timeout only after native terminal completion, rather than inventing a second timer or replaying requests.

Generated input binding uses the immutable DataBind plan: optional presence/null semantics, positional versus named selectors, native ABI, and typed-error mapping. Unsupported mixed or complex projections fail closed. Successful native response values follow the normal caller-owned DataBind lifecycle.

## Conformance and lifecycle

Tests include mixed HTTP GET / JSON-RPC POST on one Server; generated native success, defaulting and remote error; three HTTP Owners contending for two bounded generated frame slots; H1/H2 asynchronous request cancellation and absolute RPC deadline; duplicate active wire IDs; rejected typed-call output staying unchanged; and stop-timeout/drain before descriptor release. The owned-native conformance uses two heap-backed string fields in a real Server-mounted RPC method, with partial-ingress/duplicate-field rollback, successful and failed native execution, and counted exactly-once buffer releases checked only after stop/destroy. Separately, the CSerde failure test rejects a partially written result and checks the next RPC does not inherit staged bytes.

Formal protocol tests retain private engine access; public server applications link `CHttp::App`, while standalone RPC clients link `CHttp::Client`.

The following are **not** provided: automatic settlement retry/replay, dynamic schema interpretation in the hot path, streaming JSON-RPC, HTTP/3 or a second CNet runtime. Exact cancellation, plugin generation and sanitizer checks remain explicit release gates.

The module-owned `crpc_server_test` also runs an explicit HTTP/2 request with
430 state values, 810 history values and 64 rows of 54 binary action values.
The real CSerde reader checks every position and the client validates all
64 deterministic count-of-bits results. Explicit 32 KiB request limits admit
the payload; an 8 KiB RPC encoding limit returns `SALTS_EMSGSIZE` at
`rpc-prepare` without another handler call. This is protocol-engine coverage,
not generated structured RPC egress or a trained-model/downstream acceptance.
