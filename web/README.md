# CHttp::Web

`CHttp::Web` is the optional server-driven presentation layer built on top of `CHttp::Server`, typed CMeta models, and Jinja CMeta rendering.

It is intended for native-C admin UIs, dashboards, developer portals, CRUD/internal tools, device consoles, and hybrid HTML/API services. It does not introduce a second HTTP runtime, a hidden event loop, a client-side framework, or a Wt-style server widget toolkit.

## Package boundary

```text
CHttp::Server
     +
Salts::CMeta
     +
Salts::JinjaCMeta / DataBind
     |
     v
CHttp::Web
```

The hard dependency rules are:

- `CHttp::Server` must not depend on Jinja, HTMX, forms, sessions, or UI policy.
- Jinja CMeta must not depend on CHTTP request/session concepts.
- `CHttp::Web` is the optional bridge and owns the presentation-layer integration.

Installed consumers use:

```cmake
find_package(Chttp CONFIG REQUIRED)
target_link_libraries(my_ui PRIVATE CHttp::Web)
```

and include:

```c
#include <chttp_web/web.h>
```

## Reference application patterns

### Minimal SSR

See `web/examples/chttp_web_example.c`.

The application creates one bounded renderer from a fixed template bundle, renders a typed CMeta model, and returns HTML through `chttp_web_render_response()`. Template names are frozen at renderer initialization and HTML autoescape is enabled.

### Request context and HTMX interaction

See `web/examples/chttp_web_context_example.c`.

It demonstrates:

- copied request context for route params, selected headers, and selected session values;
- full-page SSR for ordinary requests;
- fragment rendering for exact `HX-Request: true`;
- `HX-Trigger`, `HX-Retarget`, and redirect helpers;
- bounded response-header failure behavior.

For CRUD-style applications, keep the mutation route ordinary HTTP state mutation and return either a redirect or an HTMX fragment. The renderer remains presentation-only; storage/database ownership stays in the application layer.

### Session, forms, CSRF, and flash

See `web/examples/chttp_web_session_example.c`.

The example covers bounded form parsing, CSRF token creation/validation/rotation, session invalidation, and session-backed flash messages. JWT-only API routes remain outside CSRF scope unless the application explicitly opts in.

### Security policy

See `web/examples/chttp_web_security_example.c`.

It combines the Web security middleware with CHTTP CORS, JWT, rate limiting, and per-response header overrides. Jinja autoescape and HTTP security headers are independent protections.

### Deferred worker rendering

The tested worker pattern is:

```text
owner thread
  -> copy every request/session/header/param value needed by the job
  -> chttp_server_response_defer()
  -> application worker
  -> build typed model
  -> chttp_web_deferred_render_reply()
```

See `web/tests/test_chttp_web_deferred.c` for the executable qualification example.

Important constraints:

- a renderer is synchronous and non-reentrant;
- one renderer may be owned by one worker at a time;
- parallel workers use independent renderers or external serialization;
- handler-borrowed request/session/JWT/param/header pointers must never cross threads;
- stale/cancelled deferred handles fail explicitly and are never retried implicitly.

### Server-Sent Events

`chttp_web_sse_response()` is a thin wrapper over the existing CHTTP response-source engine. HTTP/1.1 chunking and HTTP/2 DATA framing remain owned by CHTTP.

A route follows this shape:

```c
static int next_event(void *user, chttp_web_sse_event *event) {
  *event = (chttp_web_sse_event)CHTTP_WEB_SSE_EVENT_INIT;
  event->has_data = true;
  event->data = (chttp_web_string_view){"ready", 5};
  return SALTS_OK; /* return SALTS_ENOENT for normal EOF */
}

static int events(void *user,
                  const chttp_server_request_view *request,
                  chttp_server_response *response) {
  chttp_web_sse_stream *stream = user;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  (void)request;
  return chttp_web_sse_response(response, stream, &error) == CHTTP_WEB_OK
      ? SALTS_OK
      : error.native_status;
}
```

See `web/tests/test_chttp_web_sse.c` for byte-exact formatting, H1/h2c streaming, disconnect cleanup, and capacity tests.

## Ownership and lifecycle

- renderer init/load is bounded and fail-closed;
- renderer render is synchronous and non-reentrant;
- render output is owned and atomic: partial output is not published;
- request context snapshots are caller-owned and bounded;
- deferred handles are generation checked;
- SSE cleanup follows the existing source lifecycle and runs exactly once;
- server stop/drain semantics remain owned by `CHttp::Server`.

## Qualification status

The production-readiness claim is intentionally gated by issue #28:

- #40 correctness/lifecycle/sanitizers;
- #41 install/export + standalone consumer;
- #42 reproducible benchmarks;
- #43 reference surface and final documentation.

Until those gates are all green, documentation should describe `CHttp::Web` as the optional native-C server-driven presentation layer under qualification, not as fully production-qualified.
