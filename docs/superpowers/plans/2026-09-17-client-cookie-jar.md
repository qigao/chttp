# CHTTP Client Cookie Jar Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded, automatic RFC cookie jar to `CHttp::Client` so HTTP/1.1 and HTTP/2 ingest every valid `Set-Cookie` field and replay matching cookies automatically, while preserving current public struct ABI and existing explicit-header behavior.

**Architecture:** Cookie protocol logic lives in a focused internal `chttp_cookie_jar` module shared by H1 and H2. Standalone `chttp_async_client` owns its jar; blocking `chttp_client` owns one jar outside its recoverable internal async transport so timeout recovery cannot erase cookies. Request admission derives one cookie context, optionally appends one generated `Cookie` field through a temporary effective-options wrapper, and stores the context in the request slot for response-side `Set-Cookie` ingestion before user completion.

**Tech Stack:** C11, CMake, TinyTest, llhttp, Salts clocks (`salts_monotonic_ms`, `salts_realtime_ms`), `Salts::DateTimeParser`, vcpkg `libpsl` 0.21.5 through CMake `FindPkgConfig`, CNet, existing H1 serializer and H2 HPACK stack.

**Spec:** `docs/superpowers/specs/2026-09-17-client-cookie-jar-design.md`

## Global Constraints

- Base implementation branch starts from `master` commit `d2643228b458ee6438dc856261f7434143c8eccf`; rebase only if master moves before implementation begins, then rerun every RED/GREEN gate.
- Do not change the layout of `chttp_client_config`, `chttp_options`, or `chttp_request_options`.
- Automatic cookie handling is enabled by default; explicit caller `Cookie` is a complete one-request override.
- H1 and H2 must share one semantic jar implementation and one owner-thread mutation order.
- Default capability is at least 3000 cookies total and 50 per cookie-domain bucket.
- Reject name+value over 4096 octets; Domain/Path attributes over 1024 octets are not accepted as scope attributes.
- Cookie replacement identity is `(name, domain, host_only, path)`.
- `Max-Age` uses a monotonic deadline; `Expires` uses a realtime/UTC deadline; `Max-Age` wins when both are valid.
- Parse and retain SameSite metadata but do not invent browser top-level-site/navigation state.
- Reject unsafe `Secure`, `__Secure-`, `__Host-`, public-suffix, and insecure-overwrite cases per the committed design.
- Do not comma-combine `Set-Cookie`; ingest every repeated field independently.
- Never truncate a generated `Cookie` header; fail request admission with `SALTS_EMSGSIZE` before sending bytes.
- Cookie rejection/eviction must not convert a valid HTTP response into an HTTP failure.
- Add `libpsl` privately to `CHttp::Client`; do not expose it through `ChttpConfig.cmake` unless static/export evidence proves that necessary.
- Do not add CMake install-verification production code.
- Follow repository fail-fast, bounded-memory, C11, TinyTest, formatting, and exact-head verification rules from `AGENTS.md`.

---

## File Structure

### New files

- `http_client/src/chttp_cookie_jar.h` — internal cookie types, limits, clock seam, request context, prepare/ingest/store/query interfaces.
- `http_client/src/chttp_cookie_jar.c` — parsing, canonicalization, PSL checks, expiry, replacement, eviction, selection, ordering, serialization.
- `http_client/tests/chttp_cookie_jar_test.c` — deterministic unit tests against the internal jar with injected monotonic/realtime clocks.
- `http_client/tests/chttp_cookie_header_cpp_test.cpp` — public header/API compile test plus legacy public-struct ABI guard.
- `.github/workflows/client-cookie-jar.yml` — exact-head Linux/macOS/Windows cookie verification; no install-verify production code.

### Existing files to modify

- `vcpkg.json` — add `libpsl`.
- `http_client/CMakeLists.txt` — compile `chttp_cookie_jar.c`; resolve/link `libpsl` privately with `PkgConfig::LIBPSL`; keep `Salts::DateTimeParser` private.
- `http_client/include/http_client/http.h` — add only the new versioned cookie options type and management functions; do not extend existing structs.
- `http_client/src/chttp_client_internal.h` — internal init path for a borrowed cookie jar used by blocking client recovery.
- `http_client/src/chttp_client.c` — jar ownership for standalone async clients, request prepare/commit, per-slot cookie context, H1 response ingestion, H2 completion ingestion, management API implementation.
- `http_client/src/chttp_requests.c` — blocking-client jar ownership and recovery-preserving internal async reinitialization; blocking management API wrappers.
- `http_client/tests/CMakeLists.txt` — register cookie unit/header tests.
- `http_client/tests/chttp_requests_test.c` — H1 blocking cookie round-trip and explicit override tests.
- `http_client/tests/chttp_h2_client_test.c` — H2 repeated Set-Cookie, automatic replay, concurrent-stream ordering tests.
- `README.md` or `http_client/README.md` — document default automatic jar, explicit override, configuration, non-browser SameSite behavior.

