# Reflected HTTP Application

## Public entry-point switch

Application is the public server programming model. It owns one HTTP engine,
one bounded worker pool and one CMeta ACE Configurator graph. Controllers bind
reflected Invocation actions or generated DataBind HTTP/RPC methods; Services
are injected through canonical CMeta Interfaces. Application init resolves
receivers, validates exact ABIs and publishes every configured binding before
start. Manual Server/service/worker registration remains private.

This is an intentional source/ABI migration. Exchange actions, owner-thread
business dispatch and borrowed per-method executors are removed from the public
model. There is one programming model, with no compatibility alias, execution
mode fallback or parallel versioned API. Rebuild native callers and install to
a clean SDK prefix so old headers do not survive an overlay install. Rollback
installs the preceding SDK and rebuilds those callers.

The implementation reuses the existing Server, CMeta ACE, CFlow executor,
SaltsBase thread pool, CNet/NativeIO and DataBind SDKs. It adds no dependency or
second scheduler, parser, router, Session store or plugin lease authority.

## Chosen composition

- Explicit deployments select providers and Interface projections. ACE owns
  dependency admission, creation, activation rollback and reverse destruction.
  No global registry or request-time service lookup is added.
- Each Controller names a Component stable ID and canonical receiver DataDesc.
  Its native receiver is resolved once from the activated graph. Matching size
  or a raw object pointer does not establish canonical identity or ownership.
- `CHTTP_ACTION` declares `int(void *, const chttp_invocation *)` with canonical
  Function ABI. C11 mappings and C++17 adapters validate metadata before routes
  become reachable. Callback code and metadata remain live through close.
- Global Invocation Interceptors wrap actions, generated HTTP/RPC and assets.
  Per-action hooks follow global hooks, with a combined limit of 16. Generated
  method hooks see each DTO operation separately, including RPC notifications.
- The Server remains the protocol and native terminal owner. Static security,
  JWT and CORS policy run before the business chain. Denied JWT, CORS preflight
  and protocol failures need not execute business callbacks.

Alternative designs were owner-thread business callbacks, a caller-supplied
executor per binding, and nested worker submission around the old API. The first
blocks protocol progress; the latter two split lifetime and capacity authority
and allow post-and-wait deadlocks. One Application-owned pool gives every
accepted request a charged record and a joined terminal. It also makes Bean
concurrency explicit: the default one worker serializes business callbacks;
multiple workers require thread-safe Services, Controllers and Interceptors.
A Jinja renderer is synchronous and non-reentrant: use independent instances or
explicit serialization when increasing worker count.

## Thread roles and request path

| Role | Ownership and work |
| --- | --- |
| Control thread | Activate Beans, configure/start ingress, close/drain. |
| HTTP owner | CNet/NativeIO progress, H1/H2/TLS parsing, generations, native flow control, response publication and terminal observation. |
| Application worker | Header admission, Controller/Service methods, request/method hooks, DTO binding/serialization, rendering, upload/source/WS callbacks and completion observers. |

```text
owner: headers, built-in checks, bounded record claim
  -> worker: optional custom header admission
  -> owner: body admission and bounded reception
  -> worker: before -> Controller/Service -> after or on_error
  -> owner: commit Session intent and staged response; send
  -> owner: observe native terminal
  -> worker: completion observer; release only after every owner settles
```

H1 retains ordered keep-alive/pipelined responses. H2 admits independent bounded
stream records; their business completion order may differ from arrival order.
The response always returns to the original owner. No business method waits on
its own pool. `chttp_application_is_worker` exposes scope; closing from that pool
returns `SALTS_EBUSY`.

## Invocation ownership and response staging

An Invocation and every accessor view borrow only the active worker callback.
They cannot cross threads, escape into a later callback or be retained after
return. Request headers, route parameters, body, target and JWT claims have
owned snapshots. `request->session` and `chttp_invocation_session` expose the
same detached intent view, never a live engine Session store.

Responses use copied, bounded drafts. `respond` selects buffered status/type/body;
`set_header` performs a copied case-insensitive upsert during before/action/after
unwind. Content-Type has one authoritative row in the reply arena. Reserved
transport fields fail with `EPERM`; malformed fields fail with `EINVAL`;
count/byte exhaustion fails with `ENOBUFS` without mutation. Draft size exhaustion
uses `EMSGSIZE`; selection seals the body, and a second selection is `EALREADY`.
Failed business discards the draft rather than publishing partial success.

