# CHTTP Client Cookie Jar Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded automatic RFC cookie jar to `CHttp::Client` so HTTP/1.1 and HTTP/2 ingest every valid `Set-Cookie` field and replay matching cookies automatically, while preserving current public struct ABI and explicit-header behavior.

**Architecture:** Cookie protocol logic lives in a focused internal `chttp_cookie_jar` module shared by H1 and H2. A standalone `chttp_async_client` owns its jar. A blocking `chttp_client` owns its jar outside its recoverable internal async transport; the internal async client borrows that jar so timeout recovery cannot erase cookie state. Request admission prepares one canonical cookie context and an optional generated `Cookie` field, then stores the context in the request slot so response cookies can be ingested before user completion.

**Tech Stack:** C11, CMake, TinyTest, llhttp, CNet, Salts clocks (`salts_monotonic_ms`, `salts_realtime_ms`), `Salts::DateTimeParser`, vcpkg `libpsl` 0.21.5 via CMake `FindPkgConfig`, existing H1 serializer and H2 HPACK implementation.

**Spec:** `docs/superpowers/specs/2026-09-17-client-cookie-jar-design.md`

## Global Constraints

- Implementation base is `master` at `d2643228b458ee6438dc856261f7434143c8eccf`; if master moves before execution, rebase first and rerun all RED/GREEN gates.
- Do not change the layout of `chttp_client_config`, `chttp_options`, or `chttp_request_options`.
- Automatic cookies are enabled by default; an explicit caller `Cookie` field is a complete one-request override.
- H1 and H2 share one jar implementation and one owner-thread mutation order.
- Default capability is at least 3000 cookies total and 50 cookies per canonical domain bucket.
- Reject cookie name+value above 4096 octets. Ignore Domain/Path attributes above 1024 octets as scope attributes.
- Replacement identity is `(name, domain, host_only, path)`.
- `Max-Age` uses a monotonic deadline. `Expires` uses a realtime UTC deadline. `Max-Age` wins when both are valid.
- Parse and retain SameSite metadata but do not fabricate browser top-level-site/navigation state.
- Enforce Secure, insecure-overwrite protection, `__Secure-`, `__Host-`, host-only/domain/path rules, and public-suffix protection from the committed design.
- Never comma-combine `Set-Cookie`; ingest repeated response fields independently.
- Never truncate generated `Cookie`; return `SALTS_EMSGSIZE` before sending request bytes.
- Rejected/malformed/evicted response cookies do not turn a valid HTTP response into an HTTP failure.
- `libpsl` is private to `CHttp::Client`; `cmake/ChttpConfig.cmake.in` stays unchanged unless a real static/export failure proves otherwise.
- Do not add CMake install-verification production code.
- Use exact-head CI before any merge-readiness claim.

---

## File Structure

### Create

- `http_client/src/chttp_cookie_jar.h` — internal cookie store, clock seam, request context, prepare/store/ingest/query APIs.
- `http_client/src/chttp_cookie_jar.c` — parsing, normalization, PSL checks, expiry, replacement, eviction, matching, sorting, serialization.
- `http_client/tests/chttp_cookie_jar_test.c` — deterministic internal jar tests using injected clocks.
- `http_client/tests/chttp_cookie_header_cpp_test.cpp` — public header compile test and legacy-struct ABI guard.
- `.github/workflows/client-cookie-jar.yml` — exact-head Linux/macOS/Windows cookie verification plus Android compile verification.

### Modify

- `vcpkg.json` — add `libpsl`.
- `http_client/CMakeLists.txt` — compile cookie module; link `PkgConfig::LIBPSL` and `Salts::DateTimeParser` privately.
- `http_client/include/http_client/http.h` — add only a versioned cookie options type and management functions.
- `http_client/src/chttp_client_internal.h` — internal async-init entry point that may borrow a blocking owner's jar.
- `http_client/src/chttp_client.c` — async jar ownership, request preparation, slot context, response ingestion, async management API.
- `http_client/src/chttp_requests.c` — blocking jar ownership, recovery preservation, blocking management API.
- `http_client/tests/CMakeLists.txt` — register new tests.
- `http_client/tests/chttp_api_test.c` — lifecycle/configuration behavior.
- `http_client/tests/chttp_requests_test.c` — H1 cookie replay, override, deletion, recovery tests.
- `http_client/tests/chttp_tls_test.c` — Secure/insecure-overwrite integration.
- `http_client/tests/chttp_h2_client_test.c` — H2 replay, repeated fields, concurrent stream temporal behavior.
- `http_client/README.md` — cookie client behavior and API.

