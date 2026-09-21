# CHttp::Web

`CHttp::Web` is the optional native-C server-driven web application layer in
CHTTP.

> **Product claim:** `CHttp::Web` combines CHTTP routing, middleware,
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
CHttp::Web
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
- `CHttp::Web` is the bridge and may depend on both.
- Existing CHTTP routing, middleware, sessions, JWT, CORS, rate limiting,
  deferred replies, streaming, HTTP/2, WebSocket, file serving, and statistics
  remain owned by CHTTP rather than being reimplemented here.

## Package use

The installed CMake package exports `CHttp::Web`.

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_web_app LANGUAGES C)

find_package(Chttp CONFIG REQUIRED)

add_executable(my_web_app main.c)
target_link_libraries(my_web_app PRIVATE CHttp::Web)
```

Applications include the public entry point:

```c
#include <chttp_web/web.h>
```

The standalone package qualification in PR #44 proves that an out-of-tree
consumer can configure with `find_package(Chttp CONFIG REQUIRED)`, link only
`CHttp::Web`, build, and run without source-tree fallback.

## Core model

A Web application normally composes these pieces:

1. Initialize one or more bounded `chttp_web_renderer` instances from a fixed
   application-owned template bundle.
2. Register ordinary CHTTP routes and middleware.
3. Build typed CMeta models in route handlers.
4. Render a full page or fragment with `chttp_web_render_response()`.
5. Use `chttp_web_request_context_init()` when templates need selected
   request parameters, headers, session values, or HTMX state.
6. Parse and bind bounded form input with the form APIs.
7. Apply CSRF validation to cookie/session-authenticated mutation routes.
8. Use deferred replies for blocking business work and SSE for event streams.
9. Stop/drain the server and worker execution before destroying renderers.

Templates are frozen into the renderer. Request-controlled filesystem template
loading is not part of the default product surface.

## Browser authentication and application shell

Web III adds browser-authenticated application composition without turning
CHttp::Web into an identity provider.

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
    |      -> chttp_web_auth_middleware
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
CHttp::Web never verifies passwords by itself. The application supplies a
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

`chttp_web_auth_middleware()` requires an authenticated Session principal and
may invoke an application-owned authorization callback.

Callback semantics are explicit:

- `SALTS_OK` — authorize and continue to the protected handler;
- `SALTS_EPERM` — explicit authorization denial;
- every other error — fail closed and propagate as a server error.

The default authorization-denied response is HTTP 403. Applications may supply
a forbidden handler for rendered UX. Templates never make authorization
decisions.

JWT bearer admission remains a `CHttp::Server` capability. Browser Session
authorization and JWT bearer authorization are separate composition choices;
CHttp::Web does not translate one into the other.

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

`chttp_web_assets_use()` is a thin URL-to-file mapping layer over
`chttp_server_serve_file()`; CHTTP remains the file streaming, conditional
request, range, metadata, and async-I/O owner.

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

`web/examples/chttp_web_authenticated_app.c` is the Web III reference
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

The repository keeps reference programs under `web/examples/`. They are part
of the normal example build and must remain independently compilable.

| Example | What it demonstrates |
| --- | --- |
| `chttp_web_example.c` | Minimal SSR, typed CMeta model, template inheritance, HTML autoescape, normal CHTTP route lifecycle |
| `chttp_web_context_example.c` | Typed request context, route params, selected headers/session values, full-page vs HTMX fragment rendering, HX response helpers |
| `chttp_web_crud_example.c` | Bounded in-memory CRUD, typed user sequence, ordinary form fallback, HTMX-compatible fragment mutation, session CSRF, conservative Web security policy |
| `chttp_web_session_example.c` | Bounded form parsing, session-backed CSRF token lifecycle, mutation validation, flash messages |
| `chttp_web_security_example.c` | Web security middleware composed with rate limiting, CORS, JWT bearer routes, explicit header override |
| `chttp_web_deferred_example.c` | Owner-thread request-state copy, response defer, bounded Salts worker queue, worker-side render and generation-checked deferred reply |
| `chttp_web_form_upload_example.c` | Structured validation, ordinary + HTMX error UX, multipart upload staging, Session CSRF, flash/redirect success, hostile metadata escaping |
| `chttp_web_authenticated_app.c` | Complete browser-authenticated flow: login, Session regeneration, principal/authz middleware, safe return targets, asset mount, Jinja full-page/fragment rendering, logout, stale-cookie and autoescape qualification |

Configure with examples enabled and build the desired target:

```text
cmake --preset linux-dev-user -DBUILD_EXAMPLES=ON
cmake --build build/linux-gcc-debug --target chttp_web_example
cmake --build build/linux-gcc-debug --target chttp_web_context_example
cmake --build build/linux-gcc-debug --target chttp_web_crud_example
cmake --build build/linux-gcc-debug --target chttp_web_session_example
cmake --build build/linux-gcc-debug --target chttp_web_security_example
cmake --build build/linux-gcc-debug --target chttp_web_deferred_example
cmake --build build/linux-gcc-debug --target chttp_web_form_upload_example
cmake --build build/linux-gcc-debug --target chttp_web_authenticated_app_example
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