### Existing code intentionally left structurally unchanged

- `http_client/src/chttp_request.c` — continue serializing the `headers` array it receives; cookie integration passes temporary effective options rather than adding cookie policy here.
- `http_client/src/chttp_response.c` — already stores every H1 response field as a separate `chttp_header`; only change it if a test proves a repeated-header defect.
- `http_client/src/chttp_h2_session.c` — already stores repeated regular H2 response fields separately; cookie policy stays outside HPACK/protocol parsing.

---

### Task 1: Establish the internal cookie module and deterministic clock seam

**Files:**
- Create: `http_client/src/chttp_cookie_jar.h`
- Create: `http_client/src/chttp_cookie_jar.c`
- Create: `http_client/tests/chttp_cookie_jar_test.c`
- Modify: `http_client/tests/CMakeLists.txt`
- Modify: `http_client/CMakeLists.txt`

**Interfaces:**
- Produces:

```c
typedef uint64_t (*chttp_cookie_now_ms_fn)(void *user);

typedef struct chttp_cookie_clock {
  chttp_cookie_now_ms_fn monotonic_ms;
  chttp_cookie_now_ms_fn realtime_ms;
  void *user;
} chttp_cookie_clock;

typedef struct chttp_cookie_jar_options_internal {
  int enabled;
  size_t cookie_capacity;
  size_t cookies_per_domain;
  size_t max_cookie_header_bytes;
} chttp_cookie_jar_options_internal;

typedef struct chttp_cookie_jar chttp_cookie_jar;

int chttp_cookie_jar_create(chttp_cookie_jar **out_jar,
                            const chttp_cookie_jar_options_internal *options,
                            const chttp_cookie_clock *clock);
void chttp_cookie_jar_destroy(chttp_cookie_jar *jar);
void chttp_cookie_jar_clear(chttp_cookie_jar *jar);
size_t chttp_cookie_jar_count(const chttp_cookie_jar *jar);
```

- Default production clock calls `salts_monotonic_ms()` and `salts_realtime_ms()`.
- Defaults are named constants in `chttp_cookie_jar.c`: total `3000`, per-domain `50`, name+value `4096`, Domain/Path attribute `1024`.

- [ ] **Step 1: Register a RED unit-test target**

Add to `http_client/tests/CMakeLists.txt`:

```cmake
add_executable(chttp_cookie_jar_test chttp_cookie_jar_test.c)
target_link_libraries(chttp_cookie_jar_test PRIVATE CHttp::Client Salts::TinyTest)
target_include_directories(chttp_cookie_jar_test PRIVATE ${PROJECT_SOURCE_DIR}/http_client/src)
add_test(NAME chttp_cookie_jar_test COMMAND chttp_cookie_jar_test)
set_target_properties(chttp_cookie_jar_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
```

- [ ] **Step 2: Write the first failing lifecycle/clock tests**

Start `chttp_cookie_jar_test.c` with a deterministic clock:

```c
typedef struct test_clock {
  uint64_t monotonic_ms;
  uint64_t realtime_ms;
} test_clock;

static uint64_t test_monotonic(void *user) { return ((test_clock *)user)->monotonic_ms; }
static uint64_t test_realtime(void *user) { return ((test_clock *)user)->realtime_ms; }

suite("chttp cookie jar lifecycle") {
  it("creates an enabled empty bounded jar") {
    test_clock now = {1000u, 1700000000000ull};
    chttp_cookie_jar *jar = NULL;
    const chttp_cookie_clock clock = {test_monotonic, test_realtime, &now};
    const chttp_cookie_jar_options_internal options = {1, 8u, 4u, 512u};
    assert_int_equal(SALTS_OK, chttp_cookie_jar_create(&jar, &options, &clock));
    assert_size_equal(0u, chttp_cookie_jar_count(jar));
    chttp_cookie_jar_destroy(jar);
  }
}
```

- [ ] **Step 3: Run RED**

Run from an existing configured development build tree:

```bash
cmake --build build --target chttp_cookie_jar_test
```

