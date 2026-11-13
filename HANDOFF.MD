# Split SDK migration handoff

- Private handwritten Controller actions now use an exact callback-scoped
  `int(void *, const chttp_invocation *)` contract with canonical
  `chttp.invocation` identity. Mount admits FunctionAbi before route publication,
  rejects the old Exchange despite its identical pointer carrier, resolves hook
  Beans canonically and copies typed hook rows. Thin adapters reuse the existing
  worker ACE chain and terminal join; the raw handwritten worker signature was
  replaced, without adding an installed parallel API or public default mode.
- Invocation borrows the owned request snapshot and bounded copied draft. Its
  request view exposes no Session pointer; explicit Session access returns only
  the detached intent transaction. Cancellation reads the existing synchronized
  failed native-terminal state. Handles/views cannot escape their callback.
  Record the actual callback thread token, not only executor membership: another
  worker on the same pool must not read/write a foreign Invocation. The formal
  two-worker overlapping-scope test exercises this distinction.
- C++17 private worker declarations require extern-C linkage to the C library.
  The typed action guard unwinds C++ locals and translates exceptions before
  crossing C worker frames. TinyTest's function-like `context(...)` macro also
  expands constructor member initializers; use a distinct member name, without
  changing TinyTest. The public with-helper starts before its body, so private
  pre-start mount fixtures initialize/mount/start explicitly and close at C++
  scope exit; do not change that public lifecycle as a test workaround.
- Validation: native-release-user complete graph, 120/120 formal CTests and
  install pass in `migration-worker-invocation-{final-build,final-full-ctest,final-install}.log`.
  New Invocation suite: 18 cases / 243 assertions; C++ suite: 10 cases / 58
  assertions. Coverage includes H1/H2 binary snapshot/draft ownership, hook
  ordering/rejection/failure, unsealed/overflow results, copied metadata rows,
  ABI/Bean admission rollback, foreign and same-pool thread rejection, observed
  cancellation, C++ exception/RAII and existing Session/CSRF/principal gates.
  Earlier failed local fixture logs remain preserved. Feature CI must still
  qualify the commit; Android device execution and open #290 are not claimed.

- Private generated HTTP/RPC after_completion now joins the existing App
  business/Session/native-terminal notification. Admission reserves one HTTP
  record or RPC max_batch_items records and charges checked bytes before defer.
  A bounded proxy tracks entered hooks while the SDK ACE chain owns ordering;
  completion visits entered hooks backward, RPC methods in batch order, on the
  same worker pool. Invalid ingress/unknown methods have no entered notification.
- Completion retains frozen method/native/binding/interceptor/response-adapter
  facts separately from request business/publication/Session/transport status.
  DTO/frame/Plan results and RPC parser views are already destroyed. Only
  callback-borrowed copied HTTP/const Session views and pinned canonical metadata
  are exposed. Canceled running business and blocked observers retain records,
  Beans and real DSO leases through finite close; no additional task per method,
  unbounded list, owner execution fallback or installed ABI was introduced.
- RPC's per-call method record is a stack copy. Its copied hook array must never
  escape into completion storage: borrow the immutable registered hook array
  explicitly, and finalize response-adapter status after native lifetime restore.
  Keep shared inline helpers valid C++17 as well as C11 (explicit void* cast;
  ordinary aggregate initialization instead of C compound literals).
- Validation: native-release-user complete graph, 119/119 formal CTests and
  install pass in `migration-worker-method-terminal-{final-build,final-full-ctest,final-install}.log`.
  Output/completion suite: 46 cases / 2039 assertions, including H1/H2 direct/
  CFlow/real DSO, completion-only hooks, reverse entered ordering/rejection,
  batch/notification identity, ingress suppression, native/staging failure,
  cancellation, blocked-observer lease retention and 1 MiB H2 window backpressure
  while the sole worker serves another method. Worker suite also proves charged
  completion reservation and arithmetic-overflow rejection before business/defer.
  Cross-platform qualification remains the feature PR's CI responsibility;
  Android device execution and resolution of open #290 are not claimed.
  Final shared-header guards also pass the complete build, all nine Application
  CTests and install in `migration-worker-method-terminal-qualified-{build,ctest,install}.log`.

- Private generated HTTP/RPC method hooks now reuse one scheduler-neutral ACE
  chain. Application resolves canonical Bean contexts before publication and
  copies at most 16 rows into immutable method records. Bind first; enter before
  hooks forward, execute native/Plan + DataBind outcome staging, then unwind
  after/on_error backward before native/provider cleanup. CFlow output remains
  live through unwinding. Call frames and HTTP/const Session views are borrowed
  read-only; the request copy exposes no mutable Session handle. Session writes
  remain with the enclosing request transaction. No reflection lookup, scheduler
  hop or per-call hook allocation is introduced. Old engine paths reject hook
  mounts unless an explicit composition adapter owns execution.
