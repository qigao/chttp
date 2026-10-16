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
request-slot backpressure and tiny-window Stream credit progression.

The independent `chttp_service_host_h2_connection_window_test` uses one
actual H2 client session (physical connection capacity 1) and two simultaneous
48 KiB Source/Sink response streams. Each response fits the default per-Stream
65,535-byte window, but the 96 KiB aggregate exceeds the connection receive
window. The two responses complete only with connection-level
`WINDOW_UPDATE` progress. The Source terminal—not the request handler—
releases one address-stable ServiceHost Scope, and the real typed Provider's
N=11/N+1=111 output determines the streamed byte pattern. This does not
establish sustained gigabyte-size streaming, cancellation during exhausted
connection credit, or public candidate SDK support.

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

## CI evidence and sanitizer authority

**Last exact code HEAD with full end-to-end qualification:**
`dff5944919b08fd3655a3c338b2ea6e5f307fa0a`. The following independent
runs all completed SUCCESS:

- [Linux ServiceHost consumer #38848994979](https://github.com/qigao/chttp/actions/runs/38848994979):
  Release, ASan+UBSan, and a *historical mixed-SDK TSan diagnostic* each
  completed 10/10 focused CTests repeated three times (90 executions).
- [Native macOS ARM64 #38848994846](https://github.com/qigao/chttp/actions/runs/38848994846):
  exact published Salts Native 2.3.0-rc.16 / SaltsUtils Native 4.3.0-rc.11
  installed dependency SDKs; AppleClang and real Provider DSOs; 10/10 CTests
  repeated twice (20 executions), including the 48 KiB H2 connection-window test.
- [Complete first-party source TSan #38848994760](https://github.com/qigao/chttp/actions/runs/38848994760):
  source-installed Salts, SaltsUtils, and CHttp, all first-party compilation
  commands with `-fsanitize=thread`, no suppression or fallback; 10/10
  focused CTests repeated three times **plus** four sensitive H2 conformance
  tests repeated 20 times each (**110 CTest executions**). This is the
  authoritative Linux TSan result for the covered paths, not a proof of every
  possible scheduling/interleaving.

### Why full-source TSan is required

An earlier mixed-SDK [consumer run #38847793898](https://github.com/qigao/chttp/actions/runs/38847793898)
reported intermittent TSan conflicts between the Worker scalar encoder and
the H2 Owner's response-copy path. This profile linked prebuilt
**uninstrumented** Salts allocator/cache atomics; the missing happens-before
visibility is a plausible explanation, not a conclusively demonstrated
production race or a proven false positive. The fully source-instrumented
closure includes those allocator atomics and its later extended 80-iteration
H2 test found no races.

Accordingly, the **current** lightweight installed-dependency consumer
workflow checks Release and ASan+UBSan only. The independent
`chttp-servicehost-fullsource-tsan.yml` workflow is the **required**
unsuppressed TSan gate, validating real compile commands for original
Configurator/ServiceHost, CHttp, Provider DSOs, and the source-installed
Salts/SaltsUtils dependency graph. No `continue-on-error`, sanitizer
suppression, silent fallback or weakening of the full-source gate is used.
The historical mixed-SDK failure logs remain preserved for audit.

The candidate has no `install` target; no public CHttp/Salts SDK export or
default production behavior is altered.

## Open before public or stable migration

1. Windows-x64 native/installed SDK consumption: exact v145 ABI archives
   remain missing in the shared NuGet binary cache. [CHttp #250](https://github.com/qigao/chttp/issues/250)
   and upstream [vcpkg-cache #198](https://github.com/qigao/vcpkg-cache/issues/198)
   track the mismatch between generic warmer package ABIs and the exact
   CHttp consumer manifest. No changed triplet/version, source-build fallback
   or SDK publication is authorized.
2. Public ServiceHost/Configurator ABI/export naming and installed candidate
   SDK conformance remain separate from using officially released
   Salts/SaltsUtils dependencies in this test-only consumer.
3. Extended H2 large-body, connection-window-exhaustion **cancellation**
   races, peer RESET/WINDOW_UPDATE crossings, and longer multi-Owner capacity
   / soak tests remain open, beyond two 48 KiB streamed responses.

**Draft / DO NOT MERGE / DO NOT PUBLISH.** No default production runtime,
published SDK, or dependencies were switched by these tests.