### Intentionally keep policy out of

- `http_client/src/chttp_request.c`: it keeps serializing the header array it receives.
- `http_client/src/chttp_response.c`: it already stores every H1 response header independently.
- `http_client/src/chttp_h2_session.c`: it already stores every regular H2 header independently; HPACK remains cookie-policy-free.

---

### Task 1: Add bounded cookie-jar core and deterministic clocks

**Files:**
- Create: `http_client/src/chttp_cookie_jar.h`
- Create: `http_client/src/chttp_cookie_jar.c`
- Create: `http_client/tests/chttp_cookie_jar_test.c`
- Modify: `http_client/CMakeLists.txt`
- Modify: `http_client/tests/CMakeLists.txt`

**Interfaces:**

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
int chttp_cookie_jar_set_options(chttp_cookie_jar *jar,
                                 const chttp_cookie_jar_options_internal *options);
void chttp_cookie_jar_lock_options(chttp_cookie_jar *jar);
void chttp_cookie_jar_destroy(chttp_cookie_jar *jar);
void chttp_cookie_jar_clear(chttp_cookie_jar *jar);
size_t chttp_cookie_jar_count(const chttp_cookie_jar *jar);
```

`set_options` returns `SALTS_EBUSY` after `lock_options`. Zero internal capacity fields are normalized to named defaults. Production clock callbacks call `salts_monotonic_ms()` and `salts_realtime_ms()`.

- [ ] **Step 1: Register the test target**

Add:

```cmake
add_executable(chttp_cookie_jar_test chttp_cookie_jar_test.c)
target_link_libraries(chttp_cookie_jar_test PRIVATE CHttp::Client Salts::TinyTest)
target_include_directories(chttp_cookie_jar_test PRIVATE ${PROJECT_SOURCE_DIR}/http_client/src)
add_test(NAME chttp_cookie_jar_test COMMAND chttp_cookie_jar_test)
set_target_properties(chttp_cookie_jar_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
```

- [ ] **Step 2: Write RED lifecycle/clock tests**

Use:

```c
typedef struct test_clock {
  uint64_t monotonic_ms;
  uint64_t realtime_ms;
} test_clock;

static uint64_t test_monotonic(void *user) { return ((test_clock *)user)->monotonic_ms; }
static uint64_t test_realtime(void *user) { return ((test_clock *)user)->realtime_ms; }
```

Test create/count/clear, disabled mode, option normalization, `set_options` before lock, and `SALTS_EBUSY` after lock.

- [ ] **Step 3: Run RED**

```bash
cmake --build build --target chttp_cookie_jar_test
```

Expected: compile/link failure because the module does not exist.

- [ ] **Step 4: Implement lifecycle only**

Create bounded record metadata storage with overflow-checked allocation. Do not parse `Set-Cookie` in this task.

- [ ] **Step 5: Run GREEN**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add http_client/src/chttp_cookie_jar.c http_client/src/chttp_cookie_jar.h http_client/tests/chttp_cookie_jar_test.c http_client/tests/CMakeLists.txt http_client/CMakeLists.txt
git commit -m "feat(client): add bounded cookie jar core"
```

---

### Task 2: Add canonical host/path context and public-suffix protection

**Files:**
- Modify: `vcpkg.json`
- Modify: `http_client/CMakeLists.txt`
- Modify: `http_client/src/chttp_cookie_jar.h`
- Modify: `http_client/src/chttp_cookie_jar.c`
- Modify: `http_client/tests/chttp_cookie_jar_test.c`

**Interfaces:**

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

- [ ] **Step 1: Write RED context tests**

Cover:

```text
WWW.Example.COM:443 -> www.example.com
[2001:db8::1]:443 -> 2001:db8::1
/a/b?x=1 -> request path /a/b and default cookie path /a
badexample.com does not domain-match example.com
/foo does not path-match /foobar
/foo path-matches /foo/bar
Domain=com rejected from www.example.com
Domain=example.com accepted from www.example.com
public-suffix exact-host case follows the target algorithm rather than widening scope
IP literal never gains widened Domain scope
```

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

- [ ] **Step 3: Add vcpkg/CMake dependency exactly**

Add `"libpsl"` to `vcpkg.json`.

In `http_client/CMakeLists.txt`, before `add_library(chttp_client SHARED ...)` add:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPSL REQUIRED IMPORTED_TARGET libpsl)
```

Append `PkgConfig::LIBPSL` to the existing `PRIVATE` list in `target_link_libraries(chttp_client ...)`. Do not add it to the `PUBLIC` list and do not edit `cmake/ChttpConfig.cmake.in`.

- [ ] **Step 4: Implement canonicalization and PSL checks**

Use:

```c
const psl_ctx_t *psl = psl_builtin();
const int is_public = psl_is_public_suffix2(psl, domain, PSL_TYPE_ANY);
```

Implement label-boundary domain matching, bracket/port stripping, IPv4/IPv6 no-widening behavior, RFC default-path, and RFC path-match.

- [ ] **Step 5: Run GREEN**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add vcpkg.json http_client/CMakeLists.txt http_client/src/chttp_cookie_jar.c http_client/src/chttp_cookie_jar.h http_client/tests/chttp_cookie_jar_test.c
git commit -m "feat(client): add cookie scope and public suffix rules"
```

