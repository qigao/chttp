# Issue and PR integration review

Snapshot: 2026-11-11, master `5098dc8ad6572874b0c8b71d36ddf090a739d324`,
after [#272](https://github.com/qigao/chttp/pull/272). Reviewed the 14 open
issues, the descriptions and branch ancestry/diff scope of 17 open PRs,
and the current implementations cited below. This is an integration-readiness
review, not a complete line-by-line audit of every historical draft.

## Findings

| Severity | Evidence and trigger | Impact and minimum next step |
| --- | --- | --- |
| MED | PRs [#233](https://github.com/qigao/chttp/pull/233), [#234](https://github.com/qigao/chttp/pull/234), [#236](https://github.com/qigao/chttp/pull/236) have heads that are already ancestors of master; their three-dot diffs are empty. | Their open status no longer represents outstanding changes. They are closure candidates; do not merge an old CI stack again. |
| MED | [#270](https://github.com/qigao/chttp/pull/270) changes eight legacy SDK/cache files and pins vcpkg-cache 0.1.0. Master now uses environment-selected 0.1.3 with the split SDK matrix. | The original pinning solution is superseded by #272. Close as superseded or extract a demonstrably missing requirement, without restoring the old workflows. |
| MED | [#219](https://github.com/qigao/chttp/pull/219) still calls `cmeta_threadpool_*` and `cmeta_thread_*` in its deferred benchmark. Current Base owns these APIs under the new names. | Rebase the benchmark onto current published inputs and qualify it through its formal CTest entry before merging or closing #215. No new performance conclusion follows from this review. |
| MED | [#271](https://github.com/qigao/chttp/pull/271) contains the large H2 RPC regression together with its inherited #270 cache changes. | Preserve the useful bounded 32 KiB/8 KiB test, but port just that test onto current master and requalify it. Old-head green runs do not qualify the current SDK composition. |
| MED | [#269](https://github.com/qigao/chttp/issues/269) still asks for master/source consumption and removal of mandatory installed SDKs. The later authorized design in #272 deliberately requires verified published SDKs. | Reconcile the issue's acceptance text with the chosen dependency contract before closure; do not reintroduce source fallback to satisfy stale text. |

## Remaining PRs

Keep [#263](https://github.com/qigao/chttp/pull/263) as a reference-only TLS
qualification draft. Its old-source/port14 evidence does not establish an
outstanding defect in the current published dependency graph.

The ServiceHost/Component draft stack remains unmerged:
[#225](https://github.com/qigao/chttp/pull/225),
[#249](https://github.com/qigao/chttp/pull/249),
[#252](https://github.com/qigao/chttp/pull/252),
[#253](https://github.com/qigao/chttp/pull/253),
[#255](https://github.com/qigao/chttp/pull/255),
[#256](https://github.com/qigao/chttp/pull/256),
[#257](https://github.com/qigao/chttp/pull/257),
[#258](https://github.com/qigao/chttp/pull/258),
[#259](https://github.com/qigao/chttp/pull/259).
Their descriptions explicitly restrict merging or publication and use former
Salts/Utils epochs, candidate source builds, or installed qualification projects.
Any future production slice must use the current public CMeta/Base/Utils
contracts and existing module tests. This review does not authorize their
public API changes or reverse their draft-only instructions.

[#254](https://github.com/qigao/chttp/pull/254) modifies the removed legacy SDK
preflight workflow. Its exact-package GmSSL preflight needs reevaluation against
the new restoration receipts rather than restoration of that workflow.

## Issue disposition

| Issues | Evidence in current master and disposition |
| --- | --- |
| #240, #241, #242, #243, #244 | Server/Client-owned RPC exists in `http_server/rpc/`, `http_client/rpc/` and their public headers. Server call slots own bounded frame storage; Client RPC uses the existing HTTP transport. `docs/RPC.md` records ownership/drain coverage. The #272 matrix passes the formal regressions, but a newly published CHttp SDK/export qualification was not established here. Reconcile completed source work with remaining release acceptance before closing the epic/SDK issue. |
| #229 | `chttp_client_impl` already contains one CNet Manager and `cnet_client_pool`, separate H1/H2 sessions, request slots and `cnet_pool_lease` values. Pool release/bind-ready is implemented in `http_client/src/chttp_client.c`. Reconcile its old implementation premise with current coverage; do not rebuild a second pool. |
| #230 | SG-hosted external progression/cohosting remains a separate capability. Published-SDK migration alone is not its acceptance evidence. Keep open. |
| #231 | Protocol-ready admission and optional recovery/retry require separate contract qualification. Ordinary connection reuse is not proof of retry authorization. Keep open. |
| #268 | Native curl-like CLI is not supplied by the dependency migration or this string refactor. Keep open. |
| #215 | Inline owner-blocking evidence and deferred A/B benchmark acceptance are distinct; #219 needs rebasing. Keep open. |
| #224 | Component request-scope/lifetime qualification is not proven by the historical ServiceHost drafts or current SDK migration. Keep open. |
| #194 | `app/tests/service/chttp_service_generic_cross_tu_test.c` proves the cross-TU slice; it does not alone prove all cross-DSO generic graph lifetime requirements. Keep open. |
| #191 | Architectural alignment spans canonical IDL, native reflection and transport plans. Old RpcService/package names need reconciliation with integrated Server/Client RPC; no claim of complete compiler alignment. Keep open. |
| #269 | Dependency cutover is implemented by #272, but its original source-only criterion conflicts with the later published-SDK decision. Update acceptance before closure. |

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

Formal JWT regressions cover independent option storage, each optional
restriction, exact claim case, mixed-case authentication, duplicate headers,
invalid separators, non-ASCII schemes and empty tokens. The published-SDK
Windows full build and CTest pass 103/103. No performance gain is claimed.

No issues or historical PRs were closed or merged by this review.