Expected: compile/link failure because `chttp_cookie_jar.h`/symbols do not exist yet.

- [ ] **Step 4: Implement only lifecycle, options normalization, record metadata allocation, and clock defaults**

Do not implement Set-Cookie parsing yet. `create` validates nonzero effective capacities, saturating allocation-size multiplication, and injected clock function pointers; record metadata storage is bounded by `cookie_capacity`.

- [ ] **Step 5: Run GREEN**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

Expected: one cookie-jar test target passes.

- [ ] **Step 6: Commit**

```bash
git add http_client/src/chttp_cookie_jar.[ch] http_client/tests/chttp_cookie_jar_test.c http_client/tests/CMakeLists.txt http_client/CMakeLists.txt
git commit -m "feat(client): add bounded cookie jar core"
```

---

### Task 2: Implement cookie origin canonicalization, Domain/Path rules, and Public Suffix protection

**Files:**
- Modify: `vcpkg.json`
- Modify: `http_client/CMakeLists.txt`
- Modify: `http_client/src/chttp_cookie_jar.h`
- Modify: `http_client/src/chttp_cookie_jar.c`
- Modify: `http_client/tests/chttp_cookie_jar_test.c`

**Interfaces:**
- Consumes jar from Task 1.
- Produces:

```c
typedef struct chttp_cookie_request_context {
  char *host;
  char *path;
  int secure;
} chttp_cookie_request_context;

int chttp_cookie_context_create(const chttp_request_options *options,
                                chttp_cookie_request_context *out_context);
void chttp_cookie_context_destroy(chttp_cookie_request_context *context);
```

- `authority` parsing removes an optional port and IPv6 brackets without treating the port as cookie scope.
- TLS context is derived from `connection_uri` beginning with `tls://`.
- Request path strips query and computes `/` for `OPTIONS *` only where needed for cookie default-path semantics; generic `*` does not match path-scoped cookies.

- [ ] **Step 1: Write failing canonicalization/domain/path tests**

Cover at minimum:

```c
it("canonicalizes host and strips port");
it("canonicalizes bracketed ipv6 without widening domain scope");
it("computes RFC default path from request target");
it("domain matches label boundaries only");
it("path match rejects plain prefix false positives");
it("rejects public suffix domain and accepts registrable domain");
```

Use concrete cases: `WWW.Example.COM:443 -> www.example.com`, `/a/b?x=1 -> /a`, `badexample.com` must not domain-match `example.com`, `/foo` must not path-match `/foobar`, and `Domain=com` must be rejected while `Domain=example.com` may be accepted from `www.example.com`.

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

Expected: new tests fail because context/domain/path/PSL behavior is absent.

- [ ] **Step 3: Add the pinned private `libpsl` dependency**

Update `vcpkg.json` by adding `"libpsl"` to `dependencies`.

In `http_client/CMakeLists.txt` use the port's pkg-config metadata:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPSL REQUIRED IMPORTED_TARGET libpsl)
...
target_link_libraries(chttp_client PRIVATE PkgConfig::LIBPSL ...)
```

Do not add libpsl to `ChttpConfig.cmake.in`.

- [ ] **Step 4: Implement canonical context and PSL checks**

Use `psl_builtin()` and `psl_is_public_suffix2(psl_builtin(), domain, PSL_TYPE_ANY)` for Domain acceptance. Keep IP-literal Domain widening disabled even if PSL does not classify the literal.

- [ ] **Step 5: Run GREEN**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add vcpkg.json http_client/CMakeLists.txt http_client/src/chttp_cookie_jar.[ch] http_client/tests/chttp_cookie_jar_test.c
git commit -m "feat(client): add cookie scope and public suffix rules"
```

---

### Task 3: Implement Set-Cookie parsing, storage identity, expiry, prefixes, and secure integrity

**Files:**
- Modify: `http_client/src/chttp_cookie_jar.h`
- Modify: `http_client/src/chttp_cookie_jar.c`
- Modify: `http_client/tests/chttp_cookie_jar_test.c`
- Modify: `http_client/CMakeLists.txt`

**Interfaces:**
- Produces:

```c
int chttp_cookie_jar_store_set_cookie(chttp_cookie_jar *jar,
                                      const chttp_cookie_request_context *context,
                                      const char *set_cookie_value);
```

Return semantics:
- `SALTS_OK` for accepted cookie, ignored malformed/rejected cookie, exact deletion, or deterministic eviction.
- `SALTS_EINVAL` only for invalid internal call arguments.
- Memory pressure while retaining optional cookie state drops that candidate and keeps the response path successful; no partially written record remains.