- Explicit rejection maps to HTTP 403 / RPC -32003 and skips the method. Unknown
  methods/invalid ingress enter no method hooks; notifications execute hooks
  while suppressing response items. Native status, invocation/staging failure
  and rejection have separate facts. Timeout does not unwind a still-running
  method; business success after wire cancellation still unwinds success once.
  Per-method response-terminal observers now use the bounded private join above;
  the public Invocation replacement remains separate work.
- Private generated worker mounts now take their matching MethodPlan codec
  explicitly and compile bounded JSON FormatPlans plus native MessagePlan
  admission before route publication. Plans own copied schema facts; the codec
  may be freed before listener start. Required non-nullable record values and
  structured typed errors are supported. Scalar legacy engine mounts keep
  their existing behavior. Successful Fault staging maps to HTTP 409 / RPC
  -32077; failed staging still returns 500 / -32603 with outcome NONE.
- DataBind's canonical writer rewrites only root keys. Nested field-name
  rewriting and optional/nullable native state are rejected at mount, rather
  than inferred or silently omitted. JSON RPC reuses the built-in provider's
  bounded byte document and reader so uint64 and escaped/embedded-NUL text
  survive replay into envelopes. Initialize every FormatReader lease with
  DATA_BIND_FORMAT_READER_INIT before close, including scalar/abort paths.
  Readers close before backing buffers; FormatPlans free before Plugin leases.
- Structured-output validation: native-release-user complete graph, 119/119
  formal CTests and install pass in
  `migration-worker-structured-{final-build,final-full-ctest,final-install}.log`.
  The new module-owned output suite passes 25 cases / 1083 assertions across
  H1/H2 direct/CFlow/real DSO, exact uint64 and escaped/NUL text, typed mappings,
  exact/one-short capacity, abort/reuse, mixed batches/notifications, failed
  admission lease rollback and canceled running DSO/Bean drain. No Android
  device execution or resolution of open #290 is claimed.
- Native complete graph, CTest 118/118 and install pass in
  `migration-worker-method-hooks-{final-build,full-ctest,install}.log`. The initial
  focused run also recorded the open #290 H1 asset close timeout at
  application_test.c:453 (close ETIMEDOUT, pre-close active=1, terminal=OK):
  `migration-worker-method-hooks-regression-ctest.log`. The test now prints
  post-close ownership/running/stopping/active/terminal on failure. This feature
  does not fix #290; later green results do not establish its cause/resolution.
  Final status-propagation correction also passes full build, all eight
  Application CTests and install in
  `migration-worker-method-hooks-status-{build,ctest,install}.log` (binding
  suite 44 cases/894 assertions; real Plugin suite 10 cases/443 assertions).

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

- Worker response headers: the private Invocation writer now owns bounded copied
  fields through entered hook unwind and business/Session/native-terminal joins.
  Extra rows/arena are charged before defer. Content-Type uses one reserved row
  and the reply arena; normal fields retain Server case-insensitive upsert and
  append-only capacity semantics. Shared pure grammar rejects injection and
  reserved framing/WS fields before mutation. H1/H2 formal tests cover copying,
  exact/short/full budgets, MIME call order, foreign scope, failure/rejection,
  cancellation, managed Cookie precedence and failed Cookie reservation without
  ordinary Session commit; C++ exception/RAII tests cover the same writer.
  Published-SDK native-release-user full build, CTest 117/117 and install passed
  (migration-worker-header-qualified-ctest.log). The initial configuration-only
  case exposed an uninitialized-Server fixture teardown; it now has its own test
  group. Original failed test output is retained, with no TinyTest change.
- Preinstalled SDK lookup: tree-sitter-c was present in the selected vcpkg-cache
  profile; omitting its CMAKE_PREFIX_PATH caused the required OpenAPI find_library
  to fail. Follow SaltsBase/SaltsUtils: derive that prefix directly from existing
  VCPKG_INSTALLED_DIR/triplet/configuration inputs, and name each first-party
  package explicitly with the environment-selected version/root. Neither root
  configure nor the exported package clears dependency caches automatically;
  SDK/toolchain changes require explicit --fresh. Preserve MinGW explicit exports:
  linked static SDK export directives otherwise suppress implicit HTTP DLL
  exports and break S3 linkage. The final llvm-mingw-x64-release-user fresh
  configure/full build and native configure/full build/install passed
  (migration-env-direct-release-build.log and migration-env-direct-packages-install.log).
  Verification used an isolated full-repository worktree to preserve unrelated
  local manifest/version-check deletions. Logs are retained in the main build/.

