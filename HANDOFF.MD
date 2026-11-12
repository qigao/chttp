# Split SDK migration handoff

- CI 43231727490 LLVM-MinGW failed Session finite-close at
  application_session_worker_test.c:468: the second 5000-ms close returned
  ETIMEDOUT while the entire case took about 1028 ms. Original log:
  `build/ci-43231727490-mingw.log`. Application had a stop-observation race:
  server_stop can return a caller wait timeout and then Server can publish a
  normal terminal before the subsequent stats query. Do not latch that caller
  wait timeout as a permanent shutdown error. Retain ownership and return the
  timeout; a later explicit close still drains normally. Preserve real nonzero
  Server terminal/join failures. Session and Plugin finite-close regressions
  also assert the Server operational terminal is OK after the wait timeout.
  Complete native build, all eight Application CTests and install pass in
  `migration-worker-plugin-close-race-{build,ctest,install}.log`.

- Private worker HTTP Plugin mounts now resolve the existing catalog/export at
  mount with the method record's existing registry lease. Plugin-backed RPC
  mounts retain their producer execution context instead of overwriting it with
  the ACE Controller; plain direct RPC still binds the canonical ACE receiver.
  Both use the same owned Application pool without a borrowed executor. Keep
  registry/metadata stable until joined close and provider-value cleanup;
  stopping new lease admission does not revoke an admitted method or permit
  early unload. Failed mounts must release acquired leases without consuming
  the route/method slot. No new loader or lease counter is needed.
- The formal generated Plugin fixture shares a test-only probe library with
  the host. Its business function calls that probe, so thread identity and
  barriers observe actual loaded DSO code, not a host replacement. Set/clear
  the borrowed probe only before start/after joined close. The App Plugin suite
  passes 10 cases/417 assertions: H1/H2 HTTP/RPC, context preservation, 400/422/
  500, serialization failure/reuse, failed resolution rollback, stopped-provider
  rejection, and real H2 cancellation of held HTTP/RPC with finite-close lease
  and Bean retention. Logs: `migration-worker-plugin-rpc-lifecycle-{build,ctest}.log`.
  Complete native graph, CTest 118/118 and install also pass:
  `migration-worker-plugin-{final-build,full-ctest,install}.log`.

- The private worker HTTP mount now admits synchronous typed CFlow projections
  with no borrowed executor. Mount retains the existing semantic/effect/ABI
  admission and compiles one immutable Plan; worker input binding, evaluation
  and serialization use one Application task. The shared CFlow evaluator keeps
  owned result bytes live until serialization, then destroys them before the
  native input frame. The legacy engine mode still requires its explicit
  executor. Do not add a nested post/wait, owner-thread fallback or interpreter
  fallback. Formal Application tests cover H1/H2 success/400/422/500, invalid
  projection rollback, rejection of an external executor, and cancellation with
  finite close/Bean retention on a one-worker pool. Native full graph and focused
  binding suite pass in `migration-worker-cflow-{build,focused}.log`. Full native
  CTest 117/117 and install pass in `migration-worker-cflow-{full-ctest,install}.log`;
  subsequent test-only serialization-capacity failure/reuse coverage passes in
  `migration-worker-cflow-output-{build,ctest}.log`.

- Private worker Sessions now use a charged detached snapshot/intent context.
  Prefix alignment derives from the actual context/header/param union. The
  selected MSVC C headers do not provide max_align_t (CI 43225024633, preserved
  in build/ci-43225024633-msvc.log); do not guess an alignment, select a different
  compiler or add a platform fallback to conceal that compile failure.
  Never let a live native Session escape to a worker or simply remove the public
  defer guard. The private defer entry validates and attaches the owned context;
  the public deferred API keeps rejecting live Sessions. Cookie headers belong
  to the deferred response builder, not the sealed original builder. Reserve
  cookie storage and validate the pinned original generation before ordinary
  store mutations. Reject owner-mutated contexts rather than dropping changes.
