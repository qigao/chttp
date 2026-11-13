# CHttp::App

`CHttp::App` is CHTTP's native-C application module. One `chttp_app` shared
library implements generated IDL services and server-driven Web applications.

> **Product claim:** `CHttp::App` combines CHTTP routing, middleware,
> sessions, security, deferred responses, streaming, HTTP/1.1 and HTTP/2 with
> typed CMeta models and Jinja CMeta server-side rendering.

It is intended for admin UIs, dashboards, developer portals, CRUD/internal
tools, device consoles, and hybrid HTML/API services.

It is **not** a client-side framework, virtual DOM, browser runtime, or
Wt-style server widget toolkit. Browser behavior remains ordinary HTML plus
optional progressive enhancement such as HTMX.

## Architecture boundary

```text
Application
    |
    v
CHttp::App
    |-----------------------------.
    v                             |
CHttp::Server                     |
routes / middleware / sessions    |
JWT / CORS / rate limits          |
deferred / streaming / H2         |
    |                             |
    '----------> CMeta <----------'
                  |
                  v
             Jinja CMeta
             SSR / fragments
                  |
                  v
               Browser
```

The dependency direction is deliberate:

- `CHttp::Server` does not depend on Jinja, HTMX, or UI code.
- Jinja CMeta does not depend on CHTTP request/session types.
- `CHttp::App` is the bridge and may depend on both.
- Existing CHTTP routing, middleware, sessions, JWT, CORS, rate limiting,
  deferred replies, streaming, HTTP/2, WebSocket, file serving, and statistics
  remain owned by CHTTP rather than being reimplemented here.

## Package use

The installed CMake package exports one `CHttp::App` shared-library target.
Service and Web are capabilities within App, not separate libraries or build
targets. See the [merge and migration decision](../README.md#app-entry-point).

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_web_app LANGUAGES C)

find_package(Chttp CONFIG REQUIRED PATHS "$ENV{CHTTP_ROOT}" NO_DEFAULT_PATH)

