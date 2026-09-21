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

Configure with examples enabled and build the desired target:

```text
cmake --preset linux-dev-user -DBUILD_EXAMPLES=ON
cmake --build build/linux-gcc-debug --target chttp_web_example
cmake --build build/linux-gcc-debug --target chttp_web_context_example
cmake --build build/linux-gcc-debug --target chttp_web_crud_example
cmake --build build/linux-gcc-debug --target chttp_web_session_example
cmake --build build/linux-gcc-debug --target chttp_web_security_example
cmake --build build/linux-gcc-debug --target chttp_web_deferred_example
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