- Same-Session waiters occupy the existing invocation slots, not worker threads.
  Refresh after the predecessor's business-finalizer/owner-commit join; release
  the gate before native wire completion so H2 credit stalls cannot block later
  Session business work. Pins prevent expiry/reuse until gate retirement. Lock
  order at retirement is worker -> Session; owner commit releases Session before
  signaling the worker join. Canceled waiters settle without entering user code;
  stale queued generations return 409 without Set-Cookie or method invocation.
- Ordinary failed/canceled writes are discarded. Explicit invalidation is a
  fail-closed security transition: login helpers can invalidate after partial
  staging, and logout cannot leave authentication active when the response fails.
  A live owner expires the cookie even on business error; framework retirement
  performs store-only invalidation cleanup after transport/header failure, under
  the Session mutex and only for the admitted original generation. Never erase a
  newer identity or fabricate transport success. This is the existing security
  helper contract, not a retry/recovery of user business code.
- Session acceptance uses the formal App/Server suites: H1/H2 cookie flags,
  CSRF/login/logout, ID/token rotation, FIFO lost-update protection, independent
  Session progress, stale generations, canceled queued/running work, finite close,
  bounded bytes/store/header failures and one-worker H2 flow-window backpressure.
  Native full build, 117/117 CTests and install pass in
  `migration-worker-session-security-final-{build,full-ctest,install}.log`.
  The subsequent test-only cookie-failure invalidation case is in
  `migration-worker-session-cookie-failure-{build,ctest}.log`. The phase stays
  private; public Invocation/admission, provider/hooks and stream/WS migration
  still precede the public worker-default switch.

- OpenSSL negative mTLS qualification must use actual local TLS terminal
  evidence, not require the word "alert" in peer output. CI 43211623347 MSVC
  rejected the wrong-CA connection with `read:errno=113` and no HTTP response,
  so the old text assertion failed (`build/ci-43211623347-msvc.log`). Server now
  retains only an owned numeric failure snapshot/count under its existing stats
  mutex when CNet reports FAILED with a non-NONE TLS diagnostic. The existing
  OpenSSL fixture waits on that event and verifies one new failure, nonzero TLS
  status/reason, nonzero client exit and no extra HTTP request for each missing
  certificate/wrong CA/wrong EKU case. An arbitrary transport reset cannot pass.
  Provider HANDSHAKE or CHAIN_SIGNATURE categories must not be guessed into a
  finer certificate diagnosis; the first overly-specific local assertions are
  preserved in `migration-finish-prs-tls-{evidence-headers,local-category}-ctest.log`.
  Correct local TLS-stage qualification passes in
  `migration-finish-prs-tls-terminal-ctest.log`. No TLS verifier, security policy,
  public ABI, timeout, retry or fallback was changed.

- The intermittent H1 asset timeout remains open as
  https://github.com/qigao/chttp/issues/290; later green runs are not evidence
  of its root cause or resolution. Preserve the failure-boundary diagnostics.

- Private generated RPC envelopes now execute on the same Application-owned
  worker pool as handwritten/direct HTTP methods. Server composition uses an
  immutable dispatch adapter and synchronous evaluator/output callback, without
  depending on App/CFlow scheduling. Batch parsing, bounded aggregation and
  sequential notification/method behavior share the existing engine evaluator.
  A NULL response content type is preserved for empty 204 notification replies.
  H1/H2 typed calls, positional/named mixed batches, unknown methods, parse errors
  and notification-only batches pass in the Application binding suite:
  `migration-application-worker-rpc-{build,ctest}.log`. Per-method hooks and
  remaining providers/Session/stream/WS/public Invocation migration are pending.
  Complete native graph, 116/116 CTests and install pass in
  `migration-application-worker-rpc-{full-ctest,install}.log`; the subsequent
  test-only canceled-running-RPC case passes in
  `migration-application-worker-rpc-cancel-{build,ctest}.log`.

