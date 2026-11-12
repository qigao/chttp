# Reflected HTTP Application migration

The application API replaces public callback registration and the standalone
chttp_service owner. This is an intentional source-breaking migration.
HTTP/1, H2, TLS, WebSocket, RPC, body limits and terminal ownership remain with
the existing server engine. Its registration and lifecycle entry points become
private. No compatibility header, fallback implementation or versioned API is added.

## Chosen composition

- One Application owns one bounded HTTP server, one ACE Configurator graph and
  its immutable Controller/action/interceptor bindings. CMeta::ACE owns component
  creation, dependency admission, activation rollback and reverse destruction.
- Deployments explicitly select providers; there is no classpath scan, global
  service locator or hidden singleton. A Controller identifies its component and
  canonical DataDesc. Startup binds its native object once from the active graph.
- HTTP annotations describe method/path separately from canonical CMeta Function
  metadata. Exact native callback types and Function ABI are both checked before
  publishing any route. A binding cannot manufacture lifecycle/provider authority.
- Exchange actions expose callback-borrowed request/response views. DTO operations
  reuse the generated DataBind HTTP/RPC plans and native execution contracts.
  The old MethodPlan runtime becomes a private mechanism, not another service API.
- Typed ACE Interceptor chains wrap dispatch in before/after/error order. Bean
  objects and immutable metadata survive every request and deferred finalizer.
  Dispatch hooks do not claim to observe eventual transport delivery.
  Browser security headers and CORS protocol preflight run before this chain;
  static assets and SPA fallback run inside it, before terminal route dispatch.
  An Interceptor rejection cannot publish file-response work or expose bytes.
  Application owns fixed-size Security, CORS and asset policy snapshots; their
  strings, origin arrays and callback contexts remain borrowed until close.
  Caller policy structures may be reused after init. File owners retain their
  completion and drain obligations. Asset-only dispatch has no reflected
  Controller operation and supplies NULL operation metadata to hooks.
- Close stops admission, drains real I/O and deferred work, destroys the server
  and MethodPlan records, then stops the component graph. Timeout/failure retains
  live ownership for explicit close; it never zeros a live runtime.
- C11 with-application execution returns operation and close errors. C++17 offers
  a noncopyable RAII Application with checked close; an unhandled destructor
  cleanup failure is fatal, rather than silently leaking or reporting success.

Policy snapshots are copied on the control thread before activating Beans.
Sized policy inputs must admit the known structure before copying it. Runtime
callbacks borrow only Application-owned snapshots, which are immutable until
HTTP/file completion drains and the Application is freed. No per-request copy,
new allocation path, capacity policy or background policy mutation is introduced.

Only static dependency construction is required here. Dynamic ServiceHost
generation publication is an independent future capability, not a placeholder.
NativeIO/CNet retain their sole execution/completion authority. Building a second
HTTP runtime, interpreting erased native function pointers, and exposing both old
and new business registration APIs were rejected.

## Migration and validation

Public examples move to Application/Controller declarations. Historical handwritten
apps that prove Session, CSRF, templates, uploads and generated bindings become
module-owned formal fixtures; those behavior tests continue to run. No regression
is replaced with a configure marker or independent consumer project.

Application tests cover reflected ABI rejection, missing dependencies,
startup rollback, ordered interceptors, native H1/H2 requests, generated DTO
inline/deferred/CFlow binding, capacity, streamed-body receiver identity,
deferred shutdown and C++ RAII. Existing engine suites cover concurrent requests,
protocol source lifetimes and security. ACE owns graph-cycle/ambiguity admission.
Existing official CTest and the published-SDK CI matrix remain the acceptance
entry points. New public headers and installed exports expose only the new
application registration path.

Rollback requires reverting this migration as one change, restoring the old
public headers and their callers together. It does not change wire formats or
user data. No release is claimed until the complete implementation and its
formal validation pass.

## Typed C++ Bean admission

Typed factories transfer exactly std::unique_ptr<T> into delete-T ownership.
Constructor admission rejects custom/derived pointer result types; create checks
canonical DataDesc validity and exact native size/alignment before any factory
side effect. Explicit ACE providers own other deletion models. These checks
protect the typed allocation/deletion boundary; callers still supply the correct
semantic DataDesc identity for T. Borrowed Interface views gain no ownership.

## Worker-dispatched Application (design and private implementation)

This section specifies the next programming model. The implementation above is
still the current contract: ordinary actions and the HTTP Interceptor chain run
on the connection owner, while generated HTTP bindings may explicitly defer.
None of the API changes below is available merely by configuring today's SDK.