Web rendering, redirect, HTMX and error helpers now take Invocation. They share
the existing validators and renderer with private engine conformance fixtures.
`chttp_web_authorize` belongs in a before hook. Anonymous requests stage a local
303 redirect, or 200 with HX-Redirect for exact HTMX requests. Explicit denial
marks rejection and clears proceed; other errors propagate fail-closed.

Global hooks also surround ordinary 404/405 when configured. Those unmatched
requests have no Controller operation. Without business hooks, the engine keeps
its native 404/405 path and does not compete for worker records. Assets expose
the framework asset operation. RPC request hooks see the envelope; method hooks
see individual canonical methods.

## Generated HTTP and RPC

Controller method descriptors select synchronous DIRECT, CFLOW or PLUGIN
execution on the same worker. There is no nested executor post/wait. Native
execution context comes from the canonical Controller; CFlow uses its immutable
producer-owned typed projection; Plugin uses its catalog/native capability and
a mount-owned lease. Plugin registry/code and canonical metadata must outlive
Application close. Borrowed execution descriptors are copied before publication.

Every method supplies a codec matching its MethodPlan. Init compiles owned JSON
input/output FormatPlans; the codec is borrowed only during init and may then
be destroyed. HTTP binding limits and RPC batch/method capacities remain explicit.
One required, non-null record can consume JSON body alongside scalar HTTP inputs.
Nested required records and bounded sequences of supported scalar/record/sequence
elements use SDK CMeta descriptors. Native bytes, map/set/variant/optional/null
representations remain unsupported and fail at mount, including when nested.
Nonempty body input requires application/json before method hooks run. String
views are owned through native execution, not borrowed from parser storage.

Native output remains live through method-hook unwind and serialization, then
is destroyed before input storage. Failure releases partial nested conversions
and leaves later invocations usable. RPC batches execute methods sequentially
inside one worker envelope; invalid requests, notifications, per-method errors
and notification-only 204 retain existing protocol semantics.

Method hooks use a scheduler-neutral `chttp_method_call` and result. Its
`http->session` is NULL, with a separate const detached Session for observation.
Hooks cannot mutate the enclosing request transaction through that view.
`chttp_method_call_reject` is before-only. Terminal observers receive frozen
numeric method outcomes after input/output frames are gone. No DTO/parser/frame
pointer escapes through completion facts.

## Session, files, uploads, sources and WebSocket

Session work has a charged snapshot and bounded mutation intents. Same-Session
business methods serialize through the logical Session gate, while unrelated
Sessions may run concurrently. The owner commits or aborts under the existing
Session authority. Regeneration, invalidation, cookie publication and security
helpers retain their contracts; stale Session views never outlive their record.
The gate retires after business and Session settlement, not after a long-lived
source or WS peer terminates. Completion observers do not get mutable Session
authority.

File selection stages copied paths/MIME/ETag/header decisions. Conditional/range
metadata work runs on workers; native open/read/send remains with the Server.
Owner preparation errors use publication_status; subsequent read/wire failures
use transport_status. Files and asset-root topology must remain immutable through
native completion. Asset mapping keeps bounded normalization, UTF-8 checks,
traversal/encoded-separator rejection and component-wise no-symlink/reparse
checks. SPA fallback has an explicit namespace; API/asset misses never silently
become the shell. `chttp_assets.policy` is a reflected worker Bean method with
status, immutable classification and an immediately copied ETag. There are no
raw owner callbacks in the public asset mount.

Uploads use a reflected open method, copied sink, serial worker writes, close,
final Invocation and exactly-once cleanup. Failed open transfers nothing.
Successful open transfers its resource through cleanup; close must preserve it
for the final method. The final streamed request carries the copied sink user,
byte count and trailers. Multipart validation/CSRF/commit or abort runs on the
worker. Body pause/resume and byte budgets remain in the native owner. Cleanup
waits for every admitted stage, including cancellation or a finite close failure.

Sources reserve one chunk in the reply arena before ownership transfer. Reads
and cleanup are serial worker callbacks, with zero bytes meaning EOF. Native
flow control requests only bounded next chunks; a user error is not an internal
WAIT. SSE uses this same source protocol and the existing canonical formatter.
SSE scratch and user resources remain live through exactly-once cleanup.

WebSocket opening is an Invocation: accept explicitly or stage a normal denial.
Session intents and hooks settle before handshake publication. Once accepted,
optional reflected `ready` executes on a worker before all events; its event
pointer is NULL and its peer can send a greeting. Event request.operation names
the ready or event method respectively. There is no mutable Session in either
scope. Methods are serial per peer; payload/request views expire on return.
The bounded queue includes its running event; Close has independent reserved
storage. Ready requires only fixed record state and shares the same serial
stage, so it cannot consume a data-event slot or run concurrently with Close.
Native retirement plus all accepted events/ready/Close join before completion.
A native failure that emits no Close produces one abnormal local notification;
1006 is never sent on the wire. Cross-thread sends keep the existing bounded,
generation-checked command API. Stale peer handles cannot reach a reused peer.