- Private generated direct HTTP mounts now use the Application-owned pool for
  bind/validate/invoke/serialize as one task. Share the existing MethodPlan
  evaluator through an internal output adapter, retaining the engine's existing
  buffer publication while the worker copies its draft before native/output
  cleanup. Do not post another task or wait on the same pool. Hold the method
  record and ACE receiver until terminal task finalization, not just method
  return. H1/H2 success, 422, 400 and canceled-running-method tests pass:
  `migration-application-worker-generated-{build,ctest}.log`. This remains private;
  generated hook outcome mapping, CFlow/Plugin, RPC and Session/stream/WS paths
  are not yet switched to the new model.
  Complete native graph and install pass after adding failure-boundary
  diagnostics: `migration-application-worker-generated-diagnostic-{full-ctest,install}.log`
  (116/116). The preceding full run had one existing H1 asset case return
  ETIMEDOUT (`migration-application-worker-generated-full-ctest.log`,
  application_test.c:441). The helper previously combined request and close
  errors without identifying the failing boundary. It now reports request
  stage/native error and close status/Server counters on failure. Both the
  diagnostic focused run and full run pass, but the original timeout's cause
  remains unproven; do not call it fixed or erase its failure record.

- Application shutdown must drain its owned workers before destroying Server
  storage. A handler deadline/reset can settle transport while a running method
  still holds a deferred handle whose target lives in Server storage. Stop the
  Server, join workers/observers with the remaining budget, then destroy DTO and
  Server state, and finally release the ACE graph. A held H2 method after its
  real handler deadline tests finite-close retention and safe late publication:
  `migration-application-worker-server-lifetime-{build,ctest}.log`.

- H2 TLS client test peer must not close after flushing a new native send from
  a preceding send callback. `proto_want_write == false` only means the encoder
  was drained; require `send_active == false` before closing. Otherwise response
  bytes may be canceled behind a SETTINGS send. macOS CI 43207921894 failed
  `chttp_h2_client_test.c:2027` with request ESHUTDOWN despite server OK;
  original log is `build/ci-43207921894-macos.log`. No timeout increase, retry,
  TLS policy change or production fallback is involved.

- Worker migration foundation is private: Application still uses its published
  dispatch model until the complete Invocation/Session/stream/WS migration.
  Deep-copy parser-backed headers, params, JWT strings/times, peer and body before
  a queued worker can outlive the callback. After H1 deadline/H2 reset, keep this
  storage until the real worker/finalizer settles; stale deferred publication
  cannot release it early. The private bridge now joins business finalization
  with the real native response terminal, then runs completion/finalization on
  its worker. The private Application prototype resolves real ACE Controller
  receivers and retains the graph until worker observers settle. Its private
  mounts cannot mix owner-affine legacy callbacks. No public mode is exposed.
  The worker suite covers 21 cases including two workers/two owners; Application
  tests cover injected Service execution and finite-close graph retention. Full
  native-release-user build, 116/116 CTests and install pass; logs are
  `build/migration-application-work-{late-build,late-ctest,full-ctest,install}.log`.
  The subsequent terminal/ACE implementation also passed 116/116 and install:
  `build/migration-application-worker-final-{full-ctest,install}.log`; added
  parallel and RST(NO_ERROR) cases pass their existing CTest entries in
  `migration-application-worker-parallel-ctest.log` and
  `migration-application-worker-rst-ctest.log`.
- H2 protocol stream closure can precede native send completion. Keep terminal
  tickets in ready/writing batches and settle only the batch NativeIO confirms;
  connection failure/reset cancels outstanding tickets. RST_STREAM(NO_ERROR)
  is cancellation despite its zero protocol error. Clear ticket ownership
  before signaling because a worker may immediately finalize its storage.
  A paused H1 client body sink does not prove native send remains pending:
  kernel buffers may accept the whole response. An initial test made that
  invalid inference and failed (`migration-application-work-completion-fixed-ctest.log`).
  Replace that inference with deterministic H2 flow-window backpressure; keep
  H1's actual send/deadline/observer tests. Do not equate local send completion
  with peer application consumption or widen timing assertions to hide it.
- Worker drain checks known publication errors before waiting, including a zero
  (infinite) deadline, and preserves the original error and live ownership. An
  oversized reply rejected by the real engine while its completion observer is
  held verifies fail-fast close, retained payload and later complete cleanup:
  `migration-application-worker-failfast-{build,ctest}.log`. Publication failure
  must signal waiters; only signaling final slot release can otherwise strand
  a waiter behind a completion-publication invariant failure.

