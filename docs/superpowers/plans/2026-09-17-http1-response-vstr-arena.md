# HTTP/1 Response Parser vstr + Arena Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the HTTP/1 client response parser from four independent heap allocations to one bounded parser-owned arena, use `vstr` views for parser-owned textual slices, and preserve the existing public CHTTP ABI and response lifetime semantics.

**Architecture:** Keep `chttp_response_parser` as the single owner. Initialization computes the complete bounded layout from `chttp_limits` and sink mode, performs one zeroed backing allocation, and derives the public header array, header text storage, reason storage, and optional buffered body region from it. llhttp callbacks continue to append fragmented bytes into bounded storage, but completed status/header slices are represented internally as non-owning `vstr` views; existing `const char *` API pointers remain NUL-compatible projections into the same arena.

**Tech Stack:** C11, llhttp, Salts `vstr`, TinyTest, CMake/Ninja, AddressSanitizer, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-17-http1-response-vstr-arena-design.md`

## Global Constraints

- Public `chttp_header`, `chttp_response_view`, request submission APIs, and callback signatures must not change.
- `chttp_limits` remains authoritative for all parser capacities; no parser arena growth or `realloc()` is allowed while parsing.
- Buffered response bodies live in the arena only when neither a callback body sink nor a file sink is active.
- Callback body sink and file sink modes remain streaming and must not reserve `max_response_body_bytes` inside the parser arena.
- Existing HTTP version checks, informational-response limits, upgrade rejection, body/header bounds, failure stages, and llhttp pause behavior remain unchanged.
- HTTP server parsing, H2/HPACK, WebSocket pool allocation, RPC/S3 allocation, TLS/network pooling, Salts allocator APIs, and public `vstr` exposure are out of scope.
- The branch remains `perf/http1-response-vstr-arena`; PR #3 remains draft until focused tests, ASan, full build, full CTest, and exact-head/clean-source verification pass.

---

## File Map

- `http_client/src/chttp_internal.h` — parser ownership fields, transient `vstr` views, and internal sink-aware initializer declaration.
- `http_client/src/chttp_response.c` — arena sizing/layout, parser initialization/destruction, fragmented status/header assembly, message reset, and parser execution semantics.
- `http_client/src/chttp_request.c` — behavior-preserving `chttp_response_view_header()` refactor from handwritten ASCII comparison to `vstr_ieq`.
- `http_client/src/chttp_client.c` — pass the file-sink lifecycle into parser initialization before arena sizing instead of attaching it afterward.
- `http_client/tests/chttp_response_test.c` — direct parser TDD coverage for arena layout/modes, fragmentation, exact bounds, informational reset/reuse, and header lookup.
- `.github/workflows/vstr-hot-paths.yml` — repurpose the existing exact-head verification workflow for the new branch and add response-parser/ASan gates.

---

### Task 1: Establish One Bounded Arena Ownership Root

**Files:**
- Modify: `http_client/src/chttp_internal.h`
- Modify: `http_client/src/chttp_response.c`
- Test: `http_client/tests/chttp_response_test.c`

**Interfaces:**
- Consumes: existing `chttp_limits`, `chttp_body_sink`, and opaque `chttp_file_sink_transfer`.
- Produces:
  ```c
  int chttp_response_parser_init_with_sinks(
      chttp_response_parser *parser,
      chttp_method method,
      const chttp_limits *limits,
      const chttp_body_sink *sink,
      chttp_file_sink_transfer *file_sink_transfer);
  ```
- Existing `chttp_response_parser_init()` and `chttp_response_parser_init_with_sink()` remain available and delegate to the new internal initializer.
- `chttp_response_parser_destroy()` owns and releases exactly `parser->arena`; all region pointers are derived and never individually freed.

- [ ] **Step 1: Write the failing arena-mode tests**

Add `<stdint.h>` to `chttp_response_test.c` and add tests inside the existing parser spec that directly inspect internal parser state:

```c
it("owns one bounded arena in buffered mode") {
  chttp_limits limits = chttp_response_test_limits();
  chttp_response_parser parser;
  const unsigned char *arena_end;

  check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
  check_not_null(parser.arena);
  check_true(parser.arena_capacity > 0u);
  arena_end = parser.arena + parser.arena_capacity;
  check_true((const unsigned char *)parser.headers >= parser.arena);
  check_true((const unsigned char *)parser.header_storage >= parser.arena);
  check_true((const unsigned char *)parser.reason_storage >= parser.arena);
  check_true((const unsigned char *)parser.body_storage >= parser.arena);
  check_true((const unsigned char *)parser.body_storage < arena_end);
  chttp_response_parser_destroy(&parser);
  check_null(parser.arena);
}