add_executable(my_web_app main.c)
target_link_libraries(my_web_app PRIVATE CHttp::App)
```

Applications include the public entry point:

```c
#include <chttp_app/app.h>
```

App initialization, parsing, validation, uploads, generated MethodPlan,
deferred execution and DSO behavior are verified by the formal integration
tests under `app/tests/service` and `app/tests/web`. Use the root user preset
and CTest entries described in [the build guide](../cmake/BUILD.md).

## Application, Controller and Service

`chttp_application` owns one protocol engine and a real CMeta ACE Configurator
graph. Configure explicit Component deployments and Interface selections;
ACE resolves dependencies before creating Beans, injects canonical Interface
projections, and destroys dependents before their providers. No global registry
or per-request dependency lookup is used.

Each `chttp_controller` identifies a Component by stable ID and canonical
receiver DataDesc. `CHTTP_ACTION(name)` declares the reflected
`int(void *, const chttp_invocation *)` callback; `CHTTP_GET` / `CHTTP_POST`
bind it to a route. Init activates Beans, validates exact ABI, creates the
bounded worker pool and publishes configured bindings before start.

The callback receives an ACE-owned receiver and worker-scoped Invocation.
Request/parameter/header/JWT views cannot escape it. Responses use
`chttp_invocation_respond`, copied headers and file/source decisions.
`request->session` and `chttp_invocation_session` expose the same detached
Session intent view; the owner commits changes and publishes the response.

Services use normal CMeta Components with provided/required Interfaces. The
[public tests](tests/application_test.c), [C example](examples/application.c)
and [C++17 example](examples/application.cpp) demonstrate dependency injection,
worker execution and ordered Interceptors.

`chttp_invocation_interceptor` resolves a Bean with before/after/error hooks.
Global hooks precede local hooks (16 combined); ACE unwinds in reverse order.
Before hooks reject with `chttp_invocation_reject`. Unhandled failure retains
HTTP 500 behavior. Global hooks also wrap generated HTTP/RPC, assets and
unmatched 404/405. Assets expose a framework operation; unmatched requests have
NULL operation. Static security/CORS/JWT denial precedes the business chain.

Generated `http_methods` / `rpc_methods` supply immutable MethodPlans,
binding/execution metadata and a matching codec. Init compiles owned input/output
FormatPlans; the codec may then be destroyed. Configure HTTP binding and RPC
capacities explicitly. DIRECT, CFLOW and PLUGIN execute on the owned pool without
nested post/wait or a borrowed executor. CFlow uses its typed projection; Plugin
retains its provider lease. Registries and canonical metadata outlive close.
Method-specific hooks observe DTO calls, including RPC notifications, separately
from the enclosing request hooks.

Custom admission, asset policies, upload open/body/close/cleanup, source reads,
SSE producers, WebSocket methods and terminal observers also run on workers.
`chttp_admission` and `chttp_assets.policy` resolve reflected canonical Beans.
`chttp_action.upload` transfers successful sinks through exactly-once cleanup.
WebSocket opening explicitly accepts an upgrade; optional `ready` can send a
greeting after handshake before serial events (`scope.event == NULL` for ready).
Those scopes have no mutable Session and use generation-checked bounded sends.

`config.workers` sets record/byte/reply/header limits. Defaults use one worker;
multiple workers require thread-safe Beans and renderer ownership. Exhaustion
rejects with 503 before business. HTTP capacities, TLS, socket/placement,
Session, security, JWT and admission remain explicit policies.

This is a source/ABI migration: rebuild consumers, install into a clean SDK
prefix and replace Exchange/native response usage with Invocation. No public
manual mount, borrowed executor or parallel legacy API is retained. See the
[ownership and migration decision](../docs/HTTP_APPLICATION_DESIGN.md).

Use `chttp_application_init/start/port/close` or C11
`chttp_application_with`. The latter reports operation and cleanup status
separately, never closes a pre-existing owner, and returns unsettled ownership
for explicit cleanup. A zero close timeout waits without a deadline; a finite
timeout retains a live owner when work remains. Close stops ingress, drains
real I/O and DTO task finalizers, destroys protocol storage, then releases the
ACE graph. User provider stop/destruction callbacks must terminate themselves;
the framework cannot preempt them. Metadata remains borrowed until close settles.
Application copies Security, CORS and asset policy structures during init, so a
configuration builder may reuse those structures after returning. Their referenced
strings, origin arrays, callback code and contexts remain borrowed and immutable
until close settles. JWT validators remain borrowed; admission receivers belong to the ACE graph.

C++17 `chttp::Application` is noncopyable/nonmovable and closes on destruction.
Call `close()` explicitly to handle errors; an unhandled destructor cleanup
error terminates. `chttp::Bean<T, Factory>` owns factory-created objects with
`unique_ptr` and canonical ObjectRef lifecycle. Factory exceptions translate at
the C boundary. `CHTTP_CPP_GET` catches action exceptions before returning to
C and reports a native failure; C callbacks must return normally.

Bean factories must return exactly `std::unique_ptr<T>` by value. A custom
deleter, pointer-like return or derived-pointer result cannot be erased into
Bean's `delete T` lifecycle; use an explicit ACE provider lifecycle for those
cases. The supplied DataDesc must describe T. Bean rejects invalid descriptors
or storage size/alignment mismatches before invoking the factory. Semantic
descriptor identity remains the caller's explicit metadata responsibility.

Buildable entry points: [C11](examples/application.c),
[C++17](examples/application.cpp), and
[OpenAPI UI](../openapi/examples/ui_server.c). Link only `CHttp::App` for the
server application. Old public `<chttp_app/service.h>` and
`<http_server/rpc_service.h>` are removed. Manual Server registration is private;
existing manual Web programs are retained under `tests/web/fixtures` solely
for protocol/security regression coverage.

The public C11/C++17 examples share
[canonical declarations](examples/application_contract.h): AnswerService provides
the ExampleAnswer Interface, GreetingController receives that dependency from
ACE, and a separate DispatchPolicy Bean supplies an Interceptor. `/answer`
returns the injected value as JSON; `/reject` demonstrates an explicit 403 and
does not represent authentication. `X-Example-Interceptor: ACE` makes chain
entry observable, including on unmatched routes. Press Enter for checked scoped
or RAII shutdown. The module's formal CTest entries start both actual programs,
exercise requests and verify successful process shutdown under `BUILD_TESTS`.

Rendering remains synchronous and non-reentrant. An injected rendering Service
must serialize access or own independent renderers for concurrent workers.
The remaining Web sections describe reusable request/render/security primitives;
their fixture code uses the private engine to isolate those behaviors.

## Core model

A Web application normally composes these pieces:

1. Initialize one or more bounded `chttp_web_renderer` instances from a fixed
   application-owned template bundle.
2. Declare reflected Controller actions and ACE Interceptors on Application.
3. Build typed CMeta models in route handlers.
4. Render a full page or fragment with `chttp_web_render_response()`.
5. Use `chttp_web_request_context_init()` when templates need selected
   request parameters, headers, session values, or HTMX state.
6. Parse and bind bounded form input with the form APIs.
7. Apply CSRF validation to cookie/session-authenticated mutation routes.
8. Execute business on the Application pool and use worker sources/SSE for streams.
9. Stop/drain the server and worker execution before destroying renderers.

Templates are frozen into the renderer. Request-controlled filesystem template
loading is not part of the default product surface.

## Browser authentication and application shell

Web III adds browser-authenticated application composition without turning
CHttp::App into an identity provider.

The intended flow is:

```text
anonymous browser
    |
    +-- GET /login
    |      -> Session + CSRF token
    |
    +-- POST /login
    |      -> bounded form parse
    |      -> application verifies credentials
    |      -> Session ID regeneration
    |      -> principal publication
    |      -> CSRF rotation
    |      -> safe local redirect / HX-Redirect
    |
    +-- protected route
    |      -> chttp_web_authorize
    |      -> Session principal
    |      -> optional application authorization callback
    |      -> page / fragment
    |
    '-- POST /logout
           -> CSRF validation
           -> Session invalidation
           -> expired cookie
