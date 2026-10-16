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


### Partial-response cancellation

The separate `chttp_service_host_h2_connection_cancel_test` uses one
caller-driven H2 connection and a 1-byte initial Stream receive window.
After a 48 KiB Source has delivered at least one byte, it cancels that
Stream without cancelling an independent short-response sibling. The Source
terminal runs exactly once; its original Scope is released there, not at
route-handler return. No third Owner, widened capacity, or fallback is added.

### Private staged SDK, independent C11/C++ consumers

`chttp_service_host_staged_sdk_consumer` uses the **original** Salts candidate
headers and two compiled static libraries, staged outside the CHttp build
tree. Two independent C11 and C++17 projects use the *published*
`find_package(Salts)` dependencies and import the candidate static archives
explicitly. Two actual generated Provider DSOs exercise generation
publication, address-bound Scope rejection of illicit copies, BUSY drain,
Plugin leases, and ordered unload. The test rejects a staged package that
pretends an unshipped `Salts::ServiceHostCandidate` SDK target is public.

On Windows, exactly published Salts/SaltsUtils dependency DLLs are staged
beside the Provider DLLs, in compliance with the original Plugin loader's
`DLL_LOAD_DIR | DEFAULT_DIRS` search. The staging script normalizes DLL
names case-insensitively and requires identical SHA-256 for any duplicate
name; conflicting SDK DLL identities are a hard error, not copy-order
selection. No PATH/cwd DLL loader fallback is allowed.

## CI evidence and sanitizer authority

The following independent latest successful workflows qualify the
**unchanged candidate runtime** and its private out-of-tree consumer:

| Profile | Exact evidence | Result |
| --- | --- | --- |
| Linux x64 Release | [#38860394155](https://github.com/qigao/chttp/actions/runs/38860394155), `d9f1090` | 12/12 CTests ×3 |
| Linux x64 ASan+UBSan | Same run | 12/12 CTests ×3 |
| Windows x64 / MSVC v145 / IOCP | [#38860394261](https://github.com/qigao/chttp/actions/runs/38860394261), `d9f1090` | 12/12 CTests ×2; 5 critical IOCP/H2 tests ×20; external C11/C++ DSO test ×3 |
| macOS ARM64 / AppleClang | [#38859979155](https://github.com/qigao/chttp/actions/runs/38859979155), `eb501a8` | 12/12 CTests ×2 |
| Whole first-party source TSan, Linux | [#38851580599](https://github.com/qigao/chttp/actions/runs/38851580599), earlier runtime head | 11/11 CTests ×3; 5 H2 cases ×20; no sanitizer reports |

The full-source Linux TSan profile checked **214 Salts + 240 SaltsUtils +
227 CHttp first-party compiler entries**, all with `-fsanitize=thread`, no
suppression or silent library fallback. The later changes concerned
private staged consumer packaging/workflow and Windows DLL deployment,
not the ServiceHost/CHttp execution-path implementation. The mixed-SDK
[diagnostic #38847793898](https://github.com/qigao/chttp/actions/runs/38847793898)
previously reported intermittent Worker/Owner buffer conflicts while
linking **uninstrumented** Salts allocator objects. Its evidence remains
preserved; full-source green does not categorically prove every possible
interleaving or that every mixed-SDK report was false.

The exact published Salts/SaltsUtils dependency six-RID compatibility
and GmSSL 3.2.0#12 toolchain were separately verified in
[CHttp preflight #38854251707](https://github.com/qigao/chttp/actions/runs/38854251707).
The official published `CHttp.Native` native build/pack also passed in
[#38854251731](https://github.com/qigao/chttp/actions/runs/38854251731),
with publication skipped. Windows v145 exact NuGet L2 package restoration
is proven by [vcpkg-cache #38852320111](https://github.com/qigao/vcpkg-cache/actions/runs/38852320111).
These independently qualified **published dependency** gates are not
evidence that a new public ServiceHost ABI has shipped.

Candidate mode must still have **no installation/export target** and cannot
modify the default public SDK, default runtime, or Provider ABI.

## Open before public or stable migration

1. Salts [#1153](https://github.com/qigao/salts/issues/1153): the
   **intentional breaking naming and ABI epoch** for
   `Salts::Configurator` / `Salts::ServiceHost`, including the
   `salts_service_host_scope.owner_address` layout, provider contract
   ownership, transitive static link dependencies and exact C11/C++17
   installed `find_package(Salts)` consumers across all six official
   native RIDs. This *private* candidate does not yet install/export.
2. Coordinate the final CHttp public Service MethodPlan consumer switch with
   an actual, deliberately exported Salts ServiceHost ABI. The currently
   accepted private code remains default **OFF**; do not ship two loaders,
   compatibility aliases or an implicit legacy runtime fallback.
3. Continue extended H2 connection-window/peer RESET/large-body and
   multi-Owner soak coverage beyond the currently tested 48 KiB responses
   and bounded per-request cancellation. Sanitizer GREEN is a tested
   schedule set, not a universal concurrency proof.

**Draft / DO NOT MERGE / DO NOT PUBLISH.** No public native SDK,
published package, master branch or production Service runtime is changed.
