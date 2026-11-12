# Issue and PR integration review

Snapshot: 2026-11-12, master `a615444ea31093a06d470dd3a942221a0315595a`,
after #272 and #280–#283. This records source/queue triage, not a complete
line-by-line audit of every historical branch or downstream deployment.
Original issue and PR descriptions remain available on GitHub.

## Closed historical work

Fourteen historical PRs were closed without merging their old stacks:
#225, #233, #234, #236, #249, #252, #253, #254, #255, #256, #257, #258,
#259 and #270. The former Salts source/candidate, port and SDK/cache solutions
are superseded by exact published split SDKs and the Application migration.
Their branches and original descriptions were retained. Closing these stacks
does not prove ServiceHost generations, downstream consumers or release gates.

| Issue | Disposition and evidence |
| --- | --- |
| #269 | Closed not planned: source-only consumption conflicts with the later authorized published-SDK contract in #272. Real downstream acceptance remains #244. |
| #240 | Closed not planned: the old public Server/RpcService packaging plan is superseded by the #280 Application entry point. Residual release gates remain #244. |
| #241 | Completed: Server-owned RPC registry/mounts share the HTTP listener and Owner; registration is now private behind Application. |
| #242 | Completed: typed blocking/async RPC uses the native Client HTTP transport, without a nested client or pool. |
| #243 | Completed: bounded per-call frames, params and workspaces replace shared mutable RPC storage; formal tests cover capacity, partial decode, owned values, provider leases and drain. |
| #191 | Closed duplicate: remaining generic DSO qualification is tracked by #194; its old Server/no-CMeta restriction is superseded by #280. |

## Remaining issues

Priorities and risks describe the remaining work, not all historical checkboxes.

| Priority | Issue | Residual acceptance |
| --- | --- | --- |
| P1 / MED | [#231](https://github.com/qigao/chttp/issues/231) | Deterministic hung/EOF/late-terminal diagnosis and drain qualification. `chttp_requests_recover(impl, UINT32_MAX)` is not bounded by the logical deadline; the historical WHEP delay has no confirmed CHttp root cause. Preserve retained-send ownership and default single-attempt behavior. |
| P1 / MED | [#194](https://github.com/qigao/chttp/issues/194) | Real host/provider DSO generic metadata identity, mismatch admission and finalizer-before-unload tests. Cross-TU and ordinary plugin tests do not establish this. |
| P1 / MED | [#224](https://github.com/qigao/chttp/issues/224) | Retained ServiceHost generations/Scope and bounded replacement/rollback on current public CMeta::ACE. The static Application graph is not dynamic hot replacement. Public interface design needs review before implementation. |
| P1 / MED | [#244](https://github.com/qigao/chttp/issues/244) | Current SDK artifacts/exports and real existing downstream HTTP/RPC/TLS qualification, with exact versions/platform evidence. No independent consumer verification project. |
| P2 / MED | [#229](https://github.com/qigao/chttp/issues/229) | Existing CNet pool protocol leases and identity isolation need the remaining TLS/authority/ALPN/generation/Owner and failure-path matrix; do not rewrite the implemented pool. |
| P2 / LOW | [#268](https://github.com/qigao/chttp/issues/268) | A bounded curl-like Client CLI with strict options, HTTPS identity, atomic file output and formal runtime/exit tests is not delivered. |
| P2 / MED | [#215](https://github.com/qigao/chttp/issues/215) | Port deferred CPU A/B benchmark and collect current raw owner/client/worker CPU and admission data. Old inline evidence is limited to its recorded revision. |
| P3 / LOW | [#230](https://github.com/qigao/chttp/issues/230) | Explicit host-driven Server/Client cohosting needs a named consumer and lifetime/progress design. Ordinary threaded Server plus polled Client is not cohosting. |

## Retained PRs

All four retained PRs were draft after triage. Old-head green runs do not
qualify a new baseline or authorize a production/performance claim.

| PR | Next acceptance |
| --- | --- |
| [#273](https://github.com/qigao/chttp/pull/273) | Preserve JWT/WS Base string reuse and option-lifetime/case regressions, merge current master, retain both HANDOFF records, then qualify the resulting head. |
| [#263](https://github.com/qigao/chttp/pull/263) | Port unique OpenSSL TLS 1.3 mTLS HRR/CCS and negative-identity tests onto current published SDKs; do not restore the old port/source workflow. |
| [#271](https://github.com/qigao/chttp/pull/271) | Extract the bounded large-array H2 RPC test from the closed #270 parent stack, port to formal Server tests and qualify 32 KiB success/8 KiB rejection. |
| [#219](https://github.com/qigao/chttp/pull/219) | Port the deferred CPU benchmark to current Base APIs and private Server benchmark contracts; use formal CTest and retain raw repeated measurements. |

## Verification boundary

The #280–#283 native CI covered Windows/macOS and Linux including TSan.
Combined #281–#283 qualification on published SDKs passed the full local
`native-release-user` build, 111/111 CTests and install. Android records establish
cross-build/install only. Queue triage itself did not execute new tests or
publish a release. #273 qualification after this baseline update is recorded
in its PR; no old run is relabeled as a new-head result.

## Base string reuse

The accompanying implementation uses private `tstr` storage for the JWT
validator's optional issuer/audience copies and `vstr` for borrowed comparisons
in JWT and WebSocket client/pool validation. Existing S3 builders and HTTP
request/handshake code already use Base strings and retain their hard limits.

The validator owns its copies from successful initialization through destroy;
partial allocation failure releases every owned string. NULL disables a
restriction and is preserved explicitly because `tstr_dup(NULL)` creates an
empty string. Claims compare case-sensitively; header names and Bearer schemes
use ASCII-insensitive comparison. Views are local borrows, retained by no
callback or queue. WebSocket NULL handling, header validation, wire buffers,
capacity limits, public layouts and error codes stay unchanged. No new shared
state, retry, unbounded builder or runtime owner is introduced.