Typed binding is transactional through DataBind-compatible descriptors. The
destination is not partially mutated when the bridge/bind operation fails.

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
- application staging callbacks run synchronously on the CHTTP owner thread and
  must not perform unbounded blocking;
- disk/database/remote persistence must use application-owned bounded
  queueing/worker policy when blocking work is required;
- `part_end` closes only one staged part and never means final application
  commit;
- the final commit happens only after the complete body, CSRF, and application
  validation succeed;
- parser, sink, disconnect, server-stop, CSRF, validation, or commit failures
  abort staging exactly once;
- caller-owned upload state may be reused only after commit or abort.

The full browser-oriented composition is
`web/examples/chttp_web_form_upload_example.c`. It uses a fixed application
slot pool and bounded in-memory staging deliberately; CHttp::Web does not
provide a hidden upload directory or persistence layer.

## Deferred worker rendering

CHTTP route handlers run on the server owner thread and must not block on
application work.

The supported pattern is:

```text
owner thread
    |
    +-- validate/authenticate
    +-- copy only the request values the worker needs
    +-- reserve bounded worker capacity
    +-- chttp_server_response_defer()
    |
    v
application worker
    |
    +-- business/database work
    +-- build typed model
    +-- chttp_web_deferred_render_reply()
```

`chttp_web_deferred_example.c` demonstrates this with a bounded
`salts_threadpool`. It deliberately uses one renderer with one worker thread,
which satisfies the renderer's synchronous non-reentrant ownership contract.
Applications that need parallel rendering should give workers independent
renderers or provide their own explicit serialization.

Handler-scoped request, route-param, header, session, and JWT views must never
be retained by a worker. Copy required values before returning from the owner
thread callback.

A stale/cancelled deferred handle fails through the existing generation-checked
CHTTP deferred API; the Web helper does not retry or invent fallback state.

## SSE route

`CHttp::Web` adds a thin SSE layer over CHTTP response streaming rather than
a second streaming runtime.

A `GET /events` route should:

1. keep caller-owned `chttp_web_sse_stream` state alive for the response;
2. provide a synchronous bounded `next` callback;
3. initialize the stream with `chttp_web_sse_stream_init()`;
4. commit it with `chttp_web_sse_response()`;
5. release application stream state from the exactly-once close callback.

`web/tests/test_chttp_web_sse.c` is the executable protocol reference. It
proves canonical event formatting, newline normalization, the same finite event
sequence over HTTP/1.1 and HTTP/2, and exactly-once cleanup after disconnect.

SSE and WebSocket are independent choices. WebSocket remains a CHTTP server
capability and is not reimplemented by `CHttp::Web`.

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
  and domain-specific content inspection are outside CHttp::Web and must run in
  the application's staging/commit pipeline where required.

The security reference application is
`web/examples/chttp_web_security_example.c`.

## Renderer ownership and lifecycle

A renderer is synchronous and non-reentrant.

Safe models include:

- one renderer used only on the CHTTP owner thread;
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
target_link_libraries(app PRIVATE CHttp::Web)
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
base CHttp::Web readiness gate.

## Product boundary

`CHttp::Web` is a native-C **server-driven** application layer. It does not
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