---

### Task 3: Parse and retain Set-Cookie correctly

**Files:**
- Modify: `http_client/CMakeLists.txt`
- Modify: `http_client/src/chttp_cookie_jar.h`
- Modify: `http_client/src/chttp_cookie_jar.c`
- Modify: `http_client/tests/chttp_cookie_jar_test.c`

**Interfaces:**

```c
typedef enum chttp_cookie_expiry_kind {
  CHTTP_COOKIE_SESSION = 0,
  CHTTP_COOKIE_MONOTONIC_DEADLINE,
  CHTTP_COOKIE_REALTIME_DEADLINE
} chttp_cookie_expiry_kind;

int chttp_cookie_jar_store_set_cookie(chttp_cookie_jar *jar,
                                      const chttp_cookie_request_context *context,
                                      const char *set_cookie_value);
```

`store_set_cookie` returns `SALTS_OK` for accepted cookies, ignored malformed/rejected cookies, exact deletion, and deterministic eviction. It returns `SALTS_EINVAL` only for invalid internal call arguments. A failed optional cookie payload allocation drops that candidate without leaving a partial record or failing the HTTP response path.

- [ ] **Step 1: Add RED parser/storage tests**

Cover each rule independently:

```text
basic name=value
nameless-cookie behavior
4096 boundary and 4097 rejection
control-character rejection
last valid Domain attribute
over-1024 Domain ignored as a Domain scope attribute
over-1024 Path ignored as a Path scope attribute
explicit Path and default Path
host-only and Domain identities coexist
same name/domain different Path coexist
replacement preserves creation sequence
Max-Age wins over Expires
Max-Age=0 exact deletion
Expires year < 1601 is not a valid persistent expiry
unparseable Expires leaves a session cookie
Secure rejected from plaintext
plaintext cannot shadow an overlapping Secure cookie
HttpOnly retained
SameSite Lax/Strict/None/default retained
SameSite=None without Secure rejected
case-insensitive __Secure- requirement detection
case-insensitive __Host- requirement detection
__Host- requires explicit Path=/ and no accepted Domain
unknown attributes ignored
```

Use injected clocks to prove a Max-Age cookie expires when monotonic time advances even if realtime moves backward, and an Expires cookie uses realtime rather than monotonic time.

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

- [ ] **Step 3: Link DateTimeParser privately**

Append `Salts::DateTimeParser` to the existing `PRIVATE` link list of `chttp_client`.

- [ ] **Step 4: Implement parser/storage**

Use one allocation per retained cookie payload sized for normalized name, value, domain, and path. Keep the raw `Set-Cookie` string out of retained state.

For Expires:

```c
datetime_t parsed;
if (datetime_parse(text, text_size, &parsed) == 0 && parsed.year >= 1601) {
  const time_t epoch = datetime_to_time(&parsed);
  if (epoch >= 0) {
    /* store saturated realtime deadline in milliseconds */
  }
}
```

If committed cookie-date tests reveal an RFC legacy form not accepted by `datetime_parse`, implement only that compatibility grammar inside `chttp_cookie_jar.c`; do not modify SaltsUtils in this feature.

- [ ] **Step 5: Run GREEN**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add http_client/CMakeLists.txt http_client/src/chttp_cookie_jar.c http_client/src/chttp_cookie_jar.h http_client/tests/chttp_cookie_jar_test.c
git commit -m "feat(client): parse and retain scoped cookies"
```

---

### Task 4: Select, evict, order, and serialize request cookies

**Files:**
- Modify: `http_client/src/chttp_cookie_jar.h`
- Modify: `http_client/src/chttp_cookie_jar.c`
- Modify: `http_client/tests/chttp_cookie_jar_test.c`

**Interfaces:**

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
```

`out->options` is a shallow copy. When automatic cookies are selected, allocate an owned `header_count + 1` descriptor array, copy descriptors, append exactly one `Cookie` descriptor, and point the copied options at it. Detect caller `Cookie` case-insensitively and perform no automatic merge for that request.

- [ ] **Step 1: Add RED selection/eviction tests**

Cover:

```text
host-only/domain retrieval
Secure omitted on plaintext and included on TLS
expired omitted
longer Path first
earlier creation sequence tie-break
same-name different-scope serialization
explicit Cookie suppresses automatic Cookie only for that request
per-domain least-recently-accessed eviction
total least-recently-accessed eviction
creation sequence then stable slot order break eviction ties
generated Cookie over configured cookie header bound -> SALTS_EMSGSIZE
generated Cookie over existing request header bound -> SALTS_EMSGSIZE
```

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

- [ ] **Step 3: Implement selection/serialization**

Purge expired records before selection/insertion. Never truncate a selected set. Return `SALTS_EMSGSIZE` with caller options unchanged if the complete generated field cannot fit.

- [ ] **Step 4: Run GREEN**

```bash
cmake --build build --target chttp_cookie_jar_test
ctest --test-dir build -R '^chttp_cookie_jar_test$' --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add http_client/src/chttp_cookie_jar.c http_client/src/chttp_cookie_jar.h http_client/tests/chttp_cookie_jar_test.c
git commit -m "feat(client): select and serialize request cookies"
```

---

### Task 5: Integrate cookie preparation into async H1/H2 admission

**Files:**
- Modify: `http_client/src/chttp_client_internal.h`
- Modify: `http_client/src/chttp_client.c`
- Modify: `http_client/tests/chttp_api_test.c`

**Interfaces:**

Add to `chttp_slot`:

```c
chttp_cookie_request_context cookie_context;
```

Add to `chttp_client_impl`:

```c
chttp_cookie_jar *cookie_jar;
bool owns_cookie_jar;
```

Add internal init:

```c
int chttp_async_client_init_with_cookie_jar(chttp_async_client *client,
                                            const chttp_client_config *config,
                                            chttp_cookie_jar *borrowed_cookie_jar);
```

Public `chttp_async_client_init()` creates/owns a default jar. `chttp_async_client_init_with_cookie_jar()` borrows a non-NULL jar and never destroys it.

- [ ] **Step 1: Add RED async lifecycle test**