- CI 43270230864 / Windows MSVC job 131365235518 built successfully and passed
  119/120 tests. generator_conformance timed out after 30 seconds in the Node
  ECMA-262 checker using fs.readFileSync(0), with no generator/schema error.
  The original test passed locally in 1.45 seconds, so the log alone does not
  establish whether Node startup or stdin handling stalled. The checker now
  consumes a closed temporary JSON file with stdin disabled, removing its pipe
  EOF dependency without changing the 30-second deadline, dialect or checks.
  Formal conformance includes an empty list, Unicode/escaped patterns beyond
  Windows argv capacity, and rejection of a malformed last pattern in that batch.
  native-release-user full build and generator/generator_conformance passed 2/2
  (migration-conformance-file-input-ctest.log). Original failure excerpt is kept
  at build/ci-43270230864-msvc-conformance-failure.log and in the linked CI job;
  the failed job was not rerun. Hosted MSVC qualification requires the new CI.

- Private worker file responses now copy path/MIME into the existing reply
  arena and publish a borrowed decision under the retained native-terminal
  lease. The native owner prepares the file with the existing asynchronous
  file runtime before Session commit. Preparation errors remain per-request
  publication outcomes; later read/wire failures use the native terminal.
  No entire-file buffering, worker-side native mutation or second I/O runtime.
  Raw response_file is covered; conditional serve_file/assets, admission and
  generic upload/source/SSE/WebSocket adapters remain separate migration work.
- Deferred builders previously lacked their connection binding, and H1/H2
  source callbacks selected only the ordinary builder. Formal file regressions
  exposed both assumptions. Each owner now holds its actual active source
  builder. H1 keeps its parser paused and terminal lease until EOF plus the
  last native send, including WAIT/empty/HEAD; H2 keeps its existing final
  END_STREAM terminal. Header publication is shared with buffered replies.
- Native close drains admitted requests. The first new finite-close test
  incorrectly expected an immediate successful close while H2 flow control
  was blocked (migration-worker-file-final-ctest.log: other 119 tests passed).
  Corrected formal tests verify timeout with live ownership, then drain after
  client progress. A separate client-sink cancellation verifies error terminal,
  file-I/O drain and Bean destruction. Missing-file and full Cookie capacity
  leave ordinary Session writes uncommitted; same-Session business can progress
  while a 1 MiB file response is flow-controlled with a 512-byte reply budget.
  After these test fixes and target validation, full native-release-user build,
  affected Invocation/Session suites (2/2) and install passed:
  migration-worker-file-drain-{build,ctest,install}.log. Earlier failed logs
  remain preserved. The PR's full matrix is the cross-platform qualification.

- Private pre-body admission now runs a canonical Bean callback on the same
  bounded CFlow pool/slot/byte budget as body Invocations. Header/params/JWT/peer
  views are copied; body and Session are absent. The value-only callback scope
  releases its snapshot/credit before its finalizer publishes the copied
  status/Retry-After decision. Native request storage remains generation-pinned
  until that producer's last access. JWT stays before admission; 100 Continue,
  body_open and user body delivery stay after allow. body_ms includes header
  queue/callback time. Ordinary H1/H2 only; public default and WS/streaming/assets
  migration remain separate. No extra pool, fallback, dependency or CMake flag.
- The new H2 queued/running timeout regression exposed generic deferred cleanup
  recycling a closed header gate before its producer completed (migration-worker-
  admission-cases-ctest.log). Header states now bypass response-defer cleanup;
  cancellation retains the slot through publication and finite-close failure.
- A raw two-request H1 pipeline exposed lost Connection: close on a zero-body
  request paused for admission: both file replies arrived with keep-alive and
  the peer timed out waiting for EOF (migration-worker-admission-wire-diagnostic-
  ctest.log). llhttp's actual message-complete event must capture framing before
  its flags change; the delayed wrapper only dispatches that captured event.
  Resume also preserves buffered pipelined input if the empty request immediately
  starts another deferred/retained response. Formal parser HTTP/1.0 and HTTP/1.1
  cases and the coalesced header/file-response pipeline cover this boundary.
- Formal C/C++ admission cases cover one shared slot, record/byte saturation,
  route/JWT snapshots, invalid decisions, Retry-After, no premature 100 Continue,
  queued cancellation, late completion, same-pool close rejection, ABI/Bean
  validation, exceptions and RAII. The initial byte-limit fixture allowed the
  smaller H2 header snapshot; it now uses an explicit 16-byte limit. Failed logs
  are retained. Full native-release-user build, 120/120 CTests (including the
  existing benchmarks) and install passed: migration-worker-admission-framing-
  build.log and migration-worker-admission-qualified-{ctest,install}.log.
- CI 43343061469 passed the other seven configurations but Linux TSan reported
  a real NativeIO close/wake race in the new running-header deadline/finite-close
  case. The defect is CHttp's lifecycle boundary: publication held server->mutex
  while waking, but owner cleanup stopped CNet outside that mutex and excluded
  wakers only during destroy. Native storage lifetime did not keep the readiness
  backend open. After protocol/deferred/file drain, owner cleanup now withdraws
  external wake ingress under that mutex before stop; in-flight wakes finish
  first, later wakes get ESHUTDOWN, and CNet storage stays owned through cleanup.
  Never hold the mutex through stop callbacks. The original failed job and its
  excerpt at build/ci-43343061469-tsan-admission-failure.log are preserved; no
  retry, sanitizer suppression, upstream SDK change or timeout extension.