- Deferred CPU failure root: temporary Server tracing reproduced HTTP 503 with
  one worker-admission rejection, not a lost successful response. The benchmark
  received all 150 response bytes but waited for an ok body. Source/data show
  ordinary Base threadpool_try_submit maps Disruptor publisher CAS contention
  to ENOBUFS; observed queued=1/active=0/capacity=16. Formal Base four-producer
  bounded-outstanding regression fails expected OK/got -4027 and passes with
  its internal claim/copy/publish monitor fix: qigao/salts-base PR #15.
  Base 0.1.6 releases that fix. CMeta and Utils bind producer dependencies
  exactly, so the matching composition is Base/CMeta 0.1.6 and Utils 0.1.3.
  Changing only Base fails fresh configure before generation
  (`migration-base-016-fresh.log`); preserve that evidence and exact checks.
  Restore original published SDKs into version-specific roots, update only
  the selected dependency fields in .env, and freshly configure the complete
  graph with BUILD_TESTS=ON and BUILD_BENCHMARK=ON. Do not edit installed
  Config.cmake files to bypass their dependency versions.
- Benchmark request diagnostics now distinguish EOF/receive/send failure and
  immediately reject non-200 status; worker rejection includes status and pool
  occupancy, and each repeat retains stderr beside raw JSONL. The old SDK's
  formal 128-request gate still fails as expected, now reporting HTTP 503
  directly (`migration-deferred-status-report-ctest.log`), without timeout
  widening, retry or inline fallback. No CHttp runtime change was needed.
- Investigation used a separate temporary branch and the existing CTest entry.
  Traces/data: migration-deferred-close-trace-ctest.log,
  migration-deferred-http-status-ctest.log and
  migration-deferred-admission-trace-ctest.log. The diagnostic branch and Server
  tracing code are removed; a local build/deferred-close-investigation.patch
  preserves the non-product trace for audit. No temporary validation project,
  SDK substitution or source dependency fallback was introduced.

- Handler CPU benchmark migration: clock() did not provide the intended Windows
  CPU measurement. Reuse chttp_test_thread_cpu for separate handler-owner,
  client and bounded application-worker totals; exclude listener/main explicitly.
  Keep measured threads alive and reuse one calibration across A/B cases.
  Initial port passed three repetitions; calibrated revision 36791e8 failed on
  repetition 2, deferred/4 owners/1 ms without a detailed boundary diagnostic
  (`migration-handler-cpu-final-measurements.log`). Preserve its partial raw data.
  Added boundary diagnostics then passed all 90 points
  (`migration-handler-cpu-boundary-ctest.log`), but that alone does not identify
  or fix the earlier failure. That rerun was insufficient for acceptance;
  the later Base admission diagnosis above identifies the actual failure.
  Do not claim a performance gain or close #215 based on a passing rerun.
  First CI 43166936938 found the same client-completion boundary in both
  Windows profiles (deferred/4 owners/zero work), while macOS passed. Capture
  client phase/status/native error rather than infer a pool or transport fault.
  A local two-CPU process-affinity run passed all 90 points; it is diagnostic
  evidence, not proof of a fix. Linux failed before execution because the older
  WS benchmark included its POSIX CPU helper after the engine header. Move the
  helper first; benchmark-only builds also need enable_testing() when
  BUILD_BENCHMARK is ON. The existing preset's benchmark-only full build and
  all three formal benchmark CTests passed, 42.52s.


- Independent TLS qualification must observe handshake messages: default Python
  group selection is not proof of HelloRetryRequest, and select() on a process
  pipe is not portable to Windows. The Server-owned OpenSSL CTest selects groups
  explicitly and checks the RFC HRR random and outbound dummy CCS records.
  A bounded reader queue handles startup; checked process shutdown retains logs.
  Current published SDKs pass all four TLS 1.3 profiles, mandatory-client-cert
  negatives and verified-leaf pin/forged-header rejection. Full native-release-user
  build and 112/112 CTests passed (`migration-tls-openssl-final-full-ctest.log`).
  Independent OpenSSL 3.x is a host test executable, not a product TLS library;
  no old source SDK/port stack or qualification feature switch is restored.
  First CI 43162608036 failed before configure on Linux because PowerShell
  Get-Command returned both /usr/bin/openssl and /bin/openssl. Select the first
  PATH application with -TotalCount 1; do not concatenate discovery results.
  The original failures remain recorded and are not retried unchanged.