it("does not reserve buffered body storage for streaming sinks") {
  chttp_response_sink_probe probe = {0};
  const chttp_body_sink sink = {.write = chttp_response_test_sink, .user = &probe};
  chttp_limits limits = chttp_response_test_limits();
  chttp_response_parser parser;
  chttp_file_sink_transfer *file_sink = (chttp_file_sink_transfer *)(uintptr_t)1u;

  check_equal(chttp_response_parser_init_with_sink(&parser, CHTTP_METHOD_GET, &limits, &sink),
              SALTS_OK);
  check_not_null(parser.arena);
  check_null(parser.body_storage);
  chttp_response_parser_destroy(&parser);

  check_equal(chttp_response_parser_init_with_sinks(
                  &parser, CHTTP_METHOD_GET, &limits, NULL, file_sink),
              SALTS_OK);
  check_not_null(parser.arena);
  check_null(parser.body_storage);
  check_equal(parser.file_sink_transfer, file_sink);
  chttp_response_parser_destroy(&parser);

  check_equal(chttp_response_parser_init_with_sinks(
                  &parser, CHTTP_METHOD_GET, &limits, &sink, file_sink),
              SALTS_EINVAL);
}
```

- [ ] **Step 2: Run the focused test and confirm RED**

Run:

```bash
cmake --preset linux-dev-user -DENABLE_SANITIZER_ADDRESS=OFF -DENABLE_SANITIZER_UNDEFINED=OFF
cmake --build build/linux-gcc-debug --target chttp_response_test -j2
```

Expected: build fails because `chttp_response_parser` has no `arena`/`arena_capacity` fields and `chttp_response_parser_init_with_sinks()` does not exist.

- [ ] **Step 3: Add arena ownership and exact sink-mode initialization**

In `chttp_internal.h`, add the Salts view dependency and ownership fields:

```c
#include <vstr.h>

...

typedef struct chttp_response_parser {
  llhttp_t parser;
  llhttp_settings_t settings;
  chttp_response_view response;
  unsigned char *arena;
  size_t arena_capacity;
  chttp_header *headers;
  char *header_storage;
  char *reason_storage;
  unsigned char *body_storage;
  vstr current_field;
  vstr current_value;
  vstr reason_view;
  ...
} chttp_response_parser;

int chttp_response_parser_init_with_sinks(
    chttp_response_parser *parser,
    chttp_method method,
    const chttp_limits *limits,
    const chttp_body_sink *sink,
    chttp_file_sink_transfer *file_sink_transfer);
```

In `chttp_response.c`, introduce checked sizing helpers:

```c
static bool chttp_response_size_add(size_t left, size_t right, size_t *out) {
  if (out == NULL || left > SIZE_MAX - right) return false;
  *out = left + right;
  return true;
}

static bool chttp_response_size_mul(size_t left, size_t right, size_t *out) {
  if (out == NULL || (left != 0u && right > SIZE_MAX / left)) return false;
  *out = left * right;
  return true;
}
```

Implement `chttp_response_parser_init_with_sinks()` with this exact layout policy:

```c
headers_bytes = max_header_count * sizeof(chttp_header);
header_storage_capacity = max_header_bytes + 2 * max_header_count + 1;
reason_capacity = max_reason_bytes + 1;
body_capacity = sink == NULL && file_sink_transfer == NULL
                    ? max_response_body_bytes
                    : 0u;