- Private Invocation serve_file now shares the existing Server metadata/ETag/
  date/range planner. The address-stable plan owns generated strings and borrows
  other option strings only until synchronous consumption. Workers copy policy
  fields into their header arena, and path/MIME into reply_bytes. Shared upsert
  reservation preflights the complete unique field batch before any mutation.
  The native retained decision adds only a checked bounded offset/length; the
  existing file runtime opens that range before Session commit. No full-file
  buffering, new I/O runtime or URL mapping. Metadata failures are business
  errors; later open failures are publication outcomes. A 304 needs no open and
  can commit Session changes; failed open preserves the original Session state.
- The first full conditional-file run passed 119/120 CTests and reproduced a
  legacy H1 asset close timeout (migration-worker-conditional-qualified-ctest.log).
  Review found EOF assigning the source's original keep-alive decision over an
  existing close_after_write intent. The formal owner-state fixture seeds the
  exact CLOSE-pending state at EOF and tests real peer EOF without an idle-read
  deadline. It failed after 5 seconds before the fix, then passed when EOF was
  changed to preserve an existing close intent. Logs: migration-worker-conditional-
  close-intent-{red,fixed}-ctest.log. Earlier socket-backpressure fixture attempts
  did not establish the intended pending-send precondition on Windows and remain
  preserved as fixture failures. No production test hook, retry or timeout increase.
  The original #290 log lacks a full owner event trace, so an identical historical
  interleaving is not established by this deterministic state regression.
- Formal H1/H2 worker tests cover copied options, repeated requests, validators,
  date precedence, malformed/overflow/multipart Range handling, selected ranges,
  empty/HEAD/304/400/405/412/416, exact/exhausted budgets, transactional input
  rejection, after-hook ordering and C++ linkage. Session tests cover missing
  post-plan files, 304 without open, 1 MiB files with a 512-byte reply budget,
  nonzero-offset H1 transfer, H2 flow control, cancellation and finite-close drain.
  Full native-release-user build, 120/120 CTests and install passed after the fix:
  migration-worker-conditional-close-intent-fixed-build.log and
  migration-worker-conditional-final-{ctest,install}.log. Private migration only;
  asset mounting/classifiers/SPA, upload/source/SSE and WS remain separate work.

- Private asset/SPA mounting now uses the same bounded Invocation pool, request
  snapshot, detached Session and native-terminal lease as Controllers. The
  owner checks namespace boundaries only; workers share the legacy path/symlink/
  reparse selector and conditional-file planner. Reserved asset misses stay 404,
  SPA handles its own namespace, and unrelated paths continue route dispatch.
  One stable mount context survives finite-close timeout until final drain.
  Canonical Bean policy and entered interceptors run on workers. The policy
  returns status plus immutable/borrowed ETag output, copied before after hooks;
  C++ exceptions become EIO without consuming partial output. Legacy callbacks
  and raw user contexts are rejected by the private adapter. Public defaults
  remain unchanged; upload/source/SSE and WebSocket adapters remain separate.
- The first asset ABI tests rejected an invalid consumer const-char pointer
  descriptor (migration-worker-assets-cases-ctest.log). A POINTER identity must
  describe the pointer and match its const-qualified pointee identity; an ATOM
  identity on a pointer is invalid. Corrected the descriptor and added formal
  descriptor/FunctionAbi admission assertions. No upstream CMeta change or
  relaxed validation. Full native-release-user build, 120/120 CTests including
  benchmarks, and install passed: migration-worker-assets-qualified-*.log.
  Later Session and registration test additions rebuilt and passed the formal
  asset CTest: migration-worker-assets-session-{build,ctest}.log. Coverage includes
  H1/H2 conditional/range/HEAD/SPA, hook ordering and copied policy output, JWT,
  Session cookies/304 commit, two workers/owners, slot/byte/header exhaustion,
  policy exceptions/RAII, cancellation/finite close and transactional registration.

- Private Invocation source/SSE responses now retain one charged reply-arena
  chunk and the existing worker record through producer cleanup plus native
  terminal. Owner adapters request/copy only; user read/next/close and C++ unwind
  run serially on the shared CFlow pool after action/hook unwind. The internal
  positive WAIT is distinct from every negative user error and synchronous EOF.
  Owner wake uses the Server lifecycle mutex and remains valid through pool drain.
  Failed selection transfers no resource; HEAD, failed business, cancellation
  and failed publication still settle accepted source cleanup exactly once.