- Issue/PR disposition is recorded in `docs/ISSUE_PR_REVIEW.md`, refreshed
  after the 2026-11-12 triage against master a615444. Closed historical SDK and
  ServiceHost stacks are not current implementation or acceptance evidence.
- Base string reuse keeps JWT issuer/audience ownership private and optional.
  `tstr_dup(NULL)` returns an empty string, so guard optional copies explicitly
  and pair them with `tstr_free`. Borrowed `vstr` comparisons preserve exact
  claim case and ASCII-insensitive header/scheme matching. Updated master
  a615444 qualification passes full native-release-user build, 111/111 CTests
  and install (`build/migration-base-strings-current-*.log`). Earlier 103/103
  and 104/104 evidence remains historical; cross-platform qualification belongs
  to the updated PR head CI.
- Application policy ownership: the public init contract promised copied policy
  values, while Security/CORS/asset middleware still borrowed caller structures.
  Reusing those structures after init changed nosniff, CORS admission and asset
  routing; three real H1/H2 tests reproduce the mismatch without invalid-memory
  access (`migration-application-policy-copy-red-ctest.log`). Application now
  snapshots these fixed-size structures before graph activation and registers
  only its owned copies. Referenced strings/origin arrays/callback contexts remain
  explicitly borrowed through close. Sized policy storage is admitted before
  copying; invalid size fails before any Bean factory. Full published-SDK Windows
  build, 109/109 CTest and install pass
  (`migration-application-policy-snapshot-full-ctest.log`).

- Application policy ordering: a static asset/SPA middleware can complete the
  response without calling next. Registering it before the ACE Interceptor
  bypassed global application authorization. Four real H1/H2 regressions show
  rejected assets returning 200 and hooks never entering before the fix
  (`migration-application-assets-red-ctest.log`). Register Interceptors after
  security/CORS but before assets, so rejection publishes no file response;
  successful dispatch still unwinds hooks and rejection keeps security headers.
  Asset-only requests have NULL operation metadata. Focused asset tests and
  published-SDK full build, 109/109 CTest and install pass
  (`migration-application-policy-rpc-full-ctest.log`). The same formal App binding
  suite now proves typed H1/H2 RPC and HTTP share one ACE Controller, preserve
  operation metadata boundaries, enter/unwind Interceptors and destroy once.

- Reflected Application migration: route engine `user` now points to an action
  binding record, while body-open/body-close callbacks require the ACE-owned
  Controller receiver. Forward both callbacks through explicit adapters; passing
  them through unchanged sends the wrong object. Formal Application tests cover
  H2 streamed-body open/write/close receiver identity and exactly-once close.
  Deferred MethodPlan finalizers may still borrow Beans after response terminal;
  Application close must wait for the runtime's in-flight count before destroying
  the ACE graph. A real blocked native invocation verifies timeout retains the
  owner/Beans and later drain destroys them once. Published-SDK Windows full
  build, 105/105 CTest and install passed (`migration-application-final-*` logs);
  subsequent C11 scoped-owner cases passed the focused Application CTest
  (`migration-application-scoped-ctest.log`). No device or final CI claim here.
- OpenAPI UI was gated entirely by BUILD_EXAMPLES, so the default test graph
  did not compile its Controller or run its four HTTP regressions. Build its
  example target whenever BUILD_TESTS is enabled and register those tests in
  the module's formal suite. The expanded Windows graph passed 109/109 and
  install (`migration-application-ui-qualified-*`). UI configuration is an
  explicit configured ACE Component; its native adapters keep user/request/
  response argument order. CI's MSVC link found that a public DLL data symbol
  is unsuitable for C11 constant FunctionDecl metadata. Exchange descriptors
  now use static immutable metadata with CMeta's explicit semantic identity
  equality across translation units/DLLs, preserving canonical ABI admission.
  Full build and focused Application/UI runtime tests passed after that fix
  (`migration-application-static-metadata-*`, `migration-application-static-ui-*`).