total = headers_bytes + header_storage_capacity + reason_capacity + body_capacity;
```

All multiply/add operations must use checked helpers. `malloc` already returns storage suitably aligned for `chttp_header`, and the typed header array is region zero, so no later typed-region padding is required.

Allocate exactly once:

```c
parser->arena = (unsigned char *)calloc(1u, total);
if (parser->arena == NULL) return SALTS_ENOMEM;
parser->arena_capacity = total;

cursor = parser->arena;
parser->headers = (chttp_header *)cursor;
cursor += headers_bytes;
parser->header_storage = (char *)cursor;
cursor += header_storage_capacity;
parser->reason_storage = (char *)cursor;
cursor += reason_capacity;
parser->body_storage = body_capacity != 0u ? cursor : NULL;
```

Reject simultaneous callback and file sinks with `SALTS_EINVAL`. Set `parser->file_sink_transfer = file_sink_transfer` during initialization. Keep the existing public-internal wrappers:

```c
int chttp_response_parser_init(chttp_response_parser *parser, chttp_method method,
                               const chttp_limits *limits) {
  return chttp_response_parser_init_with_sinks(parser, method, limits, NULL, NULL);
}

int chttp_response_parser_init_with_sink(chttp_response_parser *parser, chttp_method method,
                                         const chttp_limits *limits,
                                         const chttp_body_sink *sink) {
  return chttp_response_parser_init_with_sinks(parser, method, limits, sink, NULL);
}
```

Destroy through the single owner only:

```c
void chttp_response_parser_destroy(chttp_response_parser *parser) {
  if (parser == NULL) return;
  free(parser->arena);
  memset(parser, 0, sizeof(*parser));
}
```

- [ ] **Step 4: Run focused response tests and confirm GREEN**

Run:

```bash
cmake --build build/linux-gcc-debug --target chttp_response_test -j2
ctest --test-dir build/linux-gcc-debug -R '^chttp_response_test$' --output-on-failure
```

Expected: `chttp_response_test` passes.

- [ ] **Step 5: Audit the source-level ownership rule**

Run:

```bash
grep -nE '\b(malloc|calloc|realloc|free)\b' http_client/src/chttp_response.c
```

Expected after Task 1: parser initialization has one backing `calloc`, parser destruction has one matching `free`, and no parser-owned region is separately allocated/freed. Any allocation owned by a separate file-sink object is outside this file and outside this ownership root.

- [ ] **Step 6: Commit**

```bash
git add http_client/src/chttp_internal.h http_client/src/chttp_response.c http_client/tests/chttp_response_test.c
git commit -m "perf(http): add bounded response parser arena"
```

---

### Task 2: Represent Fragmented Status and Headers as `vstr` Views

**Files:**
- Modify: `http_client/src/chttp_internal.h`
- Modify: `http_client/src/chttp_response.c`
- Modify: `http_client/src/chttp_request.c`
- Test: `http_client/tests/chttp_response_test.c`

**Interfaces:**
- Consumes: arena-owned header/reason regions from Task 1.
- Produces: `current_field`, `current_value`, and `reason_view` as non-owning `vstr` slices; public response pointers still point into NUL-compatible arena storage.
- Produces behavior-preserving `chttp_response_view_header(const chttp_response_view *, const char *)` implemented with `vstr_ieq`.

- [ ] **Step 1: Strengthen the fragmentation/view tests before changing callback assembly**

In the existing byte-at-a-time response test, add exact view assertions:

```c
check_equal(parser.reason_view.len, (size_t)2u);
check_equal(parser.reason_view.data, "OK", 2u);
check_equal(chttp_response_view_header(&parser.response, "CONTENT-TYPE"), "text/plain");
check_null(chttp_response_view_header(&parser.response, "content"));
check_true(vstr_empty(parser.current_field));
check_true(vstr_empty(parser.current_value));
```

Add an explicit multi-call fragmentation case:

```c
it("keeps length-bearing views correct across split status and header callbacks") {
  static const char part1[] = "HTTP/1.1 200 Cre";
  static const char part2[] = "ated\r\nX-Frag";
  static const char part3[] = "ment: va";
  static const char part4[] = "lue\r\nContent-Length: 0\r\n\r\n";
  chttp_limits limits = chttp_response_test_limits();
  chttp_response_parser parser;

  check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
  check_equal(chttp_response_parser_execute(&parser, part1, sizeof(part1) - 1u), SALTS_OK);
  check_equal(chttp_response_parser_execute(&parser, part2, sizeof(part2) - 1u), SALTS_OK);
  check_equal(chttp_response_parser_execute(&parser, part3, sizeof(part3) - 1u), SALTS_OK);
  check_equal(chttp_response_parser_execute(&parser, part4, sizeof(part4) - 1u), SALTS_OK);
  check_true(parser.complete);
  check_equal(parser.reason_view.len, sizeof("Created") - 1u);
  check_equal(parser.reason_view.data, "Created", sizeof("Created") - 1u);
  check_equal(chttp_response_view_header(&parser.response, "x-fragment"), "value");
  chttp_response_parser_destroy(&parser);
}
```

- [ ] **Step 2: Run the focused test before refactor**

Run:

```bash
cmake --build build/linux-gcc-debug --target chttp_response_test -j2
ctest --test-dir build/linux-gcc-debug -R '^chttp_response_test$' --output-on-failure
```

Expected: new assertions involving `reason_view/current_field/current_value` fail until callback assembly populates/reset views correctly; existing public string assertions remain the compatibility baseline.

- [ ] **Step 3: Create views at completion boundaries without rescanning parser-owned text**

Keep fragmented append behavior and offsets, but change completion callbacks to use known lengths.

At status completion:

```c
parser->reason_storage[parser->reason_size] = '\0';
parser->reason_view = vstr_from_buf(parser->reason_storage, parser->reason_size);
parser->response.reason = parser->reason_view.data;
parser->reason_terminated = true;
```

At header-field completion:

```c
const size_t field_size = parser->header_storage_used - parser->field_offset;
parser->current_field =
    vstr_from_buf(parser->header_storage + parser->field_offset, field_size);