- The first formal source run exposed H2 resume placement: calling readiness
  from flush recursed through the same flush and attempted resume outside WAIT.
  Readiness now runs from owner deferred progress with an explicit stream-local
  WAIT bit; source descriptor/reset ownership remains unchanged. Preserve
  migration-worker-streams-cases-types-ctest.log (12 passed/4 failed), followed
  by migration-worker-streams-source-wait-ctest.log passing all 16 source cases.
  The later length fixture assumed H1 could revoke already received complete
  Content-Length bytes. Excess producer data instead fails the Server cleanup/
  native terminal; the client may already report its promised-length success.
  Formal tests assert both views without weakening the native error contract.
  Final native-release-user full build, 121/121 CTests (including benchmarks)
  and install passed: migration-worker-streams-source-qualified-*.log. Tests
  include larger-than-arena transfers, H1/H2 WAIT, known/unknown/empty lengths,
  SSE formatting, negative producer status, C++ exception/RAII, overload,
  cancellation and finite-close ownership. Upload and WebSocket remain ongoing.
- Source CI 43357112929 on Linux GCC/Clang exposed a cancellation-fixture race:
  the 4096-byte H2 response could finish its native send before the client's sink
  canceled. Client cancellation does not retroactively fail an already completed
  Server terminal. The producer now blocks after its first chunk until the
  client returns cancellation, guaranteeing EOF/END_STREAM has not completed.
  Original CI failures remain preserved; no production change or retry.
- Source/SSE PR #303 merged after CI 43357861421 passed all eight configurations
  and the aggregate at 2a3b150ca54fa8022a97f09f15c25be524c81274. Windows/macOS
  passed 121 CTests; Linux GCC/Clang/TSan passed 122. Merge 77cdf251 has the
  same tree c4175a7901edb462f7e9cf692577bcf76528e56f. Android is build/install only.

- Private upload mounting now resolves a reflected canonical Bean open method,
  body close/resource cleanup and final Invocation on the same bounded worker
  record. Header/JWT/params are copied before user open; one native-input-sized
  chunk plus ordinary reply/header budgets are reserved at admission. H1 pauses
  parser/read demand after a copied chunk. H2 withholds consumed DATA credit and
  uses its bounded native stream input for already received bytes. Worker open,
  writes and close are serial. Final dispatch adds a separately charged detached
  Session snapshot, preserving the existing per-Session gate without blocking
  workers. Multipart/CSRF/commit use the same web implementation on workers.
  Resource cleanup follows after_completion and the native terminal, including
  CORS bypass, failed final admission, cancellation and failed finite close.
  Public defaults and installed APIs remain unchanged; WebSocket remains ongoing.
- Initial upload tests found H1 parser writes bypassing the request-state WAIT
  bit and the saved-input/read-demand boundary (worker-upload-cases-state and
  worker-upload-h1-wait logs). H2 lifecycle additions then exposed owner-side
  consumption queuing WINDOW_UPDATE/response bytes without a subsequent flush;
  worker-upload-observed-cancel-ctest.log preserves those timeouts. Owner progress
  now flushes the concrete bytes instead of waiting for another inbound event.
  No timeout increase, retry, inline fallback or second pool.
- A canceled native response can precede the worker body-close finalizer. Such
  final business must skip Session queue admission, since its cancellation event
  has already passed. The formal two-worker test keeps another same-Session
  callback blocked, expires the upload during body close, then requires upload
  cleanup before releasing that unrelated holder. H1 client timeout alone does
  not prove observed cancellation while read credit is paused; the fixture uses
  the existing body/handler deadlines rather than assuming FIN was observed.
- Both existing parsers reject nonempty trailers. Retain that contract: remove
  an unnecessary second header-snapshot proposal, copy only final keep-alive
  after body callbacks settle, and propagate a buffered H1 continuation's actual
  400 status before closing. Coalesced chunk/terminator/Connection-close socket
  cases cover ordinary EOF and forbidden trailers. Middleware-bypassed buffered
  H1 replies now pause until their already registered native terminal settles,
  so upload cleanup reports actual write success rather than later connection
  reset. CORS, JWT, late Session exhaustion and C++ unwind have formal coverage.
- Final native-release-user full build, 122/122 CTests including benchmarks,
  and install passed: migration-worker-upload-final-*.log. An earlier overlapping
  build hit Windows' loaded-DLL write lock while the preceding CTest ran
  (worker-upload-canceled-session-build); wait for test quiescence before any
  further build in that directory. The complete validation above ran after that
  test process exited. No TinyTest, upstream SDK or dependency changes.
- Upload CI 43362263616 passed Linux GCC/Clang/TSan, Windows and Android, while
  macOS hit EPIPE in the unauthorized H1 client before reading the header-stage
  401: its unnecessary chunked body raced the Server's deliberate rejection/
  close. The JWT-before-resource fixture now sends an explicit known empty body
  when unauthorized, retaining strict 401/zero-open/zero-resource assertions.
  Its authorized control still streams data. Keep the original CI failure; no
  product change, retry or relaxation to accept transport errors as HTTP success.
