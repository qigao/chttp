# CHttp::Web — Native Server-Driven UI Layer

Date: 2026-09-20  
Status: Design proposal  
Repository: `qigao/chttp`

## 1. Goal

Add an optional, product-grade web application layer on top of the existing CHTTP server and Jinja CMeta without turning either component into a monolithic framework.

The resulting stack is:

```text
Application
    |
    v
CHttp::Web
    |------------------------------.
    v                              |
CHttp::Server                      |
routes / middleware / sessions     |
JWT / CORS / rate limit / files    |
deferred replies / H2 / WebSocket  |
    |                              |
    '----------> CMeta <-----------'
                  |
                  v
             Jinja CMeta
          SSR / fragments
                  |
                  v
                HTMX
                  |
               Browser
```

The design should make CHTTP + Jinja CMeta a credible native-C server-driven application stack for admin UIs, dashboards, developer portals, CRUD tools, device management, internal tools, and hybrid HTML/API services.

## 2. Architectural boundary

### 2.1 Dependency direction

```text
CHttp::Web
   +--> CHttp::Server
   +--> Salts::JinjaCMeta
   +--> CMeta

CHttp::Server
   X--> Jinja
   X--> HTMX
   X--> UI concerns

Salts::JinjaCMeta
   X--> CHTTP
   X--> request/response/session concerns
```

This boundary is mandatory.

The HTTP server remains a transport/application infrastructure library. Jinja CMeta remains a rendering engine. `CHttp::Web` is the optional bridge.

### 2.2 Reuse existing CHTTP capabilities

`CHttp::Web` must compose existing CHTTP features instead of reimplementing them:

- routing and path parameters;
- global and per-route middleware;
- admission hooks;
- bounded request/response bodies;
- sessions;
- JWT bearer admission;
- CORS;
- rate limiting;
- deferred cross-thread responses;
- response streaming;
- static/file serving with ETag/range handling;
- HTTP/1.1 and HTTP/2;
- WebSocket routes;
- server statistics.

## 3. Non-goals

The first product version does not add:

- a virtual DOM;
- a server-side widget tree;
- a browser runtime comparable to React/Vue;
- client-side state synchronization;
- a new router or middleware engine;
- a new session store;
- an ORM;
- a JavaScript build pipeline;
- server-side Try-it proxying;
- template-specific logic inside `CHttp::Server`.

## 4. Product model

`CHttp::Web` should be a small optional package, installable independently of the OpenAPI module.

Suggested package target:

```text
CHttp::Web
```

Suggested source layout:

```text
web/
  include/chttp_web/web.h
  src/chttp_web_renderer.c
  src/chttp_web_context.c
  src/chttp_web_htmx.c
  src/chttp_web_forms.c
  src/chttp_web_csrf.c
  src/chttp_web_flash.c
  src/chttp_web_security.c
  src/chttp_web_deferred.c
  src/chttp_web_sse.c
  tests/
  examples/
```

Exact filenames may change during implementation; dependency ownership must not.

## 5. Core rendering API

The integration API should remain smaller than the underlying Jinja CMeta API.

Conceptually:

```c
typedef struct chttp_web_renderer {
  void *impl;
} chttp_web_renderer;

typedef struct chttp_web_render_options {
  size_t size;
  const char *template_name;
  unsigned int status_code;
  const char *content_type;
  const cmeta_data_desc *model_desc;
  const void *model;
} chttp_web_render_options;

int chttp_web_renderer_init(...);
int chttp_web_renderer_destroy(...);

int chttp_web_render(
    chttp_web_renderer *renderer,
    const chttp_server_request_view *request,
    chttp_server_response *response,
    const chttp_web_render_options *options);
```

The public API must not expose Jinja implementation internals unless required for an explicit advanced-extension path.

### Required semantics

- templates are compiled/loaded through a bounded loader;
- rendering is deterministic for the same template + model;
- output size is bounded;
- failed rendering never commits a partial HTTP reply;
- autoescape policy follows Jinja CMeta template semantics;
- response content type defaults to UTF-8 HTML;
- renderer lifecycle and threading ownership are explicit;
- request pointers are never retained beyond handler scope.

## 6. Request context

A standard web request context should expose common HTTP information to templates without forcing every application model to duplicate it.

Conceptual template surface:

```jinja
{{ request.path }}
{{ request.method }}
{{ request.param("id") }}
{{ request.header("X-Request-ID") }}
{{ session.get("theme") }}
{{ flash.messages }}
```

Implementation should prefer typed CMeta-backed structures and bounded views.

The first version should not expose arbitrary mutable server objects to templates.

## 7. Full-page and fragment rendering

The web layer must treat full pages and HTML fragments as first-class outputs.

```text
GET /users
    -> users/index.html

GET /users/42
    -> users/show.html

GET /users/42/card
    -> users/_card.html
```

A helper should detect HTMX requests from standard headers, but HTMX remains optional. Ordinary HTTP clients must still receive correct HTML.

Suggested helpers:

```c
bool chttp_web_request_is_htmx(...);
int chttp_web_hx_redirect(...);
int chttp_web_hx_trigger(...);
int chttp_web_hx_retarget(...);
```

These are response-header conveniences, not a second protocol stack.