- Public Application examples now share canonical Service/Controller/Interceptor
  declarations and use real ACE Interface dependency injection. Compile-only
  coverage missed that the C++ example did not flush its listener URL to a pipe;
  the new formal HTTP test timed out before obtaining that URL
  (`migration-application-examples-red-ctest.log`). Flush after publishing the
  bound port. Module-owned CTest entries run both actual public programs, check
  the injected answer, Interceptor header/rejection, request isolation, 404 and
  checked shutdown; startup/shutdown timeouts fail and clean up the child without
  retry. Full published-SDK Windows build, 111/111 CTest and install pass
  (`migration-application-examples-full-ctest.log`).

- Socket policy audit: HTTP async/blocking Client, Server accepted streams and
  standalone WebSocket already forward CNet options. H2 WebSocket Pool omitted
  both validation and application of `config.client.socket_options`; apply it
  after CNet initialization and before Manager/connection admission. Omitted
  policy remains unchanged. The module-owned socket test reads NODELAY and
  KEEPALIVE from the real H2 socket through CNet; two sessions share that policy.
  Original failures are in `build/migration-websocket-socket-red-fixture-ctest.log`;
  the fixed focused regression passes in `build/migration-websocket-socket-fixed-ctest.log`.
  Full published-SDK Windows build and CTest pass 104/104, recorded in
  `build/migration-websocket-socket-full-ctest.log`. ManagedDial/recovery remains
  separate #231 work: socket option forwarding does not authorize HTTP replay
  or prove protocol READY/old-request settlement.

- The existing working tree contained the shared-presets migration. It was
  preserved and extended; see `docs/SPLIT_SDK_MIGRATION.md` for dependency
  ownership, the explicit eight-profile matrix and rollback.
- Remote master advanced to `6c8567a350c91701cee276683c972376814417a5` during
  migration. Its HTTPS P-384/SHA-384/X.509 runtime regressions are preserved;
  22 fixed PEM inputs are module-owned and the cases run with BUILD_TESTS.
  The old exact-source CI is superseded, not the security regression coverage.
- Actual Windows LLVM-MinGW configure against published Base 0.1.4, CMeta 0.1.4,
  CNet 0.1.3 and Utils 0.1.0 fails because Utils omits required `IDL.cmake`.
  Original log: `build/migration-split-sdk-initial-configure.log`. CHttp build
  and runtime tests have not passed; do not report this migration complete.
- Utils packaging fix: qigao/salts-utils PR 755, branch `fix/sdk-idl-install`,
  commit `32608153766f255cca76b36b14e5d504a626f9e0`. Local product configure,
  build and installation succeeded into `salts-utils/build/sdk-idl-install-stage`;
  all 11 compiler templates and IDL.cmake are installed. Existing native CTests
  passed 188/188. Published Utils 0.1.1 now supplies the packaging fix. Its
  manifest records SDK profile, archive digest and producer run using the
  Utils release schema. CHttp configuration, the HTTP example IDL generation
  target and the thread CPU CTest passed after switching to the release and
  fixing the missing Ed25519 fixture and the `Salts::URI` target name.
- `vendor/cjwt` owns JWT/JWE and now builds its prior GmSSL/libecc primitives
  privately. The old SDK header dependency is removed. HTTP/S3 use the internal
  `chttp_digest` target. Full Windows LLVM-MinGW build and install pass, as do
  all 11 cjwt CTests and HTTP JWT/CSRF/WebSocket/S3 tests. Source provenance,
  license, ownership and failure contracts are in `vendor/cjwt/crypto/README.md`.
- Full CTest initially passed 100/103. Installing the existing pinned OpenAPI
  requirements resolves `generator_conformance`. The pool test now checks
  accepted cancellation through the write deadline and the eventual timeout
  plus resource release, matching CNet's observed-completion contract.
  Core 0.1.4 is restored from its published, verified SDK. The latest full build
  passes. The initial 102/103 result and H2 TLS failure are retained in
  `build/migration-crypto-final-full-ctest.log`.