In `chttp_api_test.c`, initialize/destroy a standalone async client and prove failed immediate request admission does not lock cookie configuration. The successful-admission EBUSY policy is tested after the public API is exposed in Task 8.

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target chttp_api_test
ctest --test-dir build -R '^chttp_api_test$' --output-on-failure
```

- [ ] **Step 3: Prepare effective options before protocol branching**

Inside `chttp_async_client_submit_impl()` create:

```c
chttp_cookie_prepared_request prepared = {0};
```

Call `chttp_cookie_jar_prepare_request()` before selecting H1/H2. Pass `&prepared.options` to `chttp_request_build()` or `chttp_h2_submit()`. Move `prepared.context` into the selected slot before the request can complete. Destroy only temporary header/cookie buffers after H1 serialization or H2 submission has synchronously copied their values.

- [ ] **Step 4: Lock cookie options only on successful admission**

Immediately before each path returns `SALTS_OK` with a published `out_request`, call:

```c
chttp_cookie_jar_lock_options(impl->cookie_jar);
```

Failed admission leaves policy reconfigurable.

- [ ] **Step 5: Release context/ownership correctly**

`chttp_slot_release()` destroys its cookie context. Async destroy destroys the jar only when `owns_cookie_jar` is true.

- [ ] **Step 6: Run GREEN/regression tests**

```bash
cmake --build build --target chttp_api_test chttp_request_test chttp_h2_client_test
ctest --test-dir build -R '^(chttp_api_test|chttp_request_test|chttp_h2_client_test)$' --output-on-failure
```

- [ ] **Step 7: Commit**

```bash
git add http_client/src/chttp_client_internal.h http_client/src/chttp_client.c http_client/tests/chttp_api_test.c
git commit -m "feat(client): prepare cookie-aware requests"
```

---

### Task 6: Ingest every final H1/H2 Set-Cookie before callback delivery

**Files:**
- Modify: `http_client/src/chttp_cookie_jar.h`
- Modify: `http_client/src/chttp_cookie_jar.c`
- Modify: `http_client/src/chttp_client.c`
- Modify: `http_client/tests/chttp_requests_test.c`
- Modify: `http_client/tests/chttp_h2_client_test.c`

**Interfaces:**

```c
void chttp_cookie_jar_ingest_response(chttp_cookie_jar *jar,
                                      const chttp_cookie_request_context *context,
                                      const chttp_response_view *response);
```

The function iterates the complete header array and processes every case-insensitive `Set-Cookie` field. It never uses `chttp_response_header()` or `chttp_response_view_header()`, which intentionally return only the first matching header.

- [ ] **Step 1: Add H1 RED integration**

Have `/login` return two separate fields:

```text
Set-Cookie: sid=abc; Path=/; HttpOnly
Set-Cookie: theme=dark; Path=/ui
```

Then assert `/ui/page` receives both cookies and `/api` receives only `sid=abc`, with no caller-supplied Cookie header.

- [ ] **Step 2: Add H2 RED integration**

Return two independent lowercase `set-cookie` fields on one H2 response. Assert both remain separately visible in the response header array and that a subsequent H2 request receives the generated `cookie` field.

- [ ] **Step 3: Run RED**

```bash
cmake --build build --target chttp_requests_test chttp_h2_client_test
ctest --test-dir build -R '^(chttp_requests_test|chttp_h2_client_test)$' --output-on-failure
```

- [ ] **Step 4: Integrate H1**

In every successful final H1 completion path, call `chttp_cookie_jar_ingest_response()` before `chttp_slot_deliver()`. Refactor the EOF-success path to use the same cookie-aware final-completion helper rather than directly delivering the parser response.

- [ ] **Step 5: Integrate H2**

In `chttp_h2_complete()`, when `status == SALTS_OK && response != NULL`, ingest the response against `slot->cookie_context` before `chttp_slot_deliver()`.

- [ ] **Step 6: Run GREEN**

```bash
cmake --build build --target chttp_requests_test chttp_h2_client_test
ctest --test-dir build -R '^(chttp_requests_test|chttp_h2_client_test)$' --output-on-failure
```

- [ ] **Step 7: Commit**

```bash
git add http_client/src/chttp_cookie_jar.c http_client/src/chttp_cookie_jar.h http_client/src/chttp_client.c http_client/tests/chttp_requests_test.c http_client/tests/chttp_h2_client_test.c
git commit -m "feat(client): ingest response cookies on h1 and h2"
```

---

### Task 7: Preserve blocking-client cookie state across internal recovery

**Files:**
- Modify: `http_client/src/chttp_requests.c`
- Modify: `http_client/tests/chttp_requests_test.c`

**Interfaces:**

Extend `chttp_blocking_client_impl` with:

```c
chttp_cookie_jar *cookie_jar;
```

Its internal async client always borrows that exact jar through `chttp_async_client_init_with_cookie_jar()`.

- [ ] **Step 1: Write RED recovery regression**

Sequence:

```text
request 1 stores sid=before-recovery
request 2 triggers the existing chttp_requests_recover() path
request 3 must automatically send sid=before-recovery
```

Use the existing timeout/recovery harness in `chttp_requests_test.c`; do not expose a test-only recovery API.

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target chttp_requests_test
ctest --test-dir build -R '^chttp_requests_test$' --output-on-failure
```