```

Credential storage, password hashing, account policy, MFA, user databases, and
OIDC/SAML/OAuth provider logic remain application or integration concerns.
CHttp::App never verifies passwords by itself. The application supplies a
verified identity to `chttp_web_principal_sign_in()`.

### Session fixation and CSRF

A successful privilege transition must not reuse the anonymous Session
identifier.

`chttp_web_principal_sign_in()` validates CSRF, regenerates the CHTTP Session
identifier, publishes the principal only under the regenerated Session, then
rotates CSRF. If principal publication or CSRF rotation fails after
regeneration, the Session is invalidated rather than leaving partially
authenticated state.

The old Session identifier stops authenticating immediately after successful
regeneration. Logout validates CSRF and invalidates the complete Session, so a
stale authenticated cookie does not keep principal state alive.

Do not copy authentication state into application cookies as a workaround for
Session lifecycle. Keep the CHTTP Session as the browser principal carrier and
treat principal views as request-scoped borrowed data.

### Protected routes and authorization

`chttp_web_authorize(invocation, policy, proceed)` runs in an Invocation before
hook. Handled denial stages a response, marks rejection and clears proceed;
propagate every other error. Its authorization callback requires an
authenticated Session principal.

Callback semantics are explicit:

- `SALTS_OK` — authorize and continue to the protected handler;
- `SALTS_EPERM` — explicit authorization denial;
- every other error — fail closed and propagate as a server error.

The default authorization-denied response is HTTP 403. Applications may supply
a forbidden handler for rendered UX. Templates never make authorization
decisions.

JWT bearer admission remains a `CHttp::Server` capability. Browser Session
authorization and JWT bearer authorization are separate composition choices;
CHttp::App does not translate one into the other.

### Redirect safety

Post-login return targets are optional and always local.

`chttp_web_local_target_validate()` accepts only bounded origin-form targets
that begin with exactly one `/`. It rejects absolute URLs, scheme-relative
targets, controls, fragments, backslashes, malformed percent escapes, and
encoded bytes that can change authority/path interpretation.

When `include_return_target` is enabled, auth middleware percent-encodes the
validated request target into the configured login URL. It never constructs a
redirect from the request `Host` or `Origin` header.

Ordinary unauthenticated browser requests receive a 303 redirect. Exact HTMX
requests receive the same local destination through `HX-Redirect` on a
non-3xx response.

Applications should validate a submitted `return_to` again in the login
handler before the privilege transition. The authenticated reference
application does so before calling `chttp_web_principal_sign_in()`.

### Static assets and application shell

Set `config.assets` to a `chttp_assets` descriptor with a mount and optional
reflected worker policy. CHTTP retains native file streaming, conditional/range
policy and terminal ownership. Init copies mount and hook rows; strings and
canonical metadata remain borrowed through close.

A mount:

- owns one explicit URL prefix and one application-owned filesystem root;
- admits GET and HEAD only;
- percent-decodes into bounded storage and validates UTF-8;
- rejects traversal, encoded separators, controls, backslashes, drive-like
  separators, symlink components, and Windows reparse points;
- returns deterministic 404 for malformed, escaping, missing, or rejected
  paths;
- may apply conservative or immutable Cache-Control policy;
- may supply an application-selected strong ETag, otherwise inheriting CHTTP
  metadata validators.

SPA fallback is opt-in under a separate configured namespace. An asset miss or
an unrelated API miss is never silently converted into the application shell.

The current path-based CHTTP file API requires the selected file to remain
immutable through asynchronous completion. Therefore the asset root and its
topology must remain immutable from server start until stop. This is not a
mutable virtual filesystem API.

### Authenticated reference application

`app/tests/web/fixtures/chttp_web_authenticated_app.c` is the Web III reference
application. It uses only bounded in-memory demo identity records so the
example demonstrates browser composition rather than database or password
infrastructure.

It covers anonymous login, bounded form + CSRF, application credential
verification, Session regeneration and CSRF rotation, protected and denied
routes, ordinary and HTMX transitions, local return-target validation, static
CSS through the asset mount, Jinja CMeta full-page/fragment rendering, hostile
principal autoescape, and CSRF-protected logout.

Build and run it interactively:

```text
cmake --build build/linux-gcc-debug --target chttp_web_authenticated_app_example
build/linux-gcc-debug/bin/chttp_web_authenticated_app_example
```

The same executable has a deterministic qualification mode:

```text
build/linux-gcc-debug/bin/chttp_web_authenticated_app_example --self-test
```

That mode drives a real CHTTP client through H1 and H2 and verifies stale
pre-login/post-logout cookies, redirect safety, protected-handler
non-execution, authorization denial, asset serving, autoescape, and HTMX
progressive fragments.

## Reference applications

These Web fixtures exercise private native engine adapters and shared helper
semantics. Use the public Application examples above for new services; native
registration/builders from these fixtures are not installed APIs.

The repository keeps reference programs under `app/examples/`. They are part
of the normal example build and must remain independently compilable.

| Example | What it demonstrates |
| --- | --- |
| `chttp_web_example.c` | Minimal SSR, typed CMeta model, template inheritance, HTML autoescape, normal CHTTP route lifecycle |
| `chttp_web_context_example.c` | Typed request context, route params, selected headers/session values, full-page vs HTMX fragment rendering, HX response helpers |
| `chttp_web_crud_example.c` | Bounded in-memory CRUD, typed user sequence, ordinary form fallback, HTMX-compatible fragment mutation, session CSRF, conservative Web security policy |
| `chttp_web_session_example.c` | Bounded form parsing, session-backed CSRF token lifecycle, mutation validation, flash messages |
| `chttp_web_security_example.c` | Web security middleware composed with rate limiting, CORS, JWT bearer routes, explicit header override |
| `chttp_web_deferred_example.c` | Private engine defer/generation conformance with bounded worker rendering |
| `chttp_web_form_upload_example.c` | Structured validation, ordinary + HTMX error UX, multipart upload staging, Session CSRF, flash/redirect success, hostile metadata escaping |
| `chttp_web_authenticated_app.c` | Complete browser-authenticated flow: login, Session regeneration, principal/authz middleware, safe return targets, asset mount, Jinja full-page/fragment rendering, logout, stale-cookie and autoescape qualification |

Configure with examples enabled and build the desired target:

```text
cmake --preset native-release-user
cmake --build --preset native-release-user
ctest --preset native-release-user --no-tests=error
```

The repository presets require the same configured Salts, SaltsUtils, vcpkg,
and project roots documented in the top-level README.

## SSR and fragments

`chttp_web_render_response()` renders completely before the HTTP response is
committed. A render failure therefore cannot publish a partial HTML response.

The renderer is bounded by:

- maximum template bytes;
- maximum output bytes;
- maximum nodes/value visits;
- maximum render depth.

The same HTML autoescape policy applies to full-page and fragment rendering.

`chttp_web_context_example.c` shows the normal progressive-enhancement
pattern:

- ordinary request -> full page;
- `HX-Request: true` -> fragment;
- `HX-Trigger`, `HX-Retarget`, and `HX-Redirect` remain response-header
  conveniences rather than a second protocol stack.

## HTMX CRUD reference flow

The executable `chttp_web_crud_example.c` is the compact reference application.
It stores a bounded user set in memory so the example can focus on the Web
contract rather than a database layer. The rendered forms work as ordinary
HTML and also carry `hx-post`, `hx-target`, and `hx-swap` attributes for
progressive enhancement.

A server-driven CRUD application can stay on ordinary CHTTP routes:

| Route | Server behavior |
| --- | --- |
| `GET /users` | Render the full users page |
| `GET /users/:id` | Render full detail for an ordinary request or a detail fragment for HTMX |
| `POST /users` | Parse a bounded form, validate CSRF, bind a typed model, perform the application write, push a flash message, then redirect or emit an HX refresh trigger |
| `POST /users/:id/delete` | Validate CSRF, perform the delete, then return an ordinary redirect or HTMX fragment/trigger |
| `GET /events` | Optional SSE stream for live refresh notifications |

The Web layer intentionally does not own persistence or business transactions.
Validation and database/domain writes belong to the application. The Web layer
owns the HTTP-facing parsing, typed presentation context, browser security
helpers, and rendering bridge.

`chttp_web_crud_example.c` composes these mechanics end to end. The smaller
`chttp_web_context_example.c` and `chttp_web_session_example.c` remain useful
when studying request/fragment and session/form concerns independently.

## Forms, sessions, CSRF, and flash messages

`application/x-www-form-urlencoded` parsing is bounded and allocation-free
from the parser's point of view: the caller supplies pair storage and decoded
byte storage.

表单绑定统一使用 `chttp_web_form_bind_method_plan()` 与生成的 DataBind
HTTP MethodPlan。旧 typed/descriptor 入口及 JSON 中转存储已删除，不提供兼容接口。
标量、枚举与重复字段的标量集合直接转换为 cserde token；字段名称、默认值、
校验、集合构造及释放由 BindingPlan/CMeta 描述符统一负责。对象、map 和嵌套集合被拒绝。

调用者提供有界 `DataBindNativeOptions` 工作区、标量 scratch 和未持有资源的暂存帧。
绑定失败时，已初始化的帧字段恢复为语义空值；绑定成功后再提交业务写入，并按生成类型的
生命周期接口释放暂存资源。不能向绑定接口传入已有业务对象。完整示例与失败回归见
[`test_chttp_web_form_plan.c`](tests/test_chttp_web_form_plan.c)。

For cookie/session-authenticated mutations:

- call `chttp_web_csrf_ensure()` to make a token available;
- expose it through the request context/template;
- submit it as `_csrf` or, for an exact HTMX request, as
  `X-CSRF-Token`;
- call `chttp_web_csrf_validate()` before the application mutation;
- rotate or clear it when the application's session lifecycle requires it.

JWT-only API routes do not become CSRF-protected automatically; they remain
outside CSRF scope unless the application explicitly composes session/browser
semantics.

Flash messages are bounded session state and are consumed explicitly. They are
not a second persistence mechanism.

### Structured validation

`chttp_web_validation` is a request-local, caller-storage-backed result that
can be rendered directly through its CMeta descriptor. Applications may add
field errors and global errors after parsing/binding and after domain checks.

The validation object:

- never allocates hidden storage;
- preserves deterministic insertion order;
- copies field/message bytes into caller-supplied bounded storage;
- can be reset/reused between requests;
- exposes `valid` plus a typed error sequence to templates;
- does not interpret application/domain rules.

A failed validation result is presentation state, not an HTTP transport error.
Ordinary browser requests can re-render the complete page while exact HTMX
requests can render only the affected fragment. The same Jinja HTML autoescape
policy applies to validation messages and re-populated values.

### Streaming multipart uploads

`multipart/form-data` uses the incremental
`chttp_web_multipart_parser` and `chttp_web_upload_request` transaction on
top of CHTTP's existing route body-sink API.

The ownership chain is:

```text
application-owned bounded upload slot
    |