if (!chttp_response_storage_append(parser, "\0", 1u)) ...;
```

At header-value completion, compute the value length before the NUL append, create the view, then publish the public header:

```c
const size_t value_size = parser->header_storage_used - parser->value_offset;
parser->current_value =
    vstr_from_buf(parser->header_storage + parser->value_offset, value_size);
if (!chttp_response_storage_append(parser, "\0", 1u)) ...;

header = &parser->headers[parser->response.header_count++];
header->name = parser->current_field.data;
header->value = parser->current_value.data;
parser->current_field = (vstr){0};
parser->current_value = (vstr){0};
```

In `chttp_response_reset_message()`, clear all transient views:

```c
parser->current_field = (vstr){0};
parser->current_value = (vstr){0};
parser->reason_view = (vstr){0};
```

Do not call `strlen()` on parser-owned status/header slices.

- [ ] **Step 4: Refactor response header lookup to `vstr_ieq`**

In `chttp_request.c`, replace the remaining handwritten ASCII lookup comparator used by `chttp_response_view_header()`:

```c
const char *chttp_response_view_header(const chttp_response_view *response, const char *name) {
  const vstr wanted = name != NULL ? vstr_from_cstr(name) : (vstr){0};
  size_t index;
  if (response == NULL || name == NULL) return NULL;
  for (index = 0u; index < response->header_count; ++index) {
    const chttp_header *header = &response->headers[index];
    if (header->name != NULL && vstr_ieq(vstr_from_cstr(header->name), wanted)) return header->value;
  }
  return NULL;
}
```

Delete `chttp_ascii_lower()` / `chttp_ascii_equal()` from this file if no remaining caller exists after the refactor.

- [ ] **Step 5: Run focused request + response tests**

Run:

```bash
cmake --build build/linux-gcc-debug --target chttp_request_test chttp_response_test -j2
ctest --test-dir build/linux-gcc-debug \
  -R '^(chttp_request_test|chttp_response_test)$' --output-on-failure