- Upload PR #304 merged as f45ef6f3627d4fbf5e28ce3f9fd93f44e6fb426e after CI
  43363139443 passed all eight configurations and the aggregate at 306d808e.
  Candidate merge and qualified head share tree 8092c8b17249b98641c268e741757dd67d06fd59.
  Android remains build/install validation only.

- Private worker WS mounting now resolves canonical reflected opening/event
  methods and leases one shared Invocation record for the full peer lifetime.
  Opening explicitly accepts or returns a copied buffered denial; detached
  Session and ordinary Invocation hooks settle before native H1/H2 handshake.
  Event payload slots plus an independent Close slot are reserved/charged at
  admission. Events run serially per peer on the existing pool. Queue overflow
  fails that peer and drains previously accepted events before Close/completion.
  Owner callbacks only copy/signal bookkeeping; sends retain the existing
  bounded generation-checked command path. Native producer retirement is a
  separate join from transport terminal, preventing record reuse while peer
  reset/CNet callbacks still borrow it. Finite close retains blocked frames/Beans.
- WS header admission now composes with H1 Upgrade pause/resume and H2 Extended
  CONNECT. H1 pauses read demand for pending events; H2 queues bounded raw input
  and holds its credit until copied events settle. Partial native WS assembly
  returns credit without requiring a complete logical event. Worker errors reset
  only the H2 stream. Denials refund pre-handshake queued credit before response.
- The first WS case build used a nonexistent Function() reflection accessor;
  use FunctionAbi(action)->function. The first runtime fixture used brace-style
  route parameters; the actual router uses /ws/:id. Later raw-socket cases
  incorrectly initialized Windows' sin_addr union; use explicit address bytes,
  matching the existing upload fixture. Preserve their failed build/CTest logs.
- The 128 KiB H2 message regression exposed two existing Client progress gaps:
  send drain polled after its initial receive demand was exhausted, and receive
  waited for a complete WS event before flushing partial-message WINDOW_UPDATE.
  Keep receive demand during send drain and flush actual H2 control bytes while
  assembling an event. The original window-diagnostic and window-receive failures
  remain recorded; no timeout extension/retry/fallback. Temporary byte/status
  diagnostic output was removed after identifying the boundary.
- Opening cancellation fixtures must destroy a WS Client on the same thread
  that initialized/progressed it. The initial cross-thread cleanup returned
  EPERM. H2 graceful stop drains active streams, so a finite stop timeout alone
  does not prove opening cancellation; use the existing handler deadline and
  wait for the client's observed terminal before releasing the blocked method.
- Native full CTest run worker-ws-full was interrupted by the tool server restart
  while starting the dedicated WS benchmark (43/123); no test failure or final
  result was reported. No validation claim is based on that partial run.
- A FAILED CNet engine suppresses its Close event. The private worker adapter
  still owes an accepted peer one terminal event, so producer retirement emits
  an abnormal Close notification without wire output or replacing failure status.
  Malformed unmasked H1 frames exercise this boundary on ordinary/dedicated
  transport. Control/close capacities reuse CNet's published constants.
- The first completed full native run passed 121/123 CTests. Update the parser's
  former ENOTSUP assertion to verify gated Upgrade delivery/resume, and read WS
  route identity from the deferred target's canonical request_state. The latter
  avoids dereferencing a builder-only backpointer absent from the valid isolated
  deferred-token fixture. Preserve worker-ws-qualified-ctest.log with both failures.
- Review found a last-producer race: setting retired, unlocking and re-entering
  the completion join could let an event finalizer recycle the record before
  that last join. Retirement now publishes its flag and decides completion in
  one join critical section; all tail uses are captured owners/reserved stages.
- H1 protocol Close must finish even when the worker Close method is blocked.
  Owner progress now checks a fully drained native terminal before pausing user
  event credit. Raw ordinary/dedicated tests require physical EOF while that
  method remains blocked, then release it and join completion. The native
  producer can retire before the worker event, while the record/Bean stay live.
- A fully drained H1 WS Close now records its clean protocol decision before
  socket closure and reports success only from the actual successful transport
  terminal. An abnormal EOF can also make CNet CLOSED, so querying CLOSED after
  transport_closed is insufficient. H2 keeps the existing real-send observer:
  client END_STREAM receipt does not guarantee the Server already observed its
  local send completion before the client's subsequent TCP close. Preserve
  worker-ws-clean-terminal-cases.log with that fixture expectation failure.
- Final native-release-user full graph build, 123/123 CTests (including three
  benchmarks) and install passed: migration-worker-ws-release-{build,ctest,install}.log.
  The subsequent test-only two-owner cases rebuilt and passed all 37 WS cases:
  migration-worker-ws-multi-owner-{build,ctest}.log. The formal C++ suite includes
  six H1/H2 opening/event exception/RAII cases. Coverage also includes worker-only
  reflection, JWT/headers, Session gate release, raw early/coalesced input,
  capacity overflow, protocol failure, H2 sibling survival and 128 KiB windows,
  one-record reuse/stale handles, opening/event cancellation, blocked Close and
  finite-close ownership. Public defaults/installed interfaces remain unchanged.