- [ ] **Step 1: Add RED parser/storage tests**

Add separate tests for:

```text
basic name=value
nameless cookie behavior
4096-octet name+value boundary
control-byte rejection
last valid Domain attribute
default Path and explicit Path
Max-Age precedence over Expires
Max-Age=0 exact deletion
unparseable Expires -> session cookie
host-only vs Domain identity coexistence
same name/domain with different Path coexistence
replacement preserves creation sequence
Secure rejected from plaintext
insecure overwrite cannot shadow overlapping Secure cookie
HttpOnly retained
SameSite Lax/Strict/None/default retained
SameSite=None without Secure rejected
case-insensitive __Secure- requirement detection
case-insensitive __Host- requirement detection
__Host- requires explicit Path=/ and no Domain
unknown attributes ignored
```

Inject clocks so one test advances monotonic time while moving realtime backwards, proving a Max-Age cookie expires only by monotonic time. Add the inverse Expires test using `realtime_ms`.

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

- [ ] **Step 3: Implement parser and exact record identity**

Use one allocation per retained cookie payload sized exactly for normalized `name`, `value`, `domain`, and `path`, with cookie count and per-field protocol bounds preventing unbounded growth. Never retain the raw Set-Cookie field as a second truth source.

Represent expiry explicitly:

```c
typedef enum chttp_cookie_expiry_kind {
  CHTTP_COOKIE_SESSION = 0,
  CHTTP_COOKIE_MONOTONIC_DEADLINE,
  CHTTP_COOKIE_REALTIME_DEADLINE
} chttp_cookie_expiry_kind;
```

- [ ] **Step 4: Reuse DateTimeParser where valid and add only a cookie-local compatibility layer if tests prove it necessary**

Call:

```c
datetime_t parsed;
if (datetime_parse(text, text_size, &parsed) == 0) {
  time_t epoch = datetime_to_time(&parsed);
  ...
}
```

Add focused cookie-date parsing for RFC-required legacy forms only if a RED cookie-date test fails against `datetime_parse`; do not modify `Salts::DateTimeParser` in this task.

- [ ] **Step 5: Run GREEN**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add http_client/src/chttp_cookie_jar.[ch] http_client/tests/chttp_cookie_jar_test.c http_client/CMakeLists.txt
git commit -m "feat(client): parse and retain scoped cookies"
```

---

### Task 4: Implement retrieval ordering, explicit override detection, bounded eviction, and generated Cookie serialization

**Files:**
- Modify: `http_client/src/chttp_cookie_jar.h`
- Modify: `http_client/src/chttp_cookie_jar.c`
- Modify: `http_client/tests/chttp_cookie_jar_test.c`

**Interfaces:**
- Produces:

```c
typedef struct chttp_cookie_prepared_request {
  chttp_request_options options;
  chttp_cookie_request_context context;
  chttp_header *owned_headers;
  char *owned_cookie_value;
} chttp_cookie_prepared_request;

int chttp_cookie_jar_prepare_request(chttp_cookie_jar *jar,
                                     const chttp_request_options *input,
                                     size_t max_header_count,
                                     size_t max_header_bytes,
                                     chttp_cookie_prepared_request *out);
void chttp_cookie_prepared_request_destroy(chttp_cookie_prepared_request *prepared);
void chttp_cookie_prepared_request_move_context(chttp_cookie_request_context *destination,
                                                chttp_cookie_prepared_request *prepared);
void chttp_cookie_jar_mark_request_admitted(chttp_cookie_jar *jar);
```

`out->options` is a shallow copy of the caller options. If no automatic Cookie is needed or the caller supplied `Cookie`, it borrows the original header array. If automatic cookies are selected, allocate an owned `header_count + 1` array, copy header descriptors, append exactly one generated `Cookie` descriptor, and point `out->options.headers` at that array.

- [ ] **Step 1: Add RED retrieval/eviction tests**

Cover:

```text
host-only and Domain retrieval
Secure omitted on plaintext and included on TLS
expired cookies omitted
longer Path first
earlier creation sequence tie-break
same-name different-scope serialization
explicit caller Cookie suppresses automatic generation
response storage still works after an explicit override request
generated header over cookie-policy bound -> SALTS_EMSGSIZE
generated header over existing request header bound -> SALTS_EMSGSIZE
per-domain LRU eviction
total-capacity LRU eviction
stable slot-order final tie-break
```

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

- [ ] **Step 3: Implement selection, ordering, serializer, and deterministic eviction**

Do not truncate the selected set. If the full generated value cannot fit, return `SALTS_EMSGSIZE` and leave caller options untouched.

- [ ] **Step 4: Run GREEN**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add http_client/src/chttp_cookie_jar.[ch] http_client/tests/chttp_cookie_jar_test.c
git commit -m "feat(client): select and serialize request cookies"
```