## 8. Forms and typed binding

A useful server-driven web stack requires form handling.

The first version should support:

- `application/x-www-form-urlencoded`;
- bounded key/value parsing;
- repeated keys;
- explicit UTF-8/text treatment;
- typed binding to CMeta/DataBind-compatible structures;
- validation errors returned as a typed result rather than hidden global state.

Multipart upload support can be a later issue because CHTTP already has streaming body sinks and file primitives.

## 9. CSRF

Cookie/session-authenticated mutation routes need built-in CSRF support.

The web layer should provide middleware/helper behavior built on existing CHTTP sessions:

- generate a cryptographically random token;
- retain only bounded state in the server session;
- expose token to templates;
- validate mutation requests;
- support standard form field and optional HTMX header transport;
- constant-time comparison;
- invalidate/rotate on session invalidation as appropriate.

JWT-only APIs are outside the CSRF requirement.

## 10. Flash messages

Flash messages are a natural thin feature over existing CHTTP sessions.

Required behavior:

- bounded message count/bytes;
- one-request consumption semantics;
- level/category plus message;
- template exposure through standard context;
- no separate persistence subsystem.

## 11. Security middleware

Provide opt-in web security middleware using the existing middleware chain.

Initial policy should cover helpers for:

- Content-Security-Policy;
- X-Content-Type-Options;
- Referrer-Policy;
- frame restrictions;
- HSTS for TLS deployments;
- cache policy for authenticated pages.

The helper should not silently invent permissive CSP directives. Application customization remains explicit.

Jinja autoescape is a separate rendering guarantee and must not be described as a replacement for HTTP security headers.

## 12. Deferred rendering / worker model

Current CHTTP handlers execute serially on the owner thread and must not block.

`CHttp::Web` therefore needs a defined async/deferred pattern:

```text
owner thread
  -> authenticate / validate / copy needed request state
  -> chttp_server_response_defer()
  -> worker
       -> DB/business work
       -> build typed model
       -> render Jinja
       -> chttp_server_deferred_reply()
```

The first implementation does not need to own a thread pool. It should provide safe render-to-buffer/deferred-reply primitives and document application ownership of worker execution.

Cross-thread rendering is allowed only under a renderer ownership model proven safe by Jinja CMeta. If a renderer is single-owner, use one renderer per worker or another explicitly validated scheme.

## 13. SSE

CHTTP already has response streaming primitives. `CHttp::Web` should add a thin SSE helper rather than a new streaming subsystem.

Expected capabilities:

- correct `text/event-stream` headers;
- event/data/id/retry formatting;
- newline normalization;
- bounded event generation;
- documented disconnect/cancellation lifecycle.

This issue is independent of WebSocket support, which already belongs to CHTTP.

## 14. OpenAPI UI as qualification application

The existing OpenAPI UI should be migrated to `CHttp::Web` after the core rendering/HTMX surface stabilizes.

It becomes the first production qualification workload, proving:

- full-page SSR;
- fragment SSR;
- typed CMeta models;
- Jinja inheritance/includes/macros;
- hostile string escaping;
- browser-only Try-it;
- repeated request isolation;
- bounded rendering;
- HTMX integration;
- lifecycle and restart;
- real CHTTP routing.

The migration must reduce custom OpenAPI-specific renderer glue rather than wrap the same glue in a new name.

## 15. Product-readiness gates

`CHttp::Web` is not product-ready until all of these are demonstrated.

### Correctness
- same model + template => deterministic output;
- request A never leaks state into request B;
- template errors do not commit partial responses;
- full page and fragment rendering behave identically with respect to escaping/context.

### Security
- Jinja autoescape regression suite remains green;
- CSRF protection for session-authenticated mutations;
- security-header middleware tests;
- hostile template/model strings;
- request-controlled template paths rejected by default.

### Lifecycle
- compile/load once, render repeatedly, destroy cleanly;
- server stop/restart;
- no request-context retention;
- ASan/UBSan clean.

### Performance
Measure, do not invent thresholds initially:

- renderer startup;
- full-page render;
- fragment render;
- 1/8/32 independent render contexts;
- allocation count/bytes per render where practical;
- OpenAPI small/large document workloads.

### Packaging
- optional `CHttp::Web` target;
- no Jinja dependency from `CHttp::Server`;
- install/export package test;
- standalone consumer example.

## 16. Delivery sequence

Recommended dependency order:

```text
A. package boundary + renderer
        |
        +--> B. request context + HTMX
        |
        +--> C. forms + typed binding
                 |
                 +--> D. CSRF + flash/session UX
        |
        +--> E. security middleware
        |
        +--> F. deferred rendering contract
        |
        +--> G. SSE helper

A+B+E
   |
   +--> H. migrate OpenAPI UI

A..H
   |
   +--> I. production-readiness / benchmarks / docs / package gate
```

## 17. Definition of success

After the final gate, the project should be able to make this accurate claim:

> CHttp::Web is an optional native-C server-driven web application layer combining CHTTP's routing, middleware, sessions, security, asynchronous response and protocol capabilities with typed CMeta models and Jinja CMeta server-side rendering.

It should **not** claim to be a client-side UI framework or Wt-style server widget toolkit.