## Capacity, task publication and errors

`chttp_worker_config` requires positive workers, invocation capacity, retained
bytes and reply bytes. Header count/bytes are either both zero or both positive.
HTTP request limits and BindingPlan depth/item/frame bounds remain separate.
The default is one worker, 16 records, 512 KiB retained, 8 KiB replies and
32 copied headers in 4 KiB; configure product-specific limits explicitly.
The retained budget bounds overlapping snapshots, drafts, Session intents,
method reservation and source/WS storage; native protocol buffers and worker
stacks have their own owners and limits. Size/count arithmetic is checked.

Each record has at most one queued/running mutable stage. A successor is posted
only after its predecessor's last-use finalizer and owner readiness join. The
executor queue derives from record capacity, reserving room for admitted internal
transitions and completion notifications. A rejected post transfers no payload.
After a successful successor publication, the predecessor no longer touches
record storage. Generations and terminal joins prevent recycling while a native
producer, callback or frame still owns it.

External record/byte exhaustion rejects admission with bounded 503; explicit
rate limits remain 429. Invalid input follows existing 4xx semantics. Configuration
or ABI errors fail init and roll back. An impossible internal completion-post
failure stops admission and retains unsettled ownership for diagnosis; it never
becomes a successful response, dropped callback or inline execution fallback.
No failed operation is automatically retried.

## Cancellation, deadlines and close

Queue waiting consumes the existing phase deadline. body_ms includes custom
header admission and body reception; handler_ms starts after body completion and
covers business queue/hooks/method. Response transfer retains its native budget.
The migration does not invent a new whole-request timeout or extend those limits.

Cancellation does not establish completion. Queued business skips user code after
an observed failed native terminal. Running methods may query cancellation but
cannot be killed safely. Timeout/RST/disconnect invalidates response publication;
it does not release worker-visible data or undo external side effects. A late
response is discarded without re-executing its method.

Completion runs once after business, Session/publication and native transport
settlement, plus source/upload/WS resource and producer joins. Business,
publication, Session and transport statuses remain distinct. Native send success
means local I/O completion, not remote application consumption.

Close uses one absolute budget: stop ingress, settle native and worker owners,
run reserved completion callbacks, drain the pool, destroy protocol/DTO storage,
then stop the ACE graph. A finite timeout retains every live owner for an
explicit later close. Zero waits indefinitely. A fully cleaned operational error
can return with impl NULL; callers must inspect status. C11 `with` reports
operation and close separately and never closes a preexisting owner.

C++17 Application is noncopyable/nonmovable and terminates if an unhandled
fallible destructor close cannot settle. Explicit close lets callers handle the
status. Bean factories return exactly unique_ptr<T>; derived/custom-deleter
ownership cannot be erased into delete T. Exact canonical DataDesc identity,
size and alignment are checked. Throwing action/admission/policy/event/source/
sink adapters unwind C++ locals before translating to SALTS_EIO; lifecycle,
Interceptors and cleanup callbacks follow the no-throw/longjmp contract.

## Migration and formal acceptance

C11/C++17 Application examples and the OpenAPI UI use public descriptors only.
Application tests verify public init, injected Service execution on workers,
global hooks, Web helpers, assets, uploads and WebSocket greeting. Private
registration fixtures additionally exercise transactional ABI rejection, copied
bindings, malformed frames, capacity, cancellation, partial conversions,
finite close and real native terminals. Native Web fixtures remain internal
protocol conformance tests, not an alternative public programming model.

Build the complete graph and run formal CTest with the existing user preset;
installation uses the project's normal install step. No separate consumer or
smoke project is introduced. CI qualifies all existing OS/compiler/sanitizer
matrix configurations, including ASan/UBSan and TSan. Actual run results belong
in the PR; this design does not claim an unexecuted check passed.

## Cooperative await extension

This baseline invokes synchronous user methods on the pool. It does not promise
stackful yield, arbitrary suspension of a C stack or unlimited concurrent blocking
calls. A future async contract must give external completion/cancellation handles
explicit ownership, bound suspended work and resume only on the designated
worker, with formal race and shutdown tests before exposing an API. CFlow
executor primitives alone do not establish those Application guarantees.