---

### Task 5: Integrate one jar into async request admission and preserve per-request response context

**Files:**
- Modify: `http_client/src/chttp_client_internal.h`
- Modify: `http_client/src/chttp_client.c`
- Modify: `http_client/tests/chttp_api_test.c`

**Interfaces:**
- Add to `chttp_slot`:

```c
chttp_cookie_request_context cookie_context;
```

- Add to `chttp_client_impl`:

```c
chttp_cookie_jar *cookie_jar;
bool owns_cookie_jar;
```

- Add internal init:

```c
int chttp_async_client_init_with_cookie_jar(chttp_async_client *client,
                                            const chttp_client_config *config,
                                            chttp_cookie_jar *borrowed_cookie_jar);
```

Public `chttp_async_client_init()` calls the same implementation with no borrowed jar and owns the created default jar. The borrowed path never destroys the jar.

- [ ] **Step 1: Write RED async admission tests**

In `chttp_api_test.c`, add a fake/local server flow proving that a client can initialize with the default jar and that immediate admission errors do not corrupt/destroy jar state. Keep the test focused on lifecycle here; H1/H2 cookie round trips come in later tasks.

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target chttp_api_test
ctest --test-dir build -R '^chttp_api_test$' --output-on-failure
```

- [ ] **Step 3: Wrap both H1 and H2 submit paths with prepared effective options**

At the top of `chttp_async_client_submit_impl()`:

```c
chttp_cookie_prepared_request prepared = {0};
status = chttp_cookie_jar_prepare_request(impl->cookie_jar, options,
                                          impl->limits.max_header_count,
                                          impl->limits.max_header_bytes,
                                          &prepared);
if (status != SALTS_OK) return status;
options = &prepared.options;
```

Use `options` normally for either `chttp_h2_submit()` or `chttp_request_build()`. Move `prepared.context` into the chosen `chttp_slot` before the request can complete. Destroy only temporary header/cookie buffers after synchronous H1 serialization or H2 submission has copied the header values.

Call `chttp_cookie_jar_mark_request_admitted()` only on the path that will return `SALTS_OK` to the caller, so cookie policy becomes non-reconfigurable after the first admitted request, not after a failed attempt.

- [ ] **Step 4: Release cookie context in `chttp_slot_release()` and jar ownership in async destroy**

Ensure every failure path either returns the prepared context to its temporary owner or releases the moved slot context exactly once.

- [ ] **Step 5: Run GREEN plus existing request tests**

```bash
cmake --build build --target chttp_api_test chttp_request_test chttp_h2_client_test
ctest --test-dir build -R '^(chttp_api_test|chttp_request_test|chttp_h2_client_test)$' --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add http_client/src/chttp_client_internal.h http_client/src/chttp_client.c http_client/tests/chttp_api_test.c
git commit -m "feat(client): prepare cookie-aware requests"
```

---

### Task 6: Ingest every H1 and H2 Set-Cookie before user completion

**Files:**
- Modify: `http_client/src/chttp_cookie_jar.h`
- Modify: `http_client/src/chttp_cookie_jar.c`
- Modify: `http_client/src/chttp_client.c`
- Modify: `http_client/tests/chttp_requests_test.c`
- Modify: `http_client/tests/chttp_h2_client_test.c`

**Interfaces:**
- Produces:

```c
void chttp_cookie_jar_ingest_response(chttp_cookie_jar *jar,
                                      const chttp_cookie_request_context *context,
                                      const chttp_response_view *response);