Evidence for the existing boundaries is in [Application bindings](../app/src/chttp_application.c),
[generated deferred execution](../app/src/chttp_method_runtime.c),
[public Application types](../app/include/chttp_app/application.h) and
[HTTP deferred/source ownership](../http_server/include/http_server/http.h).
The selected CMeta 0.1.6 worker provider wraps the SaltsBase thread pool; its
[implementation](https://github.com/qigao/cmeta/blob/v0.1.6/cflow/src/executor.c)
is the scheduler reuse point. These facts do not establish the proposed mode.

### Internal implementation status

The private `app/src/chttp_application_work.h` bridge now owns a bounded CFlow
worker pool, deep-copied buffered request state and a copied response draft.
Private Application bindings resolve Controller receivers from the same ACE
graph; their methods, injected Service calls and typed Interceptor chain execute
on workers. The private prototype rejects mixing legacy owner callbacks and
is neither installed nor selected by the public Application API.

Buffered deferred responses have an owner-side terminal ticket. Business
finalization and native response completion/reset join before scheduling one
worker completion observer, retaining snapshots, admission slots and Beans
through observer finalization. H2 encoder stream closure is not native send
completion: ready tickets move to the writing batch only when that batch is
submitted, and settle when NativeIO reports it sent. Peer RST_STREAM(NO_ERROR)
still means cancellation. Success establishes local native send completion,
not consumption by the remote application.

Generated synchronous direct, CFlow and Plugin HTTP methods can mount on that same private
pool. The existing MethodPlan evaluator now writes through an internal output
adapter: the published engine path retains its buffer publication semantics,
while the worker path copies into the invocation draft before releasing the
DataBind native frame and serializer buffer. Binding, validation, native method
execution and output staging all stay on the worker; there is no nested executor
post/wait. The method record and canonical ACE receiver remain retained through
terminal finalization. CFlow compiles its producer-owned typed adapter once and
evaluates the immutable Plan in that same task, without a borrowed executor or
nested scheduling. Owned Plan output remains live through serialization, then
is destroyed before the input frame. Plugin HTTP mounts resolve catalog/native
execution under an existing mount-owned lease; Plugin RPC retains the producer's
execution context and its own mount lease. Both execute on the same pool and
keep provider code pinned through joined close. Private generated-method
before/after/on_error hooks now surround bound native/Plan execution and outcome
staging on that same pool, with canonical ACE hook receivers and bounded copied
metadata. Native status, staging failure and explicit rejection remain distinct.
Per-method completion now retains bounded frozen facts and uses the same joined
worker notification after the enclosing native response terminal.

Private generated RPC mounts use the same pool for the complete HTTP envelope.
The Server registry exposes a private scheduler-neutral evaluator/output boundary;
the owner adapter only snapshots and admits work. Existing bounded batch parsing,
sequential method dispatch, protocol errors and notification suppression remain
shared with the engine path. Pure notification batches preserve an empty 204
without adding a Content-Type header. Server registry/frame pools and ACE receivers
remain live until the worker invocation drains, including after an H2 handler
deadline. Per-RPC-method business hooks have independent bound frames and unwind
within each sequential call, including notifications and rejection.

Private buffered worker bindings now also support detached, bounded Session
snapshots and owner-side intent commits. Same-Session calls wait in the existing
invocation pool and refresh in admission order without occupying a worker. The
gate retires after business finalization joins owner commit/abort, independently
of wire completion. Ordinary failed/canceled writes are discarded; explicit
invalidation retains the existing fail-closed security contract, including after
transport loss, and never clears a newer generation. H1/H2 tests exercise CSRF,
login/logout, cookie flags, regeneration, capacity failures, stale queues,
cancellation/drain and H2 flow-window backpressure. The public deferred API still
rejects live Sessions; no partial public Application mode has been enabled.

`chttp_application_worker_test` covers snapshot isolation, overload, real H1/H2
deadlines, one/two-worker execution, hook unwinding, finite close and H2 flow
control. `chttp_application_test` exercises actual ACE-injected Controller/Service
dispatch and graph retention across a blocked completion observer;
`chttp_application_binding_test` verifies H1/H2 generated success/validation/
missing-input behavior, typed RPC, mixed/notification batches, protocol errors
and canceled running native methods. The Server suite
covers the zero-error reset. `chttp_application_plugin_test` exercises real DSO
HTTP/RPC worker execution, provider context, lease rollback, stopped-provider
admission, output failure/reuse and canceled running methods with finite close.
`chttp_application_output_test` verifies bounded required record JSON output,
typed-error mappings, integer/text fidelity, capacity rollback/reuse, mixed RPC
batches and real DSO drain under canceled running methods. Method completion
tests cover entered reverse ordering, completion-only hooks, immutable per-item
facts, blocked-observer leases and H2 flow-window backpressure while the sole
worker serves another method.
Public Session/admission integration, stream/WS adaptation, remaining DTO ingress/
state/collection representations and the public Invocation migration remain required
before switching the programming model. No runtime feature switch was added.

### Decision and alternatives

The private CFlow migration admits a synchronous producer-owned typed adapter
without an external executor. Mount compiles one immutable Plan using the same
canonical type/effect/ABI checks as the engine path. The existing Application
slot/byte admission owns the copied input and draft; its worker binds a bounded
native frame, evaluates exactly one Plan input, serializes the owned result into
the draft, and destroys that result before the frame. No second task, blocking
post/wait, interpreter fallback or async-Plan emulation is allowed. Invalid
projection/unsupported effects fail before publishing the route; evaluation or
serialization failure yields the existing 500 response. The method record and
borrowed code/captures remain live through joined native-terminal finalization,
including when cancellation wins before a running projection returns. This is
an internal step only; it does not change installed metadata or public defaults.
The legacy engine path retains its explicitly borrowed executor contract.

The private Plugin migration reuses the existing registry and mount-owned method
lease. HTTP resolves its catalog/export and exact adapter at mount; RPC borrows
the admitted provider execution context under its own registry lease, rather
than replacing that context with a Controller pointer. Both use the same owned
Application worker task for binding/invocation/serialization and require no
borrowed executor. Failed mounts release their acquired lease before exposing
a route. Stop closes new lease admission but does not revoke an admitted method;
unload stays busy until workers, native response terminals, method/Server storage
and provider-owned value cleanup have drained. The control-thread registry and
all borrowed descriptors remain address-stable through Application close. No
lease counter, loader, retry or scheduling layer is added. This remains private;
the old engine's explicit executor behavior and public defaults are unchanged.

The baseline is that application-supplied Controller actions, generated native
Service methods, RPC methods and application Interceptor hooks execute on a
bounded worker pool. Ordinary injected Service calls execute directly on the
calling worker; injection does not add another queue hop. The HTTP owner never
waits for a user method or executes one as a queue-full fallback.

Reuse an Application-owned CFlow worker executor, created through
`cflow_executor_worker_init_with_capacity`. Its implementation already uses the
ordinary SaltsBase thread pool. Application owns initialization, admission and
drain; the existing MethodPlan runtime borrows this same executor. Do not accept
an arbitrary Manual/owner-driven executor as proof of worker execution. The
initial implementation does not add an executor framework, LF backend, automatic
backend selection, or upstream dependency.

Rejected alternatives are synchronous owner dispatch (violates the requested
baseline), one thread per request/unbounded queues (unbounded resources), and
moving CNet or native endpoints to business workers (breaks progress ownership).
Starting with coroutine-only business execution would also leave ordinary
blocking libraries occupying coroutine shard threads. Pool dispatch comes first;
cooperative suspension is a later explicit execution mode, never a fallback.

This intentionally changes public execution/context semantics. Replace the
callback-scoped Exchange/engine response exposure with one Application invocation
contract; do not ship parallel old/new business APIs, version dispatch, or an
inline compatibility path. Public signatures and canonical metadata must be
reviewed before implementation. Old native/plugin bindings must fail admission,
not accidentally pass because a pointer ABI retained its old spelling.

### Thread roles and request path

| Role | Work and restrictions |
| --- | --- |
| Control thread | Create Components and workers, start ingress, close/drain. Never close/wait from a business callback. |
| HTTP connection owner | CNet/NativeIO progress, H1/H2 parsing, TLS, stream/connection generations, wire flow control, bounded transport publication and real terminal observation. No user business method or blocking queue admission. |
| Application worker | Custom admission/authorization, Controller/Service methods, HTTP/RPC business Interceptors, DTO conversion, rendering and application completion observers. No direct mutation of an engine request, response, Session store or endpoint. |
| External async producer | Publish an owned result/status and signal its designated owner. It neither resumes a coroutine on the producer thread nor invokes business hooks there. |

The logical path is:

```text
owner: parse headers; built-in protocol/JWT checks; claim bounded invocation
  -> worker: custom header admission, if configured
  -> owner: admit/reject body; parse or incrementally receive within budgets
  -> worker: before -> Controller/Service -> after or on_error
  -> owner: validate staged result; commit Session intent and response; send
  -> owner: observe response terminal (success, cancellation, or error)
  -> worker: after_completion; final release when all owners are settled
```

H1 retains at most one admitted invocation per connection and preserves ordered
keep-alive/pipelined responses. H2 uses one bounded invocation per admitted stream;
worker completion order across streams need not match arrival order. Each stream
returns to its original connection owner; this design does not migrate endpoints.

Built-in CORS preflight, protocol failures and JWT denial can terminate without
calling application code. Every supplied admission callback uses the header-only
worker stage; it must not receive a borrowed parser view. H1 does not send
100 Continue before successful admission. H2 suspends admission/credit for that
stream within existing bounds while other streams continue progressing. This
requires an internal retained admission bridge; today's response_defer API is
not valid at an arbitrary header callback.

A one-worker default preserves serialized business execution. Worker count is
independent of HTTP owner count. With multiple workers, Controller, Service and
Interceptor Beans may be invoked concurrently. Their declared implementations
must be safe for that topology; reflection and RAII provide no synchronization.
Do not automatically put a global mutex around Beans. A non-reentrant Jinja
renderer needs independent instances or explicit application serialization.

### Invocation ownership and response staging

The private handwritten Controller binding now adopts the buffered Invocation
contract before the public swap. Its exact native action is
`int(void *self, const chttp_invocation *)`; a nominal `chttp.invocation` type
identity distinguishes it from the old Exchange even though both native
arguments use an object-pointer carrier. CMeta admits effects, parameter/result
ownership, canonical types and ABI before publishing a route. The old Exchange
descriptor is rejected; no native function-pointer cast or erased invocation is
used. The callback-only handle describes its real one-pointer borrowed layout,
not an invented opaque value size. Neither it nor any accessor view may escape
the callback or be retained by a background task.

Invocation accessors borrow the existing owned request snapshot and detached
Session intent view. Its request view clears the Session pointer; explicit
Session access grants only the already-admitted staged request transaction.
Respond copies into the existing bounded draft. Cancellation queries read the
existing synchronized native-terminal state and report only observed failed
terminal, not a guessed deadline or a second cancellation authority. Successful
business return still requires a sealed response before success hooks unwind.
Only before hooks can explicitly reject; entered after/error hooks retain the
existing ACE ordering and one completion/finalization join.

Controller hook descriptors resolve canonical Bean contexts and are copied at
mount. Thin typed adapters construct a fresh callback-local Invocation over the
same request/draft/result and reuse the existing worker ACE chain, rather than
nesting a second chain or creating a task per hook. Completion still borrows the
settled read-only snapshot/outcome after terminal; it never receives a writable
Invocation. This adds no per-request allocation, queue, growing state or retention
protocol. Copied hook rows occupy fixed control-plane metadata per route, with
the existing route-capacity allocation's checked multiplication.
The existing slot/retained/reply budgets, Session gate, cancellation and close
joins remain authoritative. Frame/handle construction is on the worker; owner
callbacks only perform the established snapshot/admission handoff.

This is an internal replacement of the raw handwritten worker callback, not an
installed parallel programming model. New headers are not installed; public
Exchange defaults, streaming/admission/WS adapters and generated DTO native
contracts are unchanged until their complete public migration. C++17 action
adapters contain exceptions before returning through C worker frames and preserve
ordinary RAII unwinding. Callback code, canonical metadata and hook Beans remain
borrowed through Application drain. Rollback reverts the private binding and its
formal callers together, without changing stored data or wire behavior.

Application owns a finite set of address-stable invocation records. A record
contains its slot/generation, immutable request snapshot, canonical operation,
ACE/provider leases, active-stage deadline, cancellation state, worker-local DTO/frame
storage, staged response and terminal-result storage. Its slot cannot be reused
until business tasks, transport/source owners and completion observers settle.
This Application lifecycle record does not replace CNet's authoritative native
request/connection state machine.

Header/route/query/body/JWT/peer-identity data are copied or explicitly retained
into bounded immutable storage before the original owner callback returns.
Do not shallow-copy the existing request struct: it contains borrowed pointers,
a live Session context and body-sink context. Metadata remains borrowed from the
admitted ACE graph, whose lifetime covers the whole invocation. Native/plugin
frames retain their existing exact-once destructors and provider/module leases.

The proposed action argument is an opaque Application invocation, with borrowed
request accessors, an Application response writer and a cancellation query.
Its accessors are valid only within the documented worker invocation/hook.
A new canonical semantic identity must distinguish it from the old callback
Exchange; remove the old public argument and rebuild native callers. No v2
interface or runtime version switch is proposed.

The worker response writer owns a bounded draft, not chttp_server_response.
Returning from the user method seals it only after entered business Interceptors
unwind. Interceptor rejection stages its own rejection without running the method.
Owner-side commit validates and publishes one immutable result through the real
HTTP engine. Worker success means business completion, not transport delivery.
Publication of a copied or retained response may precede wire completion; it does
not authorize recycling the invocation, source state or Bean/provider lifetime.

No arbitrary background task may retain a raw Bean/request pointer after its
invocation returns. Generated deferred/CFlow/plugin execution must keep its frame
and lease until the producer's real terminal/finalizer. There is one business
completion authority even when an external producer participates.

### Interceptors, RPC and terminal outcomes

The private structured-output migration takes the same codec used to compile
the MethodPlan explicitly at worker mount. Control-plane schema introspection
resolves a projected record field or a declared typed-error name; native
MessagePlan admission and a JSON FormatPlan verify that payload before route
publication. Method records own copied FormatPlans and borrow canonical native
descriptors under the existing Bean/provider lifetime. The codec is borrowed
only during mount, with no runtime schema lookup or transport format guessing.
Supplying the same schema is a caller precondition: the current SDK exposes no
MethodPlan schema fingerprint. Native admission rejects incompatible payload
layouts but is not a substitute for schema identity.
Legacy engine mounts retain their scalar contract.

This slice admits required non-nullable record payloads whose FormatPlan needs
no optional/null native state overlay or nested field-name rewriting. Existing
scalar field projections keep their original raw HTTP/JSON-RPC representation.
Unsupported payloads/formats fail mount, with no scalar or format fallback.
HTTP emits structured JSON with application/json. RPC stages the same bounded
JSON and opens the existing JSON reader before commit, then synchronously
replays its exact integer/string/container tokens into the envelope writer.
The reader and backing byte buffer are retained through result/error encoding;
native DTO/error objects remain live through business-hook unwind and cleanup.

The existing per-response/value byte limit bounds the JSON sink, and native
depth/item/workspace limits still apply. RPC's temporary parser storage is
proportional to that bounded document and its depth; the outer envelope/batch
retains its own byte/depth limits. Each accepted worker/call slot owns at most
one output transaction. Capacity, native encode, writer finish, reader open or
envelope failure propagates without publishing partial output. FormatPlans
are freed before Plugin leases, and all worker/terminal joins still precede
method/Server/Bean destruction. No new dependency, parser, serializer framework,
unbounded growth, public mode or installed ABI is introduced.
RPC pays one bounded JSON parse to reuse the provider's exact token handling;
a new token tape would add separate string/container ownership and admission
rules. This choice makes no performance improvement claim.

The private generated-method business slice uses one scheduler-neutral ACE
chain shared by HTTP and RPC. Application resolves every hook context from its
canonical ACE graph and mounts a copied array of at most 16 hooks. Only the
worker composition admits these hooks; engine mounts cannot silently execute
them on an owner. After successful input binding, before hooks enter forward;
native/Plan execution and DataBind outcome staging form the target; successful
after hooks or entered on_error hooks unwind in reverse before provider/native
cleanup. Borrowed HTTP snapshots, canonical operation metadata, bound input and
staged output views live only through that call. The request copy clears its
mutable Session handle; a separate const Session view permits reads only.
Method hooks may observe DTOs/results but cannot mutate frames or Session state;
Session writes belong to the enclosing request transaction, not a batch item.
CFlow's owned output stays live
through unwinding. No hook can retain a frame or overwrite a published response.

An explicit before rejection skips the target and maps to HTTP 403 or JSON-RPC
-32003 (Forbidden); a notification still emits no item. A malformed hook/failed
callback maps to the existing internal-error path. Native status, typed business
error and binding/serialization failure enter on_error with separate DataBind
status/outcome facts; transport cancellation never reruns business on_error.
Private worker mounts use the admitted FormatPlan for required record values
and typed errors. Successful typed-error staging preserves the declared HTTP
status / RPC code and live error frame during on_error. Staging failure still
maps to HTTP 500 / RPC -32603 with DataBind outcome NONE; it never claims the
business mapping after failed serialization. Scalar-only legacy engine mounts
retain their existing behavior.
Ingress failures have no entered method chain. RPC invokes a fresh chain for
each bound batch item, preserving sequential order, method identity and
notification/error suppression. No second task is posted. Hooks remain borrowed
from the live graph through drain.

The private completion slice retains per-method numeric outcomes for the
enclosing HTTP terminal. Application admission reserves one record for HTTP,
or at most the configured RPC max_batch_items when any RPC method has completion
hooks. Checked record bytes count against the existing retained-byte budget
before response defer or business side effects. Exhaustion rejects admission;
there is no growing observer list, owner callback fallback or new executor queue.

After successful input binding, reserve a record before entering the ACE chain.
A bounded proxy records entered hook count while the upstream ACE chain remains
the ordering/error authority. Completion runs entered hooks in reverse order,
including the rejecting hook; unentered hooks and malformed/unknown/unbound
methods get no notification. RPC records retain batch order and notification
identity. Bind/execute/staging, interceptor status, response-adapter status and
the request's business/publication/Session/transport results stay distinct.

The existing Application join posts one worker notification only after business
finalization, Session commit/abort and the real native response terminal settle.
That notification visits method records, then the enclosing task observer, before
task finalization and record/snapshot release. Native frames, CFlow results and
RPC parser views are already destroyed: completion exposes immutable numeric
facts, canonical operation/typed-error names, registry-owned RPC method identity
and a read-only copied HTTP/Session view only. All borrowed metadata, hook Beans
and Plugin method leases remain live until the whole notification drains.
Cancellation is not completion and cannot observe a still-running method. A
finite close timeout retains all ownership, including a blocked completion
observer. Multiple workers may execute different invocation notifications
concurrently; a single invocation has one business writer and one later reader,
ordered through the existing executor/join synchronization.

Reusing the bounded invocation join avoids a per-method task/queue and keeps
transport settlement authoritative. Retaining DTOs until wire completion would
pin native frames and managed values unnecessarily; replaying business hooks
would duplicate side effects. Neither is part of this callback contract. This
remains an internal migration and introduces no installed API or public mode.

Use distinct stages with immutable, explicit outcomes:

| Stage | Meaning and execution |
| --- | --- |
| before | Worker; declaration order. Can reject before business side effects. |
| after / on_error | Worker; unwind entered hooks in reverse after the real business invocation ends. Never run merely because a task was queued. |
| after_completion | Worker; exactly once after both business settlement and response terminal are known. Receives separate business, cancellation and transport results; cannot replace an already committed response. |

A successful business return followed by a network reset remains business
success plus transport failure; it does not trigger a second business on_error.
A timeout cannot concurrently invoke on_error on a still-running invocation.
An explicitly rejected request has no method execution, but can still have an
observable response terminal. Pre-application protocol/JWT rejections have no
entered application chain. Transport success does not prove that the peer's
application processed the bytes.

HTTP-request Interceptors run once for the HTTP envelope, including RPC batches.
Add explicit RPC-method interception at the typed invocation boundary: method
metadata, params, result/error and entered-hook state belong to each call.
Selecting an HTTP or RPC-method scope is explicit; an HTTP hook is not silently
replayed N times. Initial batch execution is sequential within one worker-owned
batch invocation, preserving current behavior and bounding work by max_batch_items.
It releases the worker on return; no nested task posting plus wait on the same
pool. Notifications execute business hooks but emit no JSON-RPC result item;
the batch aggregator preserves current empty/notification/error wire semantics.
Each method unwinds its business hooks when it returns. Its response-completion
observer sees the enclosing HTTP batch terminal, not a separate wire receipt for
that JSON-RPC item; notification completion does not invent a response item.
Parallel batch execution is outside this decision and needs its own bounded
admission, result aggregation and ordering contract.

### Capacity, task publication and errors

Use these proposed Application inputs (names are design vocabulary, not public
header declarations): worker_count W > 0, invocation_capacity J > 0, and an
explicit finite retained-byte budget. Existing HTTP header/body/response and
BindingPlan depth/item/frame bounds remain the per-request limits. The default
config must fill valid bounded values, including W=1; init rejects invalid or
overflowing explicit values. No CMake feature flag is added.

The worker executor's queue capacity is derived from J. An invocation may have
at most one queued worker stage at a time: admission, business, stream step, or
completion notification. A stage that hands off to the owner must finish its
frame and predecessor finalizer last-use publication before a successor can run.
A run callback must not post its successor while its later finalize callback
still needs the invocation; owner readiness and predecessor-finalized readiness
must join before the single successor is scheduled. Transport events that
arrive early record status; they do not schedule a second concurrent stage.
There may be multiple owners waiting on a record, but never two mutable business
stages for that record. Completed/result-waiting records still count against J.

Consequently, when an admitted invocation schedules its next stage, at most J-1
other records can occupy queue entries. The J-entry pool queue reserves capacity
for that transition, including after_completion; no second unbounded queue,
allocation on terminal, blocking post, inline fallback or notification drop is
allowed. The chosen SDK's descriptor admission must be verified to honor this
invariant under concurrent producers. Executor acceptance is transactional: a
rejected post transfers no payload and invokes no cancel/finalize callback.

The current task tail may still be returning when the next descriptor is claimed.
Before posting that successor, release all mutable frame ownership and complete
the current task's last access to the record; code after successful publication
must not dereference it. Exactly-one scheduling/terminal arbitration uses the
record's slot/generation and release/acquire state, not copied independent flags.
Do not recycle a record while any predecessor callback can still access it.

Only external ingress competes for free records/bytes. Refuse exhausted capacity
with a bounded 503 before user execution; configured policy rate limits remain
429. Invalid input uses its existing 4xx contract; startup config/ABI mismatches
fail init. Queue wait time consumes the same absolute deadline for its stage;
work does not reset it. Existing body_ms includes custom header-admission queue
and execution plus body reception; handler_ms begins when the body is complete
and includes the business queue, hooks and method, while response transfer retains
its existing transport budget. This does not silently replace the three public
phase budgets with a new whole-request timeout. Completion publication FULL/CLOSED
after reservation is an invariant/lifecycle error, not ordinary overload: report
the failure, stop new
admission and retain unsettled ownership for control-plane diagnosis. Never
report success or drop the callback. Do not shut the worker executor while
accepted invocations can still need internal completion stages.

Track retained bytes for snapshots, DTO/native frames, drafts, Session intents
and pending notification state. Existing HTTP buffer capacity separately bounds
protocol storage; moving/retaining memory requires explicit charge transfer or
reservation, never an unaccounted second owner. Check every size/count product
and sum. Required retained storage is bounded by J and the byte budget, plus
known executor/task metadata and W worker stacks; request limits alone do not
bound all concurrent retained bytes.

### Cancellation, deadlines and close

Cancellation is a request state, not proof that user code or external I/O ended.
A queued stage checks cancellation before entering user code. A running method
sees a cooperative cancellation query and may arrange cancellation with its
external subsystem; CHTTP cannot kill a thread, undo a database write or invent
an external completion. Disconnection/RST_STREAM makes the response target stale
but leaves worker data, native frames and Bean leases alive until real settlement.
No failed/late operation is automatically retried.

The owner arbitrates one HTTP response outcome using the existing generation and
terminal authority. If timeout wins before commit, apply the existing phase
failure semantics (including H1 close/H2 stream cancellation); discard a later
business response without executing the user method again. Keep business outcome
and response/cancellation outcome distinct. Timeout while queued or running must
not free worker-visible storage. after_completion waits for both sides and runs
once, even if the response terminal happened before the worker returned.

Close uses one absolute budget: stop ingress; cancel/mark pending application
work as policy requires while owners and workers still progress; settle accepted
HTTP/deferred/source terminals and business frames; execute reserved completion
notifications; wait for invocation and callback/producer leases to reach zero;
then close/drain/destroy the Application-owned worker executor and finally the
ACE graph. Framework tasks must retain any required upstream executor/plugin
until its admitted work actually settles. Do not wait on or destroy a borrowed
shared executor or stop unrelated work on it. Blocking user code can prevent
drain: finite close returns timeout/error with ownership retained. There is no
forced free or successful close with a live task. External producers must be
quiescent before the relevant executor is destroyed.

### Session, files, streaming and WebSocket migration requirements

The private Session implementation uses one charged, invocation-owned snapshot
and staged mutation context, not the native request's live Session context.
Existing Session accessors operate on that detached context; creation, ID
regeneration and invalidation change intents only. The native owner validates
the pinned original generation, reserves the cookie header, and commits the
snapshot before response serialization. Business failure/cancellation discards
ordinary write/regeneration intents. A later wire failure does not undo an
already committed Session. Explicit invalidation is the security exception:
the existing sign-in failure/logout contract must fail closed. A live owner
expires the cookie even for a failed business result; if transport/cookie
publication has failed, framework retirement reconciles invalidation under the
store mutex before releasing the pin/gate. It clears only the admitted original
generation, never a newer identity, and touches no native endpoint. Discarding
all intents was rejected because it could preserve old authentication after
the helper explicitly invalidated it. Transport/session errors remain visible;
this cleanup does not synthesize a successful response or retry user code.
Original owner-side Session mutations are rejected at handoff, rather than
silently discarded. A generation invalidated while queued fails before business
execution with HTTP 409 and no Set-Cookie, so a late stale response cannot replace
the new generation's cookie. Owner commit/header/store failures retain their
error and follow the existing deferred H1 close/H2 reset path.

Session record pins prevent expiry/reuse before commit/abort; explicit
invalidation or regeneration still makes an old generation stale. An existing
Session's immutable record identity selects a FIFO logical gate within the
Application's existing J slots. A waiting invocation is not posted to a worker.
When both predecessor business finalization and owner commit/abort are known,
the next waiter may run and refresh its snapshot from the authoritative store.
This releases the gate before transport completion; an H2 flow-control stall
must not block later same-Session business work. Canceled waiters are removed
and settled without invoking user code. Completion observers still wait for
the separate real response terminal, and cannot mutate sealed Session intents.

All queue/gate scheduling uses the existing worker mutex; snapshot/store/pin
operations use the existing short Session mutex. Neither lock is held across
user methods, executor publication or native transport.
During gate retirement, the lock order is worker then Session; native commit
releases the Session mutex before signaling the worker join.
No automatic retry, thread-per-Session, external runtime or unbounded waiting
allocation is added.
The ordinary public deferred API continues rejecting live Sessions; only the
private owned worker bridge can attach this staged context. This remains an
internal migration slice until the complete public replacement is ready.

The worker baseline cannot be implemented by bypassing current unsupported
cases. In particular, today's response_defer explicitly rejects Session-backed
requests, WebSocket handshakes and deferred streaming bodies.

- Session remains authoritative in the existing Server store. Replace worker
  access to live chttp_session with a bounded snapshot plus staged mutations.
  Serialize same-Session business invocations through a bounded logical gate,
  coordinated under the existing short Session-store synchronization; never
  hold its mutex during user execution, await or transport. A waiting Session
  request holds an invocation slot, not a worker thread. Pin Session generation
  through commit. Owner applies validated intents (including regenerate/CSRF/
  principal/cookie transitions) before response headers; stale/invalidation or
  cancelled commits fail explicitly, never rerun business code or claim database
  rollback. Session read-only/write admission and existing Web helpers must be
  migrated together; do not expose old live contexts to workers.
- Static file selection and application authorization run on workers. File I/O
  and file response terminal remain in the existing asynchronous file runtime.
  File/source ownership survives method return and after hooks until terminal.
- Upload sinks, SSE/source producers and WebSocket opening/events are user
  callbacks and therefore need worker adapters too. Use bounded immutable chunk/
  event messages and one sequential worker stage per stream/peer; keep engine
  callbacks short and owner-affine. Preserve message ordering and generation
  checks, and return credit/read demand only after consumption. An empty producer
  queue requires an explicit WAIT/wake source contract; do not treat a synchronous
  source's zero bytes as temporary emptiness instead of EOF. Closing settles every
  admitted chunk/event and its cleanup, including explicit cancellation of work
  not executed. Per-stream bytes/events remain independently bounded; waiting
  streams/connections also consume retained Application capacity.
- Existing low-level engine source-cleanup callbacks still run on their owner
  as nonblocking resource callbacks. The Application adapter records their
  terminal and schedules the user's worker notification; it does not change the
  low-level callback's thread or native ownership contract.

These are required migration contracts, not claims of current support. Do not
publish a new default mode that silently runs these user callbacks inline,
turns features off, or exposes partially implemented public APIs.

### Cooperative await extension

After the pool baseline, an explicitly selected coroutine execution mode may
reuse SaltsBase coro_executor_yield/await_begin/await/await_for/await_complete.
It needs an admitted typed bridge to the Application invocation, not a cast of
coro_executor_t to cflow_executor. Reuse the same owned input, staged result,
terminal arbitration and Bean leases. An await suspends only the business
coroutine, resumes only on its original shard, and preserves its context until
external terminal/drain. A timeout result alone does not authorize freeing an
external operation's still-borrowed payload. Blocking third-party functions
still need a blocking worker pool or a genuine async driver; yield alone does
not make them nonblocking. No C++20 co_await API or automatic coroutine conversion
is part of this baseline.

### Implementation and formal acceptance

HIGH risks are borrowed-data escape, early slot/Bean/provider reclamation, native
ABI mismatch, and changed Session/authorization/interceptor timing. The ownership
and negative tests below are release gates. MED costs are extra bounded snapshot
copying, worker queue latency and retained long-lived stream slots; benchmark them
without claiming that moving work alone improves throughput.

1. Specify/review canonical Invocation ABI, request accessors, response writer,
   worker limits and hook stages. This is a public source/ABI migration; regenerate
   bindings, update C/C++ examples and installed package documentation together.
2. Build the worker bridge and retained response-terminal adapter internally,
   using normal targets and existing module tests. Cover handwritten actions,
   generated HTTP and sequential RPC batches first; retain the old public model
   until the complete replacement is ready, rather than publishing a mixed mode.
3. Migrate Session/browser helpers, assets, upload/source/SSE and WebSocket
   adapters and their existing fixtures. Publish the worker-default replacement
   only when every supported public business callback obeys the thread contract.
4. Add cooperative await separately once pool-only semantics and drain are proven.

Formal tests belong to app/tests/application_test.c, application_binding_test.c,
application_invocation_test.c, application_cpp_test.cpp and the existing service/web suites; engine terminal
and H1/H2 conformance remains in http_server/tests and http_common/tests.
Use barriers/events and thread identity, not timing sleeps, to verify:

- Controller/Service, all business hook stages and custom admission run on a
  worker distinct from each HTTP owner for both H1 and H2; response transmission
  and protocol progress retain their owner. One-worker execution is serialized;
  explicit multi-worker concurrency obeys Bean ownership contracts.
- Deliberately blocked user code leaves other owners/streams progressing.
  Full record/byte budgets reject before business invocation without inline
  execution; completion notification still runs when business admission is full.
- Rejection and method success/error unwind exactly the entered hooks; early and
  late response terminal, cancellation and notification scheduling each have one
  winner. No hook concurrency or record reuse before predecessor last use.
- Disconnect, H2 reset, queued/running timeout, response-copy failure, late
  completion and finite-close failure preserve all frames and provider leases;
  finalize/observer counts and retained bytes settle exactly once.
- RPC named/positional params, batches, notifications, large arrays and bounded
  errors preserve current wire behavior and add per-method hook identity.
- Concurrent same-Session H1/H2 requests, regenerate/invalidate, CSRF/login/logout,
  file/SSE/upload cleanup and ordered WS events retain existing security and
  terminal behavior. Missing WAIT/wake support fails explicitly during development.
- Existing C11/C++17 examples and formal suites pass against the selected
  published SDKs; full native CI includes TSan. Android cross-build/install is
  distinguished from actual device execution. No standalone validation project.

Observe queue/admission/byte watermarks, outstanding and cancelled invocations,
queue wait, business latency, response-terminal latency and close/drain state.
Keep the owner/client/worker CPU benchmark scope separate; worker dispatch is a
semantic/isolation choice, not an unmeasured throughput improvement claim.

Rollback restores the preceding SDK/API and rebuilds consumers as one change.
It is explicit operator action, never an automatic inline or old-SDK fallback.
No user-data or wire-format migration is intended; thread placement, Interceptor
completion timing and native binding ABI do change and require release notes.