- H2 TLS root cause was the fixture's thread ownership. It initialized the
  server CNet client on main but sent retained responses on its worker. CNet
  rejected the send with SALTS_EPERM. Assigning the poll return into the same
  status then hid that callback failure behind a later timeout. Initialize
  the client on the worker and preserve the callback's first error. Drain
  and join the worker before response assertions so a failed request cannot
  leave a thread borrowing its stack fixture. Published Core 0.1.4 now passes
  the complete Windows suite, 103/103, recorded in
  `build/migration-h2-owner-published-ctest.log`.
- Diagnosis used a temporary Core v0.1.4 source worktree with TLS size/status
  traces through the existing CHttp CTest. Product configuration was restored
  to the published SDK before final validation. Temporary source changes,
  worktree and investigation branches are removed after retaining their logs.
- Local dotenv credentials yielded HTTP 401 during restoration. The local task
  helper explicitly selected the existing gh login and preserved credentials.
  Committed CI uses its job token and the shared verified archive transport.
- `.env` now names the actual published SDKs restored into versioned roots.
  Its unrelated fields were compared and preserved. CMake does not load dotenv.
  Keep original failure logs and do not retry unchanged CI jobs.
- #231 READY qualification: `chttp_pool_lifecycle_test` now withholds the real
  H2 receive completion while continuing other NativeIO routing. TCP CONNECTED
  alone leaves the pool CONNECTING; consumed peer SETTINGS publishes READY.
  A pre-SETTINGS cancellation remains one ECANCELED result after late bytes,
  with its old request handle retired and no additional physical connection.
  The fixture matches native request slot/generation, owns the delayed
  completion through delivery/teardown and closes H2 as well as H1 before
  external stop. No product hook, ManagedDial or retry API was introduced.
  Published-SDK Windows build and full CTest pass 104/104; evidence:
  `build/migration-h2-ready-full-ctest.log`. #231 remains open.
- CI 42947933660 reported five TSan races in the io_uring WebSocket fixture:
  session capture was release-published but read without acquire, and JWT
  subject publication used a relaxed counter. Acquire the captured session,
  retire each connection before resetting its publication flag, and publish
  the subject with release/acquire. The published-SDK Windows build and
  WebSocket CTests pass; Linux TSan validation requires the new CI run.
- #231 explicit physical connection recovery uses published CNet ManagedDial;
  the default remains one attempt and failed HTTP/RPC/S3 requests are never
  replayed. H2 stages bounded owned headers until verified transport and peer
  SETTINGS, while retaining the existing body/source ownership contract.
- NativeIO can report generic EIO with a signed native connection-refused code;
  classify only explicit transport codes. Before verified TLS CONNECTED, an
  undiagnosed reset can mean rejected SNI and must remain permanent.
- A logical cancellation must not release a request lease while a retained
  NativeIO send is outstanding. Deliver the terminal result only after actual
  transport terminal, then recycle Manager state before another attempt.
  Episode timeout closes the real transport but reaches HTTP as ETIMEDOUT,
  rather than response EOF. Blocking/RPC absolute deadlines cap the initial
  episode and never restart on backoff. Design and rollback are recorded in
  `docs/CHTTP_CONNECTION_RECOVERY.md`.
- Published-SDK Windows LLVM-MinGW full build, install and CTest pass, 105/105:
  `build/migration-recovery-deadline-terminal-build.log`,
  `build/migration-recovery-final-full-ctest.log` and
  `build/migration-recovery-final-install.log`. Earlier failed logs are retained.
  Cross-platform and sanitizer qualification requires the feature CI run.
- Feature CI 42966502206 exposed four budget cases on Linux/macOS: fast refusal
  let CNet determine that the next backoff cannot fit the deadline, represented
  by next_attempt_ms=UINT64_MAX. Convert this exhausted episode into the owned
  HTTP timeout result, preserving native status, instead of returning it from
  poll before the request callback. A longer first-attempt budget plus a larger
  backoff now exercises this boundary on Windows too. Never mark idle restored
  H2 READY while the peer advertises zero stream capacity. Windows full CTest
  passes 105/105 (`build/migration-recovery-budget-preflight-ctest.log`);
  the failed first CI remains available and was not retried unchanged.
