# CHttp ServiceHostCandidate — isolated HTTP conformance

Tracking: [CHttp #224](https://github.com/qigao/chttp/issues/224),
[Draft PR #249](https://github.com/qigao/chttp/pull/249), and
[Salts #1154](https://github.com/qigao/salts/issues/1154).

## Scope and source identity

- The CHttp implementation remains a **private, opt-in candidate**, not a
  production/default ServiceHost switch.
- `CHTTP_ENABLE_SERVICE_HOST_CANDIDATE` defaults to OFF. ON builds require
  active CTests, use the **original** Salts `component-runtime/` source and
  disable installation/export rules via `CMAKE_SKIP_INSTALL_RULES`.
- Pinned Salts candidate source SHA:
  `540e1da7c2448f15f04aeaaee1106f73a2fbcc7e`.
  No Component Runtime or Plugin loader is copied into CHttp.
- Exact published dependency versions used by this branch's Linux consumer
  checks: `Salts.Native 2.3.0-rc.16` and
  `SaltsUtils.Native 4.3.0-rc.11`.
- Existing public `CHttp::App` Service headers and normal SDK behavior remain
  unchanged; no fallback or additional registry/DSO authority is introduced.

## Real-HTTP and H2 multiplex conformance profiles

The original `chttp_service_http_test` covers one mounted, typed Component
operation under an address-stable ServiceHost Scope and the existing
`DataBindNativeExecution`/MethodPlan/deferred cleanup path. Its historical
N and N+1 mounts run on two HTTP Servers; that **alone** is not a live
single-listener proof.

`chttp_service_host_single_listener_test` keeps one Server, one route,
one TCP listener, and two real IDL Provider DSOs. Its bounded test-local
typed dispatcher demonstrates N→N+1 while N's deferred work is still
outstanding, without adding a second Plugin loader.

`chttp_service_host_methodplan_switch_test` exercises the actual CHttp
`chttp_service_http_execute_deferred()` code path through a **single**
running Server/Listener and two HTTP owner lanes. The private candidate
`chttp_service_mount_host_switch_candidate()` registers the stable route
before start. The control thread invokes
`chttp_service_host_switch_prepare_candidate()` on a BUILT generation,
validates the exact typed Interface/Function ABI/MethodPlan, then the
application explicitly publishes that generation through Salts ServiceHost.

The separate `chttp_service_host_methodplan_h2c_test` repeats the full
MethodPlan / ServiceHost lifecycle on explicit HTTP/2 prior knowledge
(no HTTP/1.1 fallback). `chttp_service_host_h2_multiplex_test` uses
**one** caller-driven advanced HTTP/2 Client, with a hard physical connection
capacity of **1** and concurrent request slot capacity of **2**. It
warms the H2 session before admitting two real Streams, then crosses the
N+2→N+3 generation boundary while an old Stream remains queued at its
Provider. The third request returns `SALTS_ENOBUFS`; canceling only that
old Stream produces exactly one client cancellation callback while a sibling
Stream and a later request both complete with the N+3 result `111`.
The original generation continues reporting BUSY **after** client Stream
cancellation until the native CFlow finalizer settles.

`chttp_service_host_h2_stream_window_test` reuses that scenario but sets
the receiver's `SETTINGS_INITIAL_WINDOW_SIZE` to **1 byte** on its single
H2 connection. The 2- and 3-byte MethodPlan outputs require per-Stream
`WINDOW_UPDATE` progress in order to complete. This qualifies bounded
request-slot backpressure and tiny-window Stream credit progression, but not
full connection-level 65,535-byte window exhaustion or sustained large-body
throughput.

Every admitted HTTP invocation pins exactly one nonmoving per-call
ServiceHost Scope; the request's generation ID chooses its pre-admitted,
immutable binding. No dynamic Service/Plugin lookup happens in the request
hot path. CHttp continues to own ingress validation, native request/error
ownership, response serialization, middleware continuation, bounded
executor admission, deferred terminal and finalization.

## Generation and failure contracts

- An old N request returns `11` even if N+1 is published in the meantime.
  A new request to the **same port and route** returns N+1 `111`.
- HTTP middleware continuation enters and returns exactly once per request,
  without creating a separate async/DSO owner.
- Old N generation drain and Plugin DSO unload return BUSY until accepted
  requests and their native finalizers settle.
- A third prepared binding returns `SALTS_ENOBUFS` when both slots are
  occupied; no third attached generation/fallback is accepted.
- Reuse is explicit: after Salts confirms N `DRAINED`, and the finalizer
  count reaches zero, `chttp_service_host_switch_retire_candidate()`
  retires that old binding slot. The same route then admits N+2, returns
  `11` from its Provider, and retains the two-slot upper bound.
- Invalid MethodPlan ingress keeps the existing HTTP 400 semantics.
  Admission after ServiceHost close receives HTTP 503.
- Even if no work is pending, `chttp_service_destroy()` returns
  `SALTS_EBUSY` while the mounted Server still owns the route reference.
  The Server must be destroyed first.
- Provider Interface projection and calls execute on the control plane
  **outside** the admission mutex; only the fully constructed binding is
  published under the mutex.

## CI evidence

At exact CHttp source HEAD `5083756f552b716b2ee69abded13273d3a80d070`,
[CHttp candidate run #38843075392](https://github.com/qigao/chttp/actions/runs/38843075392)
completed **SUCCESS**. Linux Release, ASan+UBSan and TSan each ran all
**9/9 focused CTests**, with `--repeat until-fail:3`: **81 CTest executions
in total**, including H1, H2c, same-session multiplexer, stream-window=1
and original hot-path/lifecycle contracts. The independent
[post-cancel Scope BUSY check #38842749393](https://github.com/qigao/chttp/actions/runs/38842749393)
also passed all three modes before the tiny-window case was added.

For the TSan mode, `verify_servicehost_tsan_compiles.py` inspects the real
`compile_commands.json` and verifies `-fsanitize=thread` on the CHttp
Service source, both dedicated HTTP tests, both Provider DSO translation units,
and the **original** Salts Configurator/ServiceHost runtime source files.
Each mode also verifies that the candidate has **no install target**.

These are Linux candidate-consumer checks. They **do not** establish a fully
source-instrumented Salts Plugin/CMeta/Platform/CNet binary closure: some
upstream objects still come from the exact published SDK.

## Open before public or stable migration

1. Qualify the **full** first-party instrumented source/DSO closure under
   TSan, with deterministic N→N+1 cancel/drain races.
2. Qualify Linux/Windows/macOS native + installed SDK consumers and verify
   the deliberate future ServiceHost/Configurator public ABI/export cutover.
   Windows v145 vcpkg binary cache misses are independently tracked in
   [CHttp #250](https://github.com/qigao/chttp/issues/250).
3. Extend the H2 Stream cancellation/flow-control evidence to **connection
   window exhaustion, sustained large response bodies and multiple
   concurrent streams with real H2 RST_STREAM/WINDOW_UPDATE races**.
   The present one-byte Stream receive-credit case covers short typed outputs,
   not large-body throughput or network-level saturation.
4. Continue multi-Owner extended sessions and resource pressure/load soak
   beyond the deterministic two-stream capacity exercise.

**Draft / DO NOT MERGE / DO NOT PUBLISH.** No default production parser,
runtime, public SDK, or dependency pin is changed by the candidate mode.