```

The function iterates `response->headers[0..header_count)` and calls `chttp_cookie_jar_store_set_cookie()` for every case-insensitive `Set-Cookie` name. It never uses the first-header convenience APIs.

- [ ] **Step 1: Add H1 RED integration test**

Create a server route sequence in `chttp_requests_test.c`:

1. `/login` returns two separate headers: `Set-Cookie: sid=abc; Path=/; HttpOnly` and `Set-Cookie: theme=dark; Path=/ui`.
2. `/ui/page` asserts the next client request contains both cookies in correct order/scope.
3. `/api` asserts only `sid=abc` is present.

The client must not manually supply `Cookie`.

- [ ] **Step 2: Add H2 RED integration test**

In `chttp_h2_client_test.c`, create the equivalent H2 flow with two repeated `set-cookie` response fields. Assert the next H2 request receives the generated `cookie` field and that repeated response headers remain separately visible in `response.header_count`.

- [ ] **Step 3: Run RED**

```bash
cmake --build build --target chttp_requests_test chttp_h2_client_test
ctest --test-dir build -R '^(chttp_requests_test|chttp_h2_client_test)$' --output-on-failure
```

- [ ] **Step 4: Ingest H1 before `chttp_slot_deliver()`**

In `chttp_slot_complete_response()`, call `chttp_cookie_jar_ingest_response(slot->client->cookie_jar, &slot->cookie_context, &slot->response_parser.response)` before the callback is delivered. Also cover the EOF completion path that currently delivers a completed parser directly, so every successful final H1 response passes through the same ingestion helper.

- [ ] **Step 5: Ingest H2 before `chttp_slot_deliver()`**

In `chttp_h2_complete()`, when `status == SALTS_OK && response != NULL`, ingest against `slot->cookie_context` before delivering the callback. H2 protocol code continues to preserve repeated fields; no Set-Cookie logic belongs in HPACK.

- [ ] **Step 6: Run GREEN**

```bash
cmake --build build --target chttp_requests_test chttp_h2_client_test
ctest --test-dir build -R '^(chttp_requests_test|chttp_h2_client_test)$' --output-on-failure
```

- [ ] **Step 7: Commit**

```bash
git add http_client/src/chttp_cookie_jar.[ch] http_client/src/chttp_client.c http_client/tests/chttp_requests_test.c http_client/tests/chttp_h2_client_test.c
git commit -m "feat(client): ingest response cookies on h1 and h2"
```

---

### Task 7: Preserve blocking-client cookies across internal timeout recovery

**Files:**
- Modify: `http_client/src/chttp_requests.c`
- Modify: `http_client/src/chttp_client_internal.h`
- Modify: `http_client/tests/chttp_requests_test.c`

**Interfaces:**
- `chttp_blocking_client_impl` owns one `chttp_cookie_jar *cookie_jar`.
- Its internal async client is always initialized with `chttp_async_client_init_with_cookie_jar(..., impl->cookie_jar)` and therefore borrows rather than owns that jar.

- [ ] **Step 1: Write the RED recovery regression test**

Test sequence:

1. successful request stores `sid=before-recovery`;
2. a controlled request forces the blocking wrapper through `chttp_requests_recover()`;
3. next successful request must still send `Cookie: sid=before-recovery`.

This test is mandatory because the pre-cookie blocking implementation destroys and reinitializes its internal async client during recovery.

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target chttp_requests_test
ctest --test-dir build -R '^chttp_requests_test$' --output-on-failure
```

- [ ] **Step 3: Move jar ownership to the blocking owner**

In `chttp_client_init()`, create the jar first, then initialize `impl->async` with it as borrowed state. In `chttp_requests_recover()`, reinitialize the async transport with the same jar pointer. In `chttp_client_destroy()`, destroy the jar only after internal async teardown succeeds.

- [ ] **Step 4: Run GREEN**

```bash
cmake --build build --target chttp_requests_test
ctest --test-dir build -R '^chttp_requests_test$' --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add http_client/src/chttp_requests.c http_client/src/chttp_client_internal.h http_client/tests/chttp_requests_test.c
git commit -m "fix(client): preserve cookies across transport recovery"
```

---

### Task 8: Add the complete public cookie configuration/management API without changing existing struct ABI

**Files:**
- Modify: `http_client/include/http_client/http.h`
- Modify: `http_client/src/chttp_client.c`
- Modify: `http_client/src/chttp_requests.c`
- Create: `http_client/tests/chttp_cookie_header_cpp_test.cpp`
- Modify: `http_client/tests/CMakeLists.txt`

**Interfaces:**
- Add exactly:

```c
typedef struct chttp_cookie_jar_options {
  size_t size;
  int enabled;
  size_t cookie_capacity;
  size_t cookies_per_domain;
  size_t max_cookie_header_bytes;
} chttp_cookie_jar_options;

#define CHTTP_COOKIE_JAR_OPTIONS_INIT \
  {sizeof(chttp_cookie_jar_options), 1, 0u, 0u, 0u}

int chttp_client_set_cookie_jar_options(chttp_client *client,
                                        const chttp_cookie_jar_options *options);
int chttp_async_client_set_cookie_jar_options(chttp_async_client *client,
                                              const chttp_cookie_jar_options *options);
int chttp_client_cookies_clear(chttp_client *client);
int chttp_async_client_cookies_clear(chttp_async_client *client);
int chttp_client_cookie_count(const chttp_client *client, size_t *out_count);
int chttp_async_client_cookie_count(const chttp_async_client *client, size_t *out_count);
```