body_open
    v
chttp_web_upload_request
    |
    +-- incremental multipart parser
    +-- reserved bounded _csrf capture
    +-- application staging callbacks
    |
body_close
    v
terminal handler via request.body_sink_user
    |
    +-- CSRF validation
    +-- structured/application validation
    +-- commit exactly once
    '--- or abort exactly once
```

Important limits and lifecycle rules:

- multipart boundary length is capped by the MIME 70-byte maximum;
- part count, header count/bytes, field bytes, filename bytes, content-type
  bytes, and total body bytes are all explicit bounds;
- request body bytes and part metadata views are callback-scoped;
- application staging callbacks run synchronously on Application workers and
  must terminate for close to settle;
- disk/database/remote persistence needs explicit worker/resource limits;
- `part_end` closes only one staged part and never means final application
  commit;
- the final commit happens only after the complete body, CSRF, and application
  validation succeed;
- parser, sink, disconnect, server-stop, CSRF, validation, or commit failures
  abort staging exactly once;
- caller-owned upload state may be reused only after commit or abort.

The full browser-oriented composition is
`app/tests/web/fixtures/chttp_web_form_upload_example.c`. It uses a fixed application
slot pool and bounded in-memory staging deliberately; CHttp::App does not
provide a hidden upload directory or persistence layer.

## Worker rendering

Application runs Controller methods on its bounded pool. Render a typed model
with `chttp_web_render_response(renderer, invocation, ...)`; output is copied
before the renderer buffer is released. There is no public manual defer step.

Use one renderer per serialized worker, independent instances for parallel
workers, or explicit application synchronization. Invocation views cannot be
retained by later producers. Cancellation preserves model/renderer lifetime
until running business and native ownership settle.

The older `chttp_web_deferred_example.c` fixture directly tests private engine
defer/generation contracts. It is not the public Application usage pattern.

## SSE route

`CHttp::App` adds a thin SSE layer over CHTTP response streaming rather than
a second streaming runtime.

A `GET /events` route should:

1. keep caller-owned `chttp_web_sse_stream` state alive for the response;
2. provide a synchronous bounded `next` callback;
3. initialize the stream with `chttp_web_sse_stream_init()`;
4. select it with `chttp_web_sse_response(invocation, stream, error)`;
5. release application stream state from the exactly-once close callback.

`app/tests/web/test_chttp_web_sse.c` is the executable protocol reference. It
proves canonical event formatting, newline normalization, the same finite event
sequence over HTTP/1.1 and HTTP/2, and exactly-once cleanup after disconnect.

SSE and WebSocket are independent choices. Application configures reflected opening/ready/event methods; the Server/CNet
engine retains framing and transport ownership.

## Security deployment guidance

Jinja HTML autoescape and HTTP/browser security policy are separate guarantees.
Use both where appropriate.

For browser-facing applications:

- keep credential verification, password hashing, MFA and identity-provider
  integration in the application/integration layer;
- regenerate the Session identifier before publishing authenticated principal
  state;
- validate login return targets as bounded local origin-form targets and never
  reflect Host/Origin into redirects;
- keep authorization in explicit callbacks and treat callback errors as
  fail-closed;
- keep static asset roots/topology immutable while serving them, reject
  symlink/reparse escape, and scope SPA fallback to an explicit namespace;
- install `chttp_web_security_reference_policy()` as a conservative
  same-origin baseline;
- use `chttp_web_security_authenticated_policy()` for authenticated pages
  that should also be private/no-store;
- enable HSTS explicitly only when HTTPS is actually enforced at the browser
  boundary;
- register Web security middleware before CORS if security headers are required
  on CORS preflight responses;
- keep CSP widening explicit, especially `connect-src` for cross-origin
  browser operations;
- apply CSRF validation to session-authenticated mutation routes;
- use CHTTP admission/rate limiting and JWT middleware for the routes that need
  them;
- preserve configured request/response/template/body limits rather than
  replacing them with unbounded buffers;
- never select a filesystem template path directly from request input.
- treat multipart filenames and per-part `Content-Type` values as untrusted
  metadata; they are presentation hints, not authorization or path decisions;
- normalize/sanitize any application-selected persistence name independently
  and never join a client filename directly to a filesystem path;
- keep upload authorization, quota/accounting, persistence, retention, and
  rollback policy in the application;
- antivirus, malware scanning, content disarm/reconstruction, MIME sniffing,
  and domain-specific content inspection are outside CHttp::App and must run in
  the application's staging/commit pipeline where required.

The security reference application is
`app/tests/web/fixtures/chttp_web_security_example.c`.

## Renderer ownership and lifecycle

A renderer is synchronous and non-reentrant.

Safe models include:

- one renderer owned by one application worker;
- one renderer per parallel worker;
- an externally serialized renderer when the application accepts that
  serialization point.

Do not overlap init, render, or destroy for the same renderer.

The lifecycle qualification in PR #45 covers deterministic rendering,
request isolation, repeated server start/stop/destroy, deferred lifecycle, SSE
cleanup, full regression, AddressSanitizer, and UndefinedBehaviorSanitizer.

## OpenAPI UI qualification application

The OpenAPI documentation UI runs on the generic Web layer and serves as a
real application workload for:

- full-page SSR;
- fragment SSR;
- typed CMeta models;
- hostile-string escaping;
- HTMX interaction;
- browser-only Try-it behavior;
- repeated request isolation;
- CHTTP routing and lifecycle.

OpenAPI-specific UI code should not bypass the generic Web rendering/security
boundary.

## Qualification evidence

The product-readiness gates are tracked by issue #28:

- #40 / PR #45 — correctness, lifecycle, full regression, ASan and UBSan;
- #41 / PR #44 — install/export and standalone installed-package consumer;
- #42 / PR #46 — reproducible renderer/OpenAPI benchmark evidence;
- #43 — reference applications, product documentation, and final claim.

Benchmark evidence records observations without introducing unsupported
performance thresholds.

### Web III browser-authentication evidence

The browser-authenticated application-shell expansion is tracked by #68:

- #69 / PR #75 — Session ID regeneration for privilege transitions;
- #70 / PR #76 — bounded browser principal plus login/logout helpers;
- PR #77 — SaltsUtils component/package ownership and current-master CI
  prerequisite;
- #71 / PR #78 — protected-route authentication/authorization middleware and
  bounded safe return-target semantics;
- #72 / PR #79 — static asset mount with traversal, UTF-8,
  symlink/reparse-root escape, range/conditional, H1/H2, and cache-policy
  qualification;
- #73 / PR #80 — independently runnable authenticated reference application
  with real-client self-test.

The final #74 gate reruns the complete focused Web suite, authenticated
reference smoke, full CTest, ASan, UBSan, exact-head clean-source check, and
the installed-package out-of-tree consumer against current Salts and
SaltsUtils `master` heads.

The installed consumer uses only:

```cmake
find_package(Chttp CONFIG REQUIRED)
target_link_libraries(app PRIVATE CHttp::App)
```

and links representative principal, auth middleware, local-target, and asset
mount public APIs without creating a second DataBind package or source-tree
fallback.

### Web II forms/upload evidence

The forms/upload expansion is tracked by #55:

- #56 / PR #61 — structured validation/error bag;
- #57 / PR #62 — bounded incremental multipart parser;
- #63 / PR #64 — per-request streamed-body context continuity;
- #58 / PR #65 — upload staging transaction, CSRF-safe commit, H1/H2 and
  disconnect/stop cleanup;
- #59 / PR #66 — complete ordinary + HTMX validation/upload reference
  application;
- #60 — final installed-package, sanitizer, regression, documentation, and
  security qualification.

Web II extends the product surface; it does not reopen or weaken the completed
base CHttp::App readiness gate.

## Product boundary

`CHttp::App` is a native-C **server-driven** application layer. It does not
provide:

- a virtual DOM;
- a browser-side state runtime;
- a React/Vue-style client framework;
- a Wt-style server widget tree;
- an ORM;
- a JavaScript build pipeline;
- a second router, session store, or event loop.

Those non-goals keep the package composable: CHTTP remains the HTTP/application
infrastructure, CMeta remains the typed data boundary, and Jinja CMeta remains
the rendering engine.