```

Expected: both tests pass.

- [ ] **Step 6: Commit**

```bash
git add http_client/src/chttp_internal.h http_client/src/chttp_response.c \
        http_client/src/chttp_request.c http_client/tests/chttp_response_test.c
git commit -m "perf(http): use vstr views in response parsing"
```

---

### Task 3: Lock Exact Bounds and Informational-Response Arena Reuse

**Files:**
- Modify: `http_client/src/chttp_response.c`
- Test: `http_client/tests/chttp_response_test.c`

**Interfaces:**
- Consumes: Task 1 arena and Task 2 view-reset behavior.
- Produces: explicit regression coverage that exact configured limits succeed, one-byte-over inputs fail with the existing error category, and an informational response cannot leak stale pointers/state into the final response.

- [ ] **Step 1: Add exact-bound RED/characterization cases**

Add a compact exact-bound test using isolated parser instances:

```c
it("accepts exact response bounds and rejects one-byte-over inputs") {
  static const char one_header[] = "HTTP/1.1 204 OK\r\nX: y\r\n\r\n";
  static const char body[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  chttp_limits limits = chttp_response_test_limits();
  chttp_response_parser parser;

  limits.max_header_count = 1u;
  limits.max_header_bytes = 6u; /* 1 field + 1 value + ': ' + CRLF */
  check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
  check_equal(chttp_response_parser_execute(&parser, one_header, sizeof(one_header) - 1u), SALTS_OK);
  check_equal(parser.response.header_count, (size_t)1u);
  chttp_response_parser_destroy(&parser);

  limits.max_header_bytes = 5u;
  check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
  check_equal(chttp_response_parser_execute(&parser, one_header, sizeof(one_header) - 1u),
              SALTS_EMSGSIZE);
  chttp_response_parser_destroy(&parser);

  limits = chttp_response_test_limits();
  limits.max_response_body_bytes = 5u;
  check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
  check_equal(chttp_response_parser_execute(&parser, body, sizeof(body) - 1u), SALTS_OK);
  check_equal(parser.response.body, "hello", 5u);
  chttp_response_parser_destroy(&parser);

  limits.max_response_body_bytes = 4u;
  check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
  check_equal(chttp_response_parser_execute(&parser, body, sizeof(body) - 1u), SALTS_EMSGSIZE);
  chttp_response_parser_destroy(&parser);
}
```

Add a reason-phrase boundary check:

```c
limits = chttp_response_test_limits();
limits.max_start_line_bytes = 17u; /* max reason = 2 with existing 15-byte overhead */
check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
check_equal(chttp_response_parser_execute(
                &parser, "HTTP/1.1 204 OK\r\n\r\n", sizeof("HTTP/1.1 204 OK\r\n\r\n") - 1u),
            SALTS_OK);
chttp_response_parser_destroy(&parser);

limits.max_start_line_bytes = 16u;
check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
check_equal(chttp_response_parser_execute(
                &parser, "HTTP/1.1 204 OK\r\n\r\n", sizeof("HTTP/1.1 204 OK\r\n\r\n") - 1u),
            SALTS_EMSGSIZE);
chttp_response_parser_destroy(&parser);
```

- [ ] **Step 2: Add informational-response stale-state regression**

```c
it("reuses the arena without leaking informational response state") {
  static const char input[] = "HTTP/1.1 100 Continue\r\n"
                              "X-Old: stale\r\n\r\n"
                              "HTTP/1.1 200 OK\r\n"
                              "X-New: final\r\n"
                              "Content-Length: 0\r\n\r\n";
  chttp_limits limits = chttp_response_test_limits();
  chttp_response_parser parser;

  check_equal(chttp_response_parser_init(&parser, CHTTP_METHOD_GET, &limits), SALTS_OK);
  check_equal(chttp_response_parser_execute(&parser, input, sizeof(input) - 1u), SALTS_OK);
  check_true(parser.complete);
  check_equal(parser.informational_responses, (size_t)1u);
  check_null(chttp_response_view_header(&parser.response, "x-old"));
  check_equal(chttp_response_view_header(&parser.response, "x-new"), "final");
  check_equal(parser.response.reason, "OK");
  check_equal(parser.reason_view.len, (size_t)2u);
  chttp_response_parser_destroy(&parser);
}
```

- [ ] **Step 3: Run tests and identify any arena-accounting/reset regression**

Run:

```bash
cmake --build build/linux-gcc-debug --target chttp_response_test -j2
ctest --test-dir build/linux-gcc-debug -R '^chttp_response_test$' --output-on-failure
```

Expected: any mismatch must fail in the specific configured-bound or stale-state assertion; do not loosen limits to make the test pass.

- [ ] **Step 4: Apply only the minimal accounting/reset fix needed**

If a new test fails, fix `chttp_response_reset_message()`, `chttp_response_storage_append()`, `chttp_response_wire_add()`, or status/body bound handling while preserving these invariants:

```c
/* NUL bytes consume storage only. */
header_wire_bytes += protocol_bytes_only;
header_storage_used += copied_text_bytes + compatibility_nuls;

/* Message reset reuses capacity, never reallocates. */
header_storage_used = 0u;
header_wire_bytes = 0u;
response.header_count = 0u;
response.body_size = 0u;
current_field = (vstr){0};
current_value = (vstr){0};
reason_view = (vstr){0};
```

Do not change public error categories.

- [ ] **Step 5: Re-run focused tests and commit**

```bash
cmake --build build/linux-gcc-debug --target chttp_response_test -j2
ctest --test-dir build/linux-gcc-debug -R '^chttp_response_test$' --output-on-failure
git add http_client/src/chttp_response.c http_client/tests/chttp_response_test.c
git commit -m "test(http): lock response arena boundaries and reuse"
```

---

### Task 4: Pass File-Sink Mode Before Arena Sizing

**Files:**
- Modify: `http_client/src/chttp_client.c`
- Test: `http_client/tests/chttp_response_test.c`
- Verify: `http_client/tests/chttp_file_sink_test.c`
- Verify: `http_client/tests/chttp_requests_test.c`

**Interfaces:**
- Consumes: `chttp_response_parser_init_with_sinks()` from Task 1.
- Produces: client-side file sink initialization in which `file_sink_transfer` is known before parser arena allocation; removes the old post-init assignment.

- [ ] **Step 1: Keep the Task 1 file-sink sentinel test as the direct RED/GREEN contract**

Confirm this assertion remains in `chttp_response_test.c`:

```c
check_equal(chttp_response_parser_init_with_sinks(
                &parser, CHTTP_METHOD_GET, &limits, NULL, file_sink),
            SALTS_OK);
check_null(parser.body_storage);
check_equal(parser.file_sink_transfer, file_sink);
```

This test is the allocation-mode contract. It must pass before wiring the real client caller.

- [ ] **Step 2: Change `chttp_slot_prepare_streaming()` to use the sink-aware initializer**

Replace:

```c
status = chttp_response_parser_init_with_sink(&slot->response_parser, options->method,
                                              &slot->client->limits, options->body_sink);
if (status != SALTS_OK) return status;
slot->response_parser.file_sink_transfer = file_sink_transfer;
```

with:

```c
status = chttp_response_parser_init_with_sinks(
    &slot->response_parser, options->method, &slot->client->limits,
    options->body_sink, file_sink_transfer);
if (status != SALTS_OK) return status;
```

No later assignment to `response_parser.file_sink_transfer` remains.

- [ ] **Step 3: Build the affected client graph**

Run:

```bash
cmake --build build/linux-gcc-debug --target \
  chttp_response_test chttp_file_sink_test chttp_requests_test -j2
```

Expected: build succeeds with the new internal signature and no stale caller.

- [ ] **Step 4: Run the affected tests**

Run:

```bash
ctest --test-dir build/linux-gcc-debug \
  -R '^(chttp_response_test|chttp_file_sink_test|chttp_requests_test)$' \
  --output-on-failure
```

Expected: all three tests pass; file-sink lifecycle semantics remain unchanged.

- [ ] **Step 5: Commit**

```bash
git add http_client/src/chttp_client.c http_client/tests/chttp_response_test.c
git commit -m "perf(http): size response arena for file sink mode"
```

---

### Task 5: Repurpose Exact-Head CI for the Response Arena Gate

**Files:**
- Modify: `.github/workflows/vstr-hot-paths.yml`

**Interfaces:**
- Consumes: merged Salts pin `c4197712261a563ed7e238152b34cb50a2ef98a9`, SaltsUtils pin `c6662ccd382a4311ff648ff80570d8d8f98028a8`, and vcpkg pin `b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01` already verified by #2.
- Produces: a push-triggered exact-head verification workflow for `perf/http1-response-vstr-arena` that runs focused parser tests, a focused ASan gate, full build, full CTest, and clean-source verification.

- [ ] **Step 1: Retarget the existing verification workflow to the new branch**

Change the workflow name and push branch:

```yaml
name: HTTP response arena verification

on:
  push:
    branches:
      - perf/http1-response-vstr-arena
  workflow_dispatch:
```

Keep exact SHA dependency checkout/cache semantics from #2 unchanged.

- [ ] **Step 2: Make response/file-sink/request tests the focused normal gate**

Change focused build targets to:

```yaml
- name: Build focused targets
  run: |
    set -euxo pipefail
    cmake --build build/linux-gcc-debug --target \
      chttp_response_test chttp_file_sink_test chttp_requests_test -j2
```

Change focused CTest regex to:

```bash
ctest --test-dir build/linux-gcc-debug \
  -R '^(chttp_response_test|chttp_file_sink_test|chttp_requests_test)$' \
  --output-on-failure
```

- [ ] **Step 3: Add a focused ASan response-parser gate after the normal full CTest**

Reconfigure the same developer build with ASan enabled and rebuild the response target:

```yaml
- name: Verify response parser lifecycle with AddressSanitizer
  shell: bash
  env:
    PROJECT_ROOT: ${{ runner.temp }}/salts-project
    VCPKG_ROOT: ${{ github.workspace }}/vcpkg
    SALTS_UTILS_ROOT: /opt/salts-utils/debug
    LD_LIBRARY_PATH: /opt/salts-utils/debug/lib:/opt/salts-utils/debug/bin
    ASAN_OPTIONS: detect_leaks=1:halt_on_error=1
  run: |
    set -euxo pipefail
    cmake --preset linux-dev-user \
      -DENABLE_SANITIZER_ADDRESS=ON \
      -DENABLE_SANITIZER_UNDEFINED=OFF
    cmake --build build/linux-gcc-debug --target chttp_response_test -j2
    ctest --test-dir build/linux-gcc-debug \
      -R '^chttp_response_test$' --output-on-failure
```

Do not enable sanitizers for the separately built Salts/SaltsUtils packages in this PR; the gate is for CHTTP parser lifecycle ownership.

- [ ] **Step 4: Preserve full normal build/full CTest before ASan reconfiguration**

The workflow order must remain:

```text
exact checkout/dependencies
-> normal configure
-> focused normal tests
-> full normal build
-> full normal CTest
-> focused ASan response parser
-> exact-head/clean-source verification
```

The exact-head/clean-source step remains `if: always()`.

- [ ] **Step 5: Commit the CI gate**

```bash
git add .github/workflows/vstr-hot-paths.yml
git commit -m "ci: verify H1 response arena migration"
```

Pushing this commit to `perf/http1-response-vstr-arena` must trigger the new exact-head workflow.

---

### Task 6: Final Audit and PR Readiness

**Files:**
- Verify: `http_client/src/chttp_internal.h`
- Verify: `http_client/src/chttp_response.c`
- Verify: `http_client/src/chttp_request.c`
- Verify: `http_client/src/chttp_client.c`
- Verify: `http_client/tests/chttp_response_test.c`
- Verify: `.github/workflows/vstr-hot-paths.yml`
- Update metadata only: PR #3 body after evidence exists

**Interfaces:**
- Consumes: Tasks 1-5 exact branch state.
- Produces: evidence sufficient to move PR #3 from draft to ready-for-review; does not merge the PR.

- [ ] **Step 1: Run the complete local focused suite**

```bash
cmake --preset linux-dev-user -DENABLE_SANITIZER_ADDRESS=OFF -DENABLE_SANITIZER_UNDEFINED=OFF
cmake --build build/linux-gcc-debug --target \
  chttp_request_test chttp_response_test chttp_file_sink_test chttp_requests_test -j2
ctest --test-dir build/linux-gcc-debug \
  -R '^(chttp_request_test|chttp_response_test|chttp_file_sink_test|chttp_requests_test)$' \
  --output-on-failure
```

Expected: all focused tests pass.

- [ ] **Step 2: Run full normal build and full CTest**

```bash
cmake --build --preset linux-dev-user -j2
ctest --test-dir build/linux-gcc-debug --no-tests=error --output-on-failure
```

Expected: full build succeeds and CTest reports zero failed tests.

- [ ] **Step 3: Run the focused local ASan lifecycle gate**

```bash
cmake --preset linux-dev-user -DENABLE_SANITIZER_ADDRESS=ON -DENABLE_SANITIZER_UNDEFINED=OFF
cmake --build build/linux-gcc-debug --target chttp_response_test -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  ctest --test-dir build/linux-gcc-debug -R '^chttp_response_test$' --output-on-failure
```

Expected: `chttp_response_test` passes with no ASan/leak report.

- [ ] **Step 4: Audit allocation and scope invariants**

Run:

```bash
grep -nE '\b(malloc|calloc|realloc|free)\b' http_client/src/chttp_response.c
git diff master...HEAD -- http_server http_common/http/chttp_h2_* \
  http_client/src/chttp_h2_session.c http_client/src/chttp_websocket_pool.c
```

Expected:

- response parser source has one parser backing allocation and one ownership-root free, with no parser `realloc()`;
- scoped server/H2/WebSocket production paths have no diff.

- [ ] **Step 5: Verify the GitHub exact-head workflow**

After pushing the final implementation commit, record the exact PR head SHA and inspect `HTTP response arena verification` for that same SHA. Required successful steps:

```text
Checkout exact Chttp head
Checkout exact public dependencies
Run focused tests
Build full Chttp graph
Run full Chttp CTest graph
Verify response parser lifecycle with AddressSanitizer
Verify exact head and clean tracked source
```

Do not use an earlier-head run as evidence.

- [ ] **Step 6: Update PR #3 evidence and mark ready only after every gate is green**

Update the PR body with:

```text
- exact implementation head SHA
- focused response/request/file-sink test result
- full build + full CTest result
- focused ASan result
- source allocation audit result
- exact-head/clean-source workflow result
```

Then mark PR #3 ready for review. Do not merge it without a separate explicit instruction.