- #231 H2 replay boundary: the real protocol peer can reject the first stream
  with REFUSED_STREAM or send GOAWAY Last-Stream-ID=0 before a response.
  Under both default and explicit physical-recovery policies, the original GET
  returns one ECONNRESET/native REFUSED_STREAM; only a later explicit GET reaches
  the peer again. Exact paths/request/connection counts also verify healthy
  stream-session reuse versus GOAWAY replacement. Published-SDK Windows full
  build and `chttp_h2_client_test` pass; logs:
  `build/migration-h2-replay-boundary-build.log` and
  `build/migration-h2-replay-boundary-ctest.log`.
- #231 logical retry is opt-in per blocking call through chttp_request_retry;
  old HTTP/RPC/S3/file/async calls remain single attempts. Keep typed attempt
  evidence in the request owner and copy it during terminal callback before
  slot recycle. Diagnostic stage strings never prove remote execution safety.
  One owned Base-tstr/header/body snapshot and one absolute deadline survive
  all attempts. CNet retry gate and backoff helper authorize each new attempt
  only after old request/stream/payload settlement; byte budget is cumulative.
- A GOAWAY error must remain permanent for streams below Last-Stream-ID too:
  their later transport loss cannot erase that protocol failure or restart
  physical recovery. Partial H2 frame/header input conservatively disables
  replay if response attribution is incomplete. TLS identity/ALPN rejection
  still permits only one connection even with application idempotence enabled.
- Raw H2 failure fixtures must consume the test body and SETTINGS ACK before
  closing. Closing with unread DATA can create a native write reset instead
  of the intended observed-response-prefix boundary. Check queued speculative
  connections while CNet still owns Winsock initialization, before destroying
  the client (WSACleanup otherwise invalidates this raw-peer observation).
- Published-SDK native-release-user full build, CTest 105/105 and install pass:
  `build/migration-logical-retry-goaway-final-ctest.log` and
  `build/migration-logical-retry-goaway-final-install.log`. The final fixture
  change also passes chttp_h2_client_test:
  `build/migration-logical-retry-peer-drain-ctest.log`.
  API/ownership/errors/limitations and rollback: docs/CHTTP_LOGICAL_RETRY.md.
- #231 deterministic budget qualification: retry counters, fixed absolute
  deadline, byte credit and seeded backoff now live in the owner-local private
  chttp_retry_budget. prepare follows real attempt settlement; advance commits
  admission credit only once. Time is an explicit input, not a global clock
  override or runtime test hook. Formal chttp_retry_budget_test covers exact
  seeded waits, late/exhausted budgets, duplicate prepare/advance, method/protocol
  authorization, cancellation/authentication, zero-body bounds and overflow.
  Existing real-peer and retained-NativeIO regressions continue through CTest.
  Published-SDK Windows native-release-user full build and CTest pass 106/106:
  `build/migration-retry-clock-budget-build.log` and
  `build/migration-retry-clock-budget-ctest.log`.
- CI 42975102602 failed the old physical-attempt exhaustion assertion under
  TSan (sampled 1, expected 2), without a TSan race report. Sampling a live
  ManagedDial snapshot only between polls misses a fast final refusal/recycle
  completed within one poll. The test now records the snapshot in the actual
  state callback and forwards the original observer unchanged, retaining the
  exact two-attempt assertion. No retry, sleep, production hook or TinyTest
  modification is needed. Windows full build and focused recovery suite pass:
  `build/migration-retry-clock-attempt-observer-ctest.log`.

- Typed C++ Bean admission: ObjectRef cannot infer the allocation size or the
  native T behind an erased pointer. Bean accepted mismatched reflected native
  size/alignment and invoked a factory even when DataDesc was missing. Three
  formal runtime cases reproduce this without invalid reads
  (`migration-cpp-bean-admission-red-ctest.log`). Validate DataDesc and exact
  sizeof(T)/alignof(T) before factory side effects. Constructor constraints also
  require an exact std::unique_ptr<T> value result, preserving the default delete-T
  lifecycle instead of erasing custom deleter or derived-pointer semantics.
  Public constructibility assertions cover these type boundaries; normal aligned
  objects still construct and destroy once. Explicit ACE providers support other
  deletion models. Full published-SDK Windows build, 109/109 CTest and install
  passed (`migration-cpp-bean-admission-full-ctest.log`).