- [ ] **Step 3: Move blocking ownership above the recoverable async transport**

`chttp_client_init()` creates the jar, then initializes `impl->async` with it as borrowed state. `chttp_requests_recover()` reinitializes async with the same jar. `chttp_client_destroy()` destroys the jar after the internal async client is successfully destroyed.

- [ ] **Step 4: Run GREEN**

```bash
cmake --build build --target chttp_requests_test
ctest --test-dir build -R '^chttp_requests_test$' --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add http_client/src/chttp_requests.c http_client/tests/chttp_requests_test.c
git commit -m "fix(client): preserve cookies across transport recovery"
```

---

### Task 8: Publish complete versioned cookie controls without ABI break

**Files:**
- Modify: `http_client/include/http_client/http.h`
- Modify: `http_client/src/chttp_client.c`
- Modify: `http_client/src/chttp_requests.c`
- Create: `http_client/tests/chttp_cookie_header_cpp_test.cpp`
- Modify: `http_client/tests/CMakeLists.txt`
- Modify: `http_client/tests/chttp_api_test.c`

**Public interfaces:**

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

- [ ] **Step 1: Write RED public-header/ABI test**

Create a shadow pre-cookie config:

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

int main() {
  chttp_cookie_jar_options options = CHTTP_COOKIE_JAR_OPTIONS_INIT;
  return options.size == sizeof(options) && options.enabled == 1 ? 0 : 1;
}
```

Register it as `chttp_cookie_header_cpp_test` linked to `CHttp::Client`.

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target chttp_cookie_header_cpp_test
```

Expected: compile failure because the new public type is absent.

- [ ] **Step 3: Add declarations and full implementations in one task**

Map the public options into `chttp_cookie_jar_options_internal` and call `chttp_cookie_jar_set_options()`. Do not publish stubs. Blocking APIs operate on `chttp_blocking_client_impl->cookie_jar`; async APIs operate on `chttp_client_impl->cookie_jar`.

- [ ] **Step 4: Add behavior tests**

Cover default-enabled behavior, explicit disable, custom capacity, configuration before first admitted request, `SALTS_EBUSY` after first admitted request, `clear`, and `count` ignoring expired records.

- [ ] **Step 5: Run GREEN**

```bash
cmake --build build --target chttp_cookie_header_cpp_test chttp_api_test chttp_requests_test
ctest --test-dir build -R '^(chttp_cookie_header_cpp_test|chttp_api_test|chttp_requests_test)$' --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add http_client/include/http_client/http.h http_client/src/chttp_client.c http_client/src/chttp_requests.c http_client/tests/chttp_cookie_header_cpp_test.cpp http_client/tests/CMakeLists.txt http_client/tests/chttp_api_test.c
git commit -m "feat(client): expose bounded cookie jar controls"
```

---

### Task 9: Lock transport/security edge cases end to end

**Files:**
- Modify: `http_client/tests/chttp_requests_test.c`
- Modify: `http_client/tests/chttp_tls_test.c`
- Modify: `http_client/tests/chttp_h2_client_test.c`

- [ ] **Step 1: Explicit override round trip**

Store `sid=jar`, send one request with explicit `Cookie: sid=manual`, assert the server sees only `sid=manual`, then send a request without explicit Cookie and assert automatic replay returns to `sid=jar` unless the override response changed the jar.

- [ ] **Step 2: Exact-scope deletion**

Store `sid=root; Path=/` and `sid=admin; Path=/admin`. Delete only `/admin` with `Max-Age=0`; prove the root cookie survives.

- [ ] **Step 3: Secure transport integrity**

Using existing TLS test material, prove a TLS response may set `Secure`, plaintext never sends it, and plaintext `Set-Cookie: sid=evil; Secure` cannot replace the secure cookie.

- [ ] **Step 4: H2 temporal concurrency**