## Private worker JSON body ingress

- Worker HTTP mounts now compile one required, non-nullable record body from
  the explicit MethodPlan codec/JSON format. The existing owned snapshot feeds
  the published JSON provider only while BindingPlan decodes/validates input;
  the reader closes before business hooks. Native DTOs and cached FormatPlans
  retire before their Plugin lease. Codec storage may be freed before start.
- MethodPlan compilation rejects nested field aliases in the selected SDK.
  Preserve that admission boundary; do not bypass it with a consumer mapper.
  The first admission failure is recorded in worker-body-admission.log.
- The canonical JSON reader returns CSERDE_UNSUPPORTED for unknown keys/root
  kinds. After mount admits the schema, the HTTP input boundary maps only that
  wire rejection to CSERDE_VALUE_OUT_OF_RANGE (400), preserving other failures.
  CSERDE_INVALID_TOKEN is not a permitted reader callback return and becomes a
  callback error; preserve worker-body-reader.log with that failed experiment.
- Empty owned native strings may use their canonical zero representation.
  Test length/content rather than requiring a non-NULL tstr. The transient JSON
  DOM scales with admitted body bytes/active workers, outside native owned-byte
  accounting. An oversized transport body retains H1 413/H2 stream-reset behavior.
- Full native-release-user graph build, 124/124 CTests (three benchmarks) and
  install passed: migration-worker-body-release-{build,ctest,install}.log.
  Application-focused regression passed 11/11 suites; the initial body suite had
  27 cases across H1/H2 and DIRECT/CFlow/real Plugin execution, including
  malformed/unknown/duplicate/missing input, nested validation, exact uint64/
  embedded-NUL text, media/native budgets, one-slot reuse and blocked-method
  finite-close DTO/Bean/DSO retention. No public default switch is included.
- Post-review regression exposed a native bytes field that mounted despite the
  JSON reader having no bytes-token representation. Input mount now walks the
  MessagePlan-admitted finite graph and permits only scalar/string/enum/record
  fields; bytes, containers and variants await complete projection contracts.
  Preserve migration-worker-body-byte-admission-cases.log (27 pass, 1 failure);
  the corrected suite passes all 28 cases in worker-body-shape-cases.log.

## Workspace cleanup and fresh configuration

- Sanitizer options and their inherited presets now use ENABLE_ASAN,
  ENABLE_UBSAN and ENABLE_TSAN. Refresh old caches when migrating the names;
  the existing Linux/Clang qualification and TSan exclusivity remain intact.
- ChttpConfig must substitute SaltsCore_VERSION, populated by find_package.
  A generic VERSION placeholder was undefined and rendered an empty version
  before EXACT. Fresh configuration restores the selected SDK pin (0.1.4 here).
- Fresh configuration also exposed OpenSSL missing from the host test PATH.
  Preserve migration-workspace-cleanup-fresh.log. Explicitly adding the existing
  independent OpenSSL 3.2.4 host executable to this invocation's PATH resolved
  discovery; no product TLS dependency or CMake discovery fallback was added.
- native-release-user fresh configure, complete graph build, 121/121 formal
  CTests and install passed: migration-workspace-cleanup-tools-fresh.log and
  migration-workspace-cleanup-{build,ctest,install}.log. This fresh preset leaves
  BUILD_BENCHMARK off; CI explicitly enables its three benchmark CTests.

## Private worker JSON sequence fields

- Required record bodies now admit recursively nested SEQUENCE fields through
  CMeta's public static element descriptor, after MessagePlan verifies the
  finite native/schema graph. JSON arrays feed the existing native collector;
  canonical owning providers, native budgets, rollback and bounded output stay
  responsible for container elements. Map/set/bytes/variant and optional/null
  representations remain rejected; there is no token conversion or fallback.
- The generated fixture covers list<Detail>, list<list<uint64>> and list<string>.
  Business output copies through canonical cmeta_data_value_copy, and hooks
  verify independent record/string storage. Real Plugin discovery selects the
  declared operation by name, preserving each mount's own lease/metadata.
- An initial list<bytes> negative fixture failed generation because the released
  SDK lacks that canonical CSTL storage projection. Preserve
  migration-worker-sequences-concurrency-build.log. The formal mount negative
  uses the supported list<BinaryBody> native shape and proves rejection of its
  nested bytes field; no replacement production parser/storage was introduced.
- All 55 body cases passed via CTest in worker-sequences-qualified-cases.log:
  H1/H2 DIRECT/CFlow/real Plugin round trips, empty and nested arrays, exact
  integers/embedded-NUL strings, validation and partial-collection rollback,
  aggregate item/owned-byte limits, output abort/reuse, finite-close leases and
  two simultaneous worker frames. No public default switch is included.
