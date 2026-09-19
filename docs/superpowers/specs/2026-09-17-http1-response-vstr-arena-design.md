# HTTP/1 Response Parser vstr + Arena Design

Date: 2026-09-17

Status: committed design gate; implementation not started

Branch: `perf/http1-response-vstr-arena`

Stack base: `perf/vstr-hot-paths` at `536636b0aaf98b4e97e1c325ab6e5b293e2475b4` (qigao/chttp#2)

## 1. Purpose

This change starts the second phase of the CHTTP string/memory migration. PR #2 moves selected request and WebSocket handshake hot paths to Salts `vstr`, but the HTTP/1 response parser still owns four independent heap allocations and stores parsed text only as NUL-terminated C strings.

The next PR will convert the HTTP/1 response parser to a single bounded parser-owned arena and use length-bearing `vstr` views internally while preserving the existing public C API and callback lifetime semantics.

The goal is not to make all CHTTP code use `vstr` or to introduce a repository-wide allocator in one change. The goal is to establish one correct, bounded ownership model that later server/H2 migrations can reuse.

## 2. Current State

`chttp_response_parser_init_with_sink()` currently allocates independent objects for:

- `chttp_header[max_header_count]`;
- header text storage;
- reason-phrase storage;
- buffered response body storage when no body sink is configured.

The parser appends fragmented llhttp callbacks into mutable character buffers, writes NUL terminators at field/value/status completion, and publishes `const char *` pointers into those buffers through `chttp_response_view`.

This design is bounded by `chttp_limits`, but it has four ownership roots, four allocation-failure points, repeated teardown calls, and no length-bearing representation after field/value completion.

A file sink is currently attached to `parser->file_sink_transfer` after `chttp_response_parser_init_with_sink()` returns. Therefore the initializer cannot currently know that buffered body storage is unnecessary for a file-sink request. This ordering is changed internally by this design so arena sizing is based on the real body-storage mode before allocation.

## 3. Scope

### 3.1 In scope

This PR will change only the HTTP/1 client response parser and its direct tests/support code:

- `http_client/src/chttp_internal.h`;
- `http_client/src/chttp_response.c`;
- the narrow HTTP/1 client call site that constructs the response parser;
- `http_client/src/chttp_request.c` only if the response-header lookup helper must share a small `vstr` utility;
- `http_client/tests/chttp_response_test.c`;
- narrowly related test/CI wiring when required.

The parser will:

1. allocate one bounded backing arena during initialization;
2. carve all parser-owned fixed regions from that allocation;
3. represent completed status/header slices internally as `vstr` views;
4. maintain NUL compatibility for the existing public `const char *` response API;
5. reuse the same arena across informational responses and parser message resets;
6. continue to stream bodies directly to body/file sinks when a sink is configured;
7. know the body/file sink mode before arena sizing so unused buffered-body capacity is not reserved.

### 3.2 Explicit non-goals

This PR will not change:

- `chttp_header` public ABI;
- `chttp_response_view` public ABI;
- request submission/callback signatures;
- HTTP server parsing;
- HTTP/2 frame/protocol code;
- HPACK static/dynamic tables;
- WebSocket session pool allocation;
- RPC or S3 memory models;
- TLS/network connection pooling;
- Salts allocator APIs;
- public exposure of `vstr`.

A generic CHTTP arena/slab library is also out of scope. This PR introduces a parser-local bounded arena layout, not a new public allocator subsystem.

## 4. Design Principles

### 4.1 One owner, one release

All memory that belongs to the buffered HTTP/1 response parser is owned by one backing allocation. Parser destruction performs one arena release and then clears the parser object.

### 4.2 Bounds remain authoritative

`chttp_limits` remains the source of all capacity decisions. The migration must not turn a bounded parser into a growing arena. There is no `realloc()` during parsing.

### 4.3 `vstr` carries lengths; public C strings remain compatibility views

Internal code must not rediscover lengths with `strlen()` for parser-owned status/header text. Completed slices are represented as `vstr {data,len}` views. NUL bytes are compatibility terminators written immediately after the bounded slice so the existing `chttp_header` and reason fields remain valid.

### 4.4 Streaming remains streaming

When `chttp_body_sink` or the file sink is active, response body bytes must not be copied into the parser arena. The arena reserves body storage only for the existing buffered-body mode.

### 4.5 No semantic broadening

HTTP version checks, informational-response limits, upgrade rejection, body limits, llhttp pause behavior, failure stages, and callback-scoped response lifetime remain unchanged unless a test demonstrates an existing bug that must be fixed separately.

## 5. Arena Layout

Initialization computes the full required capacity with checked arithmetic before allocating anything.

The single allocation is partitioned in deterministic order:

1. public header array: `max_header_count * sizeof(chttp_header)`;
2. header text region: existing bounded storage requirement (`max_header_bytes + 2 * max_header_count + 1` bytes);
3. reason phrase region: `max_reason_bytes + 1` bytes;
4. buffered body region: `max_response_body_bytes` bytes only when neither a body sink nor a file sink is active.

Alignment is applied before regions that store typed objects. Byte regions do not require additional alignment beyond their starting offset.

The implementation must use checked add/multiply/align helpers. Any size overflow returns `SALTS_ERANGE` before allocation.

The arena is allocated once with `malloc(total_capacity)` (or equivalent single allocation consistent with repository conventions). It is not grown after initialization.

## 6. Parser Initialization Contract

The existing public API remains unchanged, but the internal response-parser initializer must receive enough information to choose buffered versus streaming body storage before the arena is allocated.

The design permits either of these equivalent internal forms:

```c
int chttp_response_parser_init_storage(
    chttp_response_parser *parser,
    chttp_method method,
    const chttp_limits *limits,
    const chttp_body_sink *body_sink,
    chttp_file_sink_transfer *file_sink_transfer);
```

or an internal storage-mode/config object carrying the same information.

Requirements are fixed regardless of exact function name:

- plain buffered response: no body sink and no file sink -> reserve arena body region;
- callback body sink: body sink present -> no arena body region;
- file sink: file sink transfer present -> no arena body region and attach it before parse begins;
- both body sink and file sink present -> reject as `SALTS_EINVAL` if that combination is not already prohibited by the caller contract.

`chttp_response_parser_init()` may remain as a convenience wrapper for the plain buffered mode. No public header signature changes are required.

## 7. Parser Representation

The parser object will replace independent allocation roots with arena ownership and explicit region metadata. The exact field names may vary in implementation, but the model is:

```c
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

  ...existing bounds/state...
} chttp_response_parser;
```

`current_field`, `current_value`, and `reason_view` do not own memory. They refer only to parser arena bytes and are reset whenever the corresponding message state resets.

Persistent per-header `vstr` arrays are deliberately not introduced in this PR because the current public response model only needs stable C-string pointers after parse completion. If future H1 server/H2 work needs retained length metadata, that can be added at the common internal model boundary rather than pre-allocating unused metadata here.

## 8. Header Parsing Flow

### 8.1 Fragment accumulation

llhttp may split one field or value across callbacks. The parser continues to append callback bytes into the bounded header text region.

For a new field:

- record its start offset;
- append fragments without scanning for termination;
- on field completion, create `current_field = vstr_from_buf(start, length)`;
- append exactly one NUL compatibility byte.

For the following value:

- record its start offset;
- append fragments;
- on value completion, create `current_value = vstr_from_buf(start, length)`;
- append exactly one NUL compatibility byte;
- publish one `chttp_header` whose pointers reference `current_field.data` and `current_value.data`;
- clear the transient views.

No `strlen()` is permitted on parser-owned field/value data in this path.

### 8.2 Wire-byte accounting

`header_wire_bytes` retains its existing protocol accounting semantics. Compatibility NUL terminators consume arena storage but do not count as wire bytes.

The implementation must preserve the distinction between:

- wire bound (`max_header_bytes`);
- storage bound (wire bytes plus compatibility terminators).

## 9. Status/Reason Phrase Flow

Reason fragments append into the dedicated arena region. At status completion:

- `reason_view` is created from the exact accumulated length;
- one NUL terminator is written after the view;
- `response.reason` points to `reason_view.data`.

An empty reason phrase remains valid and produces a zero-length `vstr` with a valid NUL-compatible public pointer when storage is present.

No later path recomputes reason length with `strlen()`.

## 10. Body Storage

### 10.1 Buffered mode

When neither body sink nor file sink is configured, `body_storage` is the arena's body partition. Existing body-size checks occur before copying. No `malloc()` or `realloc()` occurs while parsing.

### 10.2 Callback body sink mode

When a `chttp_body_sink` is configured, the arena contains no body partition and `response.body` remains `NULL` as today.

### 10.3 File sink mode

The file-sink transfer is supplied to the internal parser initializer before arena sizing. The arena contains no body partition, and body callbacks append directly to the existing file-sink transfer.

The file-sink transfer retains its own lifecycle and buffers. This PR does not absorb file-transfer buffers into the response parser arena.

## 11. Informational Responses and Arena Reuse

The parser already supports bounded informational responses before the final response. Message reset must reuse the same arena regions rather than allocate a new arena.

Reset behavior:

- header count becomes zero;
- header-storage cursor returns to the beginning of the header text partition;
- reason cursor/view resets;
- buffered body size resets for the next message;
- transient `vstr` views are cleared;
- capacity pointers/partitions remain unchanged.

No stale `vstr` or public header pointer may be observed after a new message begins. Existing response callback scope remains authoritative.

## 12. Initialization and Failure Atomicity

Initialization follows this order:

1. validate arguments, sink mode, and limits;
2. compute all region sizes with checked arithmetic;
3. compute aligned total arena capacity;
4. perform one allocation;
5. assign region pointers inside the allocation;
6. attach the chosen body/file sink mode;
7. initialize llhttp/settings/state;
8. publish a fully initialized parser.

If any validation or size computation fails, no allocation occurs.

If the single allocation fails, return `SALTS_ENOMEM` with the parser left destroy-safe.

There must be no partially initialized state requiring multiple ownership-specific cleanup branches.

## 13. Destruction

`chttp_response_parser_destroy()` will:

1. accept `NULL` exactly as today;
2. free only the arena ownership root;
3. zero the parser structure.

It must not individually free derived region pointers.

The file-sink transfer is not owned by the parser arena and is not destroyed here, matching current lifecycle ownership.

## 14. `vstr` Use Beyond Callback Assembly

The response-header lookup helper should use `vstr` case-insensitive comparison rather than the remaining hand-written ASCII string comparator when doing so does not change the public function signature.

The helper can form views from public NUL-terminated names at the API boundary. Parser-internal code must use already-known lengths and must not call `strlen()` on arena-owned parsed slices.

This keeps PR scope narrow while eliminating the most obvious duplicate string-comparison implementation left adjacent to the migrated parser.

## 15. Error Semantics

Existing externally visible error categories remain stable:

- invalid setup/sink combination -> `SALTS_EINVAL`;
- capacity arithmetic overflow -> `SALTS_ERANGE`;
- configured protocol/body/header bound exceeded -> `SALTS_EMSGSIZE`;
- allocation failure -> `SALTS_ENOMEM`;
- parse/protocol violation -> existing `SALTS_EPROTO`/`SALTS_ENOTSUP` behavior;
- body/file sink error -> propagated existing status.

Arena exhaustion caused by a configuration-consistent parser input must map to the same configured-bound error that the old independent buffers returned; it must not silently truncate or grow.

## 16. Test Strategy

Implementation is test-driven. Required coverage includes:

### 16.1 Existing behavior

All existing `chttp_response_test` cases must pass unchanged unless a test is strengthened to assert the same contract more precisely.

### 16.2 Fragmentation

Add cases where llhttp delivers:

- field names across multiple execute calls;
- field values across multiple execute calls;
- status reason across multiple execute calls;
- multiple headers with mixed fragmentation.

Assertions cover exact public strings and header lookup.

### 16.3 Capacity boundaries

Add exact-bound tests for:

- maximum header count;
- maximum header wire bytes;
- compatibility-terminator storage at the boundary;
- maximum reason phrase;
- maximum buffered body;
- one-byte-over failures for each relevant bound.

### 16.4 Arena ownership

Add a test-visible allocation seam only if required by existing test conventions; otherwise use sanitizer/full-suite evidence. The implementation must demonstrate there is one parser backing allocation and no `realloc()` path during parse.

Do not add a production allocator abstraction solely for testing.

### 16.5 Reuse

Exercise informational response(s) followed by a final response and verify reset/reuse does not leak stale headers/reason/body state.

### 16.6 Sink modes

Verify all three storage modes:

- buffered body reserves and uses arena body storage;
- callback body sink has no parser body partition and receives bytes directly;
- file sink has no parser body partition and receives bytes through the existing file transfer path.

## 17. Verification Gates

Before the PR can be marked ready:

1. focused HTTP response parser tests pass;
2. request/response API compatibility tests pass;
3. ASan lifecycle verification passes for parser init/execute/destroy paths;
4. full CHTTP build passes;
5. full CHTTP CTest graph passes;
6. exact-head/clean-tracked-source verification passes;
7. no unrelated H2/server/WebSocket production changes are present.

The branch must pin/use the same verified dependency lineage as its stack base unless a dependency change is independently justified and verified.

## 18. PR Stack and Integration

This branch is intentionally stacked on qigao/chttp#2 because it assumes the `vstr` dependency/hot-path groundwork introduced there.

While #2 is open, the next PR should target `perf/vstr-hot-paths` so its review diff contains only the response-parser design/implementation.

After #2 merges, rebase or retarget the new PR to `master` without changing implementation semantics, then rerun exact-head verification before merge.

## 19. Follow-on Work

This PR establishes the pattern, but it does not claim CHTTP has completed its vstr/memory-pool migration. Later PRs should proceed independently:

1. HTTP/1 server parser -> `vstr` + bounded request arena;
2. shared internal H1 header-view model if duplication remains after client/server migrations;
3. HTTP/2/HPACK header representation -> `vstr`;
4. HPACK dynamic table -> dedicated bounded table allocator/slab;
5. selected WebSocket/H2 temporary allocations -> bounded arenas where lifetime data supports it;
6. only then evaluate whether public APIs should expose length-bearing string views.

Each stage requires its own exact-head full-suite evidence.

## 20. Acceptance Statement

This design is complete when the implementation can truthfully state:

> The HTTP/1 client response parser uses one bounded parser-owned arena for all parser-buffered storage, knows its buffered/body-sink/file-sink mode before arena sizing, uses length-bearing `vstr` views internally for parsed textual slices, preserves the existing public CHTTP ABI and callback lifetime rules, performs no parse-time arena growth, and passes focused, sanitizer, and full CHTTP verification.

It must not claim that all HTTP, HTTP/2, WebSocket, RPC, S3, or CHTTP public APIs have completed the vstr/memory-pool migration.