Admit stream A and stream B before either response completes. Let A set a cookie. Assert A/B already-admitted request headers do not change, while request C admitted after A's response sees the new cookie.

- [ ] **Step 5: Run focused GREEN**

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

### Task 10: Document and establish exact-head platform verification

**Files:**
- Modify: `http_client/README.md`
- Create: `.github/workflows/client-cookie-jar.yml`

**Pinned verification baselines:**

```text
Salts:      801202e58c2d86b35202414d4812e79a2fd25bae
SaltsUtils: 661a7ac8cf4a5cbc1d13902d97add24d55e86301
vcpkg:      b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01
```

- [ ] **Step 1: Document behavior**

Document default auto-jar, H1/H2 ingestion/replay, explicit Cookie override, Secure/Domain/Path/expiry/prefix/PSL behavior, SameSite non-browser boundary, client-lifetime persistence, pre-first-request configuration, and clear/count APIs.

- [ ] **Step 2: Create hosted verification jobs**

Use `ubuntu-24.04`, `windows-latest`, and `macos-latest` for configure/build/focused CTest/full CTest. Use the repository's `android-arm64-v8a-release` preset in a separate compile-only job so the new `libpsl` dependency is proven compatible with the existing Android target.

Each desktop job must verify:

```text
exact dependency SHAs
configure exact Chttp head
build chttp_cookie_jar_test
build chttp_cookie_header_cpp_test
build chttp_requests_test
build chttp_tls_test
build chttp_h2_client_test
run focused cookie/client tests
build full Chttp graph
run full CTest
verify git rev-parse HEAD equals the workflow SHA
verify git status --porcelain is empty
```

Android must configure and build `CHttp::Client` from the exact head with the pinned vcpkg baseline; no emulator test is required in this feature.

- [ ] **Step 3: Confirm private dependency boundary**

`http_client/CMakeLists.txt` contains `PkgConfig::LIBPSL` only in the private client implementation dependency set. `cmake/ChttpConfig.cmake.in` remains unchanged.

- [ ] **Step 4: Run full local verification where the configured environment exists**

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add http_client/README.md .github/workflows/client-cookie-jar.yml
git commit -m "docs(ci): verify client cookie jar"
```

---

### Task 11: Final spec audit, review, and merge-readiness gate

**Files:**
- Review every changed file from base `d2643228b458ee6438dc856261f7434143c8eccf` to final head.
- Modify production only for a concrete failing gate or review finding.

- [ ] **Step 1: Require exact-head CI evidence**

Required final evidence:

```text
Linux desktop cookie/full CTest: success
Windows desktop cookie/full CTest: success
macOS desktop cookie/full CTest: success
Android arm64 client compile: success
clean tracked source: success
```

- [ ] **Step 2: Map every spec area to code/tests**

Audit:

```text
public ABI
bounded defaults/configuration
blocking-vs-async ownership
Domain/host-only/PSL
Path/default-path
Max-Age/Expires clock separation and year boundary
Secure/insecure overwrite
HttpOnly
SameSite retention
__Secure-/__Host-
identity/replacement/deletion
LRU eviction
serialization/order/overflow
explicit Cookie override
H1 repeated Set-Cookie ingestion/replay
H2 repeated Set-Cookie ingestion/replay/concurrency
blocking recovery persistence
```

- [ ] **Step 3: Scan for forbidden shortcuts**

Search the final diff for `TODO`, `FIXME`, `HACK`, comma splitting of Set-Cookie, connection-owned cookie state, duplicated PSL data, extensions to existing public config structs, and CMake install-verify production logic. Resolve every relevant hit before review.

- [ ] **Step 4: Request formal code review**

Review against both:

```text
docs/superpowers/specs/2026-09-17-client-cookie-jar-design.md
docs/superpowers/plans/2026-09-17-client-cookie-jar.md
```

Fix every Critical/Important finding and rerun focused tests plus full exact-head CI.

- [ ] **Step 5: Prepare a PR only after all gates are green**

The PR summary must separately state implemented behavior, generic-client SameSite boundary, ABI preservation, private libpsl dependency, focused tests, full CTest, and platform evidence. Do not merge on focused tests alone.