- Complete native-release-user build, 121/121 formal CTests and install passed:
  migration-worker-sequences-final-{build,ctest,install}.log. The fresh local
  preset has benchmarks off; CI retains its full benchmark-enabled graph.


## Public worker entry points (2026-11-13)

- Application init now owns worker preparation and all Controller/HTTP/RPC,
  admission, asset, upload and WS registration. Public Invocation headers are
  installed; internal registration moved to app/src/chttp_application_registration.h.
  New product code must use configuration descriptors, not internal mounts.
- Invocation request.session now equals the worker-affine detached intent view.
  Header admission/upload-open and WS events/ready still have no Session; method
  hooks expose a separate const Session. When migrating assertions, preserve
  those distinct scopes rather than erasing Session checks globally.
- CHTTP_ACTION now declares Invocation. Negative ABI fixtures must describe a
  genuinely different canonical argument (the native request/admission ABI),
  otherwise the supposed legacy fixture has become valid after this migration.
- No business hooks means native 404/405 does not consume worker capacity. With
  global hooks, a private fallback submits through the ordinary worker record.
  The old global native middleware is gone; middleware-full tests explicitly
  configure a real static security policy to occupy that slot.
- WS ready is an optional reflected worker method with event == NULL, serialized
  before data/Close and retained through finite close. H2 peer completion tests
  destroy the client before waiting for final Server notification, as the
  existing native terminal contract requires.
- Complete native-release-user build and 121/121 formal CTests passed; evidence:
  build/migration-public-entry-final-build.log and
  build/migration-public-entry-verified-full.log. Initial failed migration
  regressions remain in their original build logs; no runtime fallback or
  TinyTest modification was introduced.
- CI run 105's GCC profile exposed a one-record fixture race in sequential
  JSON conversion cases: client receipt/destroy is not the Server terminal or
  completion-finalizer join, so the following request can correctly get 503.
  The sequential expect helper now waits within its existing 5-second bound
  for public worker stats pending/retained_bytes == 0 before the next request.
  This checks rollback release without changing runtime capacity, adding a
  request retry or weakening native lifetime. Original failed job 131763092027
  remains on run 43388598632; do not rerun that unchanged job.
- Run 107's macOS profile exposed a second fixture ordering assumption: the H2
  client send drains native output and propagates any observed peer terminal,
  so an immediately failing event can reset it before send returns. The event
  failure case now uses its existing hold/release condition to finish send
  before allowing SALTS_EIO, then still verifies exactly one message, Close and
  completion with the original business error. Failed job 131768681035 remains
  on run 43390271574; no transport return code was weakened or retried.
- Run 108's macOS C++ opening-exception case found a Client output bug: an H2
  rejection parsed while connect drains request output takes the error cleanup
  path before the old out_http_status assignment. Move that assignment to the
  common exit, preserving the original error while returning the parsed status.
  Client's existing formal socket suite adds H1/H2 403/500 denial cases. This is
  a production output fix, not a looser exception assertion. Preserve failed
  job 131771365280 on run 43391093568.


## Plain CMake and matching Base 0.1.8 composition (2026-11-13)

- Maintainer-directed cleanup removed all 19 historical CHttp GitHub Releases
  and their assets, and CHttp.Native with all 19 package versions. Both lists
  were verified empty; unrelated repositories/packages were outside the scope.
  Local receipt: build/chttp-release-cleanup-receipt.jsonl.
- CHttp shared targets no longer set VERSION/SOVERSION. Standard install/export
  rules and source/target helpers follow SaltsCNet. Product find_package uses
  SDK roots without upstream version arguments; presets no longer redeclare
  version environment variables. Package consumers resolve upstreams explicitly.
- Select Base 0.1.8, SaltsCNet 0.1.12 and Utils 0.1.4. Rename the old SaltsCore
  CMake package/root inputs to SaltsCNet / SALTS_CNET_ROOT. Restoration still
  uses version variables and validates original published manifests/assets;
  CMake itself neither restores dependencies nor selects release versions.
- Utils 0.1.4 explicitly declares CMeta 0.1.6 source ref
  679710f7e3312811efdd92494744a2499286a6a5 built against Base 0.1.8.
  Reuse Utils' released build-cmeta-sdk.ps1 for target and Android-host SDKs.
  The old released CMeta binary requires Base 0.1.6 and is a different
  composition; no SDK metadata is patched and no binary fallback is used.
- CNet's release schema supplies archive digest but not sizeBytes; acquire its
  exact size from the release asset API. Utils supplies sizeBytes as well.
  Validate each schema's actual contract without guessing alternative fields.
- Native LLVM-MinGW source SDK build, fresh CHttp configure, complete build,
  121/121 formal CTests and install passed. Logs are
  migration-plain-cmake-cmeta-sdk.log and
  migration-plain-cmake-base018-{configure,build,ctest,install}.log in build/.
  No independent consumer project or TinyTest change was introduced.