- Set-options only succeeds before first admitted request; later calls return `SALTS_EBUSY`.
- `enabled == 0` disables storage/replay; zero size/capacity fields otherwise select defaults.
- `clear` preserves configured policy; `count` ignores expired records without mutating a const client.

- [ ] **Step 1: Write the RED public-header/ABI compile test**

Create `chttp_cookie_header_cpp_test.cpp` with a shadow of the legacy `chttp_client_config` fields from pre-cookie master and assert the public struct has not grown:

```cpp
struct legacy_chttp_client_config {
  cnet_client_config network;
  size_t request_capacity;
  size_t max_start_line_bytes;
  size_t max_header_count;
  size_t max_header_bytes;
  size_t max_request_body_bytes;
  size_t max_response_body_bytes;
  size_t max_informational_responses;
  size_t stream_chunk_bytes;
  size_t h2_input_buffer_bytes;
  size_t h2_hpack_dynamic_table_bytes;
  size_t h2_max_settings_count;
};

static_assert(sizeof(chttp_client_config) == sizeof(legacy_chttp_client_config));
static_assert(sizeof(CHTTP_COOKIE_JAR_OPTIONS_INIT) != 0); // replace with an actual initialized object below
```

Use an actual function body for the macro compile check:

```cpp
int main() {
  chttp_cookie_jar_options options = CHTTP_COOKIE_JAR_OPTIONS_INIT;
  return options.size == sizeof(options) && options.enabled == 1 ? 0 : 1;
}
```

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target chttp_cookie_header_cpp_test
```

Expected: compile failure because the new public type/functions do not exist.

- [ ] **Step 3: Add declarations and fully functional implementations in the same task**

Do not publish stub APIs. Wire blocking management calls directly to the blocking-owned jar and async management calls to `chttp_client_impl->cookie_jar`.

- [ ] **Step 4: Add behavior tests for enable/disable, set-before-first-request, EBUSY-after-admission, clear, and count**

Add those to `chttp_api_test.c` or `chttp_requests_test.c` using real clients, not mocks.

- [ ] **Step 5: Run GREEN**

```bash
cmake --build build --target chttp_cookie_header_cpp_test chttp_api_test chttp_requests_test
ctest --test-dir build -R '^(chttp_cookie_header_cpp_test|chttp_api_test|chttp_requests_test)$' --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add http_client/include/http_client/http.h http_client/src/chttp_client.c http_client/src/chttp_requests.c http_client/tests/chttp_cookie_header_cpp_test.cpp http_client/tests/CMakeLists.txt http_client/tests/chttp_api_test.c http_client/tests/chttp_requests_test.c
git commit -m "feat(client): expose bounded cookie jar controls"
```

---

### Task 9: Cover explicit override, deletion, secure transport, and H2 concurrency end to end

**Files:**
- Modify: `http_client/tests/chttp_requests_test.c`
- Modify: `http_client/tests/chttp_tls_test.c`
- Modify: `http_client/tests/chttp_h2_client_test.c`

**Interfaces:**
- No new production interfaces.

- [ ] **Step 1: Add explicit `Cookie` override round trip**

Store `sid=jar`, send one request with explicit `Cookie: sid=manual`, assert server sees only `sid=manual`, then make another request without explicit Cookie and assert jar replay returns to `sid=jar` unless that response updated it.

- [ ] **Step 2: Add exact deletion test**

Store same-name cookies at `/` and `/admin`; send `Set-Cookie` with `Max-Age=0` targeting only `/admin`; assert `/` survives.

- [ ] **Step 3: Add TLS Secure tests**

Using existing TLS test material, prove:

- a TLS response can store `Secure` and the next TLS request sends it;
- a plaintext request does not send it;
- plaintext `Set-Cookie: sid=evil; Secure` cannot replace the TLS secure cookie.

- [ ] **Step 4: Add H2 concurrent stream ordering test**

Admit two H2 requests before either response completes. Have stream A set a cookie and stream B complete without one. Assert both already-admitted request headers remain unchanged, while a third request admitted after stream A's response observes the new cookie. This locks the owner-thread temporal semantics.

- [ ] **Step 5: Run focused integration GREEN**

```bash
cmake --build build --target chttp_requests_test chttp_tls_test chttp_h2_client_test
ctest --test-dir build -R '^(chttp_requests_test|chttp_tls_test|chttp_h2_client_test)$' --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add http_client/tests/chttp_requests_test.c http_client/tests/chttp_tls_test.c http_client/tests/chttp_h2_client_test.c
git commit -m "test(client): cover cookie transport semantics"
```

---

### Task 10: Documentation, dependency boundary audit, and branch CI

**Files:**
- Modify: `http_client/README.md`
- Modify: `README.md` only if the top-level capability list needs one concise cookie bullet.
- Create: `.github/workflows/client-cookie-jar.yml`

**Interfaces:**
- No new runtime interfaces.

- [ ] **Step 1: Document user-visible behavior**

Document:

```text
- automatic jar is enabled by default
- Set-Cookie is stored/replayed for H1 and H2
- explicit Cookie is a one-request override
- Secure/Domain/Path/expiry/prefix/public-suffix rules are enforced
- SameSite is retained but no browser navigation context is simulated
- cookie state lives for the client lifetime, not process persistence
- set_cookie_jar_options must be called before first admitted request
- clear/count semantics
```

- [ ] **Step 2: Add an exact-head cookie workflow**

Use a matrix for `ubuntu-24.04`, `windows-latest`, and `macos-latest`. Follow the existing repository dependency bootstrap pattern, pin the same Salts/SaltsUtils/vcpkg baselines used by current Chttp verification, and run:

```text
configure exact Chttp head
build chttp_cookie_jar_test
build chttp_cookie_header_cpp_test
build chttp_requests_test
build chttp_tls_test
build chttp_h2_client_test
run those CTests
build full Chttp graph
run full CTest
verify git rev-parse HEAD equals expected SHA
verify git status --porcelain is empty
```

Do not add install/export verification code to production CMake.

- [ ] **Step 3: Verify `libpsl` remains private**

Inspect generated CMake target interfaces or CMake source statically. `CHttp::Client` may link `PkgConfig::LIBPSL` privately; `cmake/ChttpConfig.cmake.in` must remain unchanged unless a concrete export failure proves otherwise.

- [ ] **Step 4: Run local/full verification where the configured environment permits**

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: zero failed tests.

- [ ] **Step 5: Commit**

```bash
git add http_client/README.md README.md .github/workflows/client-cookie-jar.yml
git commit -m "docs(ci): verify client cookie jar"
```

---

### Task 11: Final exact-head audit and merge-readiness gate

**Files:**
- Review all files changed since `design/client-cookie-jar` base.
- No production modification unless a failing gate identifies a concrete defect.

**Interfaces:**
- No new interfaces.

- [ ] **Step 1: Run the complete exact-head CI and capture the run IDs**

Required evidence:

```text
Linux cookie workflow: success
Windows cookie workflow: success
macOS cookie workflow: success
full Chttp build: success
full CTest: success
clean tracked source: success
```

- [ ] **Step 2: Audit spec coverage line by line**

Explicitly map every design section to implemented tests/code:

```text
public ABI
bounded defaults/configuration
Domain/host-only/PSL
Path/default-path
Max-Age/Expires clock separation
Secure/insecure overwrite
HttpOnly
SameSite retention
__Secure-/__Host-
identity/replacement/deletion
LRU eviction
serialization/order/overflow
explicit Cookie override
H1 ingestion/replay
H2 ingestion/replay/concurrency
blocking recovery persistence
```

- [ ] **Step 3: Search for forbidden shortcuts**

Search the diff for:

```text
TODO
FIXME
HACK
comma-splitting Set-Cookie
connection-owned cookie storage
manual duplicated PSL data
changes to existing public config struct layouts
CMake install-verify production code
```

Expected: none relevant.

- [ ] **Step 4: Request formal code review**

Review from the original base SHA through final head, with the committed spec and this plan as requirements. Fix every Critical/Important finding and rerun the affected focused tests plus full exact-head CI.

- [ ] **Step 5: Prepare PR as ready only after exact-head evidence is green**

PR summary must distinguish:

```text
implemented behavior
protocol boundary (generic client, not browser SameSite navigation policy)
ABI preservation
private libpsl dependency
focused test evidence
full CTest evidence
platform matrix evidence
```

Do not merge on partial/focused tests alone.
