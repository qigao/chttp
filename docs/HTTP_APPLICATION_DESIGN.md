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
