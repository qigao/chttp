# HTTP ServiceHostCandidate migration — isolated conformance

Tracks [CHttp #224](https://github.com/qigao/chttp/issues/224) and
[Salts #1154](https://github.com/qigao/salts/issues/1154).

This is a CHttp build-only, branch-scoped HTTP service consumer of the *exact*
Salts runtime source at `540e1da7c2448f15f04aeaaee1106f73a2fbcc7e`.
Default production builds remain unaffected. No public CHttp or Salts SDK ABI
has been migrated or published.

- `CHTTP_ENABLE_SERVICE_HOST_CANDIDATE=OFF` by default.
- The opt-in build consumes original Salts `component-runtime/` CMake targets
  from a pinned checkout, without a copied runtime or new DSO loader.
- `chttp_service_mount_host_candidate()` is a **private** CHttp mount-time
  adapter. Its mounted method record owns the *nonmoving* ServiceHost Scope.
  It caches the provider's typed native binding and execution before HTTP route
  publication; no per-request Component/Plugin search is added.
- Deferred task finalization completes native-value cleanup before the method
  lifetime releases its scope. Domain stop/drain still belongs to Salts.
- Real generated Provider DSOs exercise rejected component lookup, N/N+1
  publication, distinct responses from two DSOs, BUSY drain, and stop/unload.
  The migrated historical fixture uses **two HTTP Servers**; it does **not**
  prove one live listener with atomic N/N+1 route dispatch.
- CI verifies the exact Salts source SHA and published SDK pair; no fallback.
  Release and ASan/UBSan runs here are **not** a complete first-party
  cross-repository source-instrumented TSan proof.

Follow-up gates remain: a single-listener generation selection/fencing model,
H1/H2 interleaving on one host, resource capacity/Owner concurrency, installed
candidate SDK ABI review, and true full-source TSan for the new linked closure.
No merge or publication authorized by this conformance branch.
