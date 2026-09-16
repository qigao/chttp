# CHTTP Client Cookie Jar Design

Date: 2026-09-17
Status: committed design gate
Base: `master` at `d2643228b458ee6438dc856261f7434143c8eccf`

## 1. Context

CHTTP currently transports arbitrary request/response headers, so callers can manually send `Cookie` and inspect `Set-Cookie`. The server also has a bounded cookie-backed Session implementation. The client does not own cookie state: it has no Set-Cookie ingestion, scope matching, expiry handling, secure-cookie protection, public-suffix protection, or automatic Cookie replay.

The requirement is a real client cookie jar shared by HTTP/1.1 and HTTP/2. It must preserve repository constraints: bounded memory, single-owner mutable state, fail-fast request admission, no silent truncation, no existing public-struct ABI break, and no duplicated protocol/public-suffix implementation when a maintained dependency exists.

## 2. Protocol baseline

The implementation targets RFC 6265 plus the user-agent algorithms applicable to a generic HTTP client in `draft-ietf-httpbis-rfc6265bis-22`.

The successor document is still work in progress, so tests must lock the intended behavior rather than assuming a later RFC will be identical. The following draft-22 behavior is part of this design:

- multiple Set-Cookie response fields are independent and must not be comma-combined;
- cookie name+value over 4096 octets is rejected;
- Domain/Path attribute values over 1024 octets are not accepted as valid scope attributes;
- Max-Age takes precedence over Expires;
- a non-secure response cannot create a Secure cookie;
- an insecure response cannot overwrite/shadow an overlapping Secure cookie;
- cookie uniqueness is `(name, domain, host-only-flag, path)`;
- cookie names are case-sensitive;
- protected prefix detection for `__Secure-` and `__Host-` follows the draft's case-insensitive requirement check;
- public-suffix Domain cookies are rejected when PSL support is available;
- default capability is at least 50 cookies per domain and 3000 total.

CHTTP is not a browser. It has no document, script API, top-level navigation, first-/third-party state, or site-for-cookies input. SameSite metadata is parsed and retained, but CHTTP does not invent browser navigation context.

## 3. Goals

1. Ingest every valid Set-Cookie field from H1 and H2 responses.
2. Store cookies in one bounded client-owned jar.
3. Automatically attach matching cookies to subsequent H1/H2 requests.
4. Support host-only, Domain, Path, Secure, HttpOnly, SameSite, Max-Age, Expires, replacement, deletion, prefixes, and public-suffix rejection.
5. Preserve same-name cookies when scope identity differs.
6. Use deterministic retrieval ordering and deterministic eviction.
7. Let an explicit request `Cookie` header override automatic cookies for that request only.
8. Never fail an otherwise valid HTTP response solely because one Set-Cookie field is malformed/rejected/evicted.
9. Never silently truncate generated Cookie headers.
10. Preserve existing ABI and ordinary behavior outside cookie automation.

## 4. Non-goals

This feature does not add:

- durable cookie persistence across process/client destruction;
- JavaScript/non-HTTP cookie APIs;
- browser third-party/tracking/navigation policy;
- storage partitioning/browser-vendor policy;
- public iteration over HttpOnly cookie values;
- HTTP/3;
- a vendored PSL snapshot;
- CMake install-verification production code.

Persistent cookies honor Expires/Max-Age for the lifetime of the client jar. Disk persistence is a separate storage-policy feature, just as many HTTP session clients keep a cookie jar only for the session object lifetime.

## 5. Ownership and architecture

Each `chttp_client` / `chttp_async_client` owner has exactly one cookie jar. The jar is not connection-owned, slot-owned, or HTTP/2-session-owned.

```text
chttp_client / chttp_async_client
        |
        +-- connection/session pool
        +-- request slots
        +-- cookie jar
              +-- bounded records
              +-- Set-Cookie parser
              +-- expiry/deletion/replacement
              +-- domain/path/security matching
              +-- PSL validation
              +-- Cookie serializer
```

Request flow:

```text
authority + target + transport security
        -> canonical cookie context
        -> purge expired
        -> select + sort matching cookies
        -> one generated Cookie field
        -> H1 serializer or H2 HPACK
```

Response flow:

```text
all Set-Cookie fields
        -> parse independently
        -> validate origin/scope/security
        -> delete / replace / insert
        -> same jar
```

This state cannot live in the connection pool because cookie scope is based on domain/path/security, while connections are reused and H2 multiplexes unrelated request streams.

## 6. Public API and ABI

### 6.1 Do not extend existing public config structs

`chttp_client_config` has no `size`/version field. Extending it would let a newer shared library read past an older caller's struct. Therefore this design does not change the layout of `chttp_client_config`, `chttp_options`, or `chttp_request_options`.

### 6.2 Default behavior

Automatic cookie handling is enabled by default for newly initialized clients.

Named defaults must provide at least:

- 3000 cookies total;
- 50 cookies per canonical cookie domain bucket;
- the protocol's 4096-octet name+value acceptance bound;
- the protocol's 1024-octet Domain/Path attribute bound;
- generated Cookie header bounded by existing CHTTP header/request limits.

### 6.3 Adjustable policy without ABI break

Add a new versioned options type:

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
```

Semantics are fixed even if exact identifier spelling changes to match repository conventions:

- absence of an explicit options call means automatic cookies enabled with defaults;
- `enabled == 0` explicitly disables automatic storage/replay;
- `enabled != 0` enables the jar;
- zero capacity/header values select documented defaults;
- callers may intentionally choose smaller embedded limits;
- `max_cookie_header_bytes == 0` derives from existing request/header limits.

Options are configurable only before the first admitted request. Allocation may be lazy. Reconfiguration after request admission or cookie state creation returns `SALTS_EBUSY`; it never silently discards state.

Provide symmetric APIs:

```c
int chttp_client_set_cookie_jar_options(
    chttp_client *client,
    const chttp_cookie_jar_options *options);

int chttp_async_client_set_cookie_jar_options(
    chttp_async_client *client,
    const chttp_cookie_jar_options *options);

int chttp_client_cookies_clear(chttp_client *client);
int chttp_async_client_cookies_clear(chttp_async_client *client);

int chttp_client_cookie_count(const chttp_client *client, size_t *out_count);
int chttp_async_client_cookie_count(const chttp_async_client *client,
                                    size_t *out_count);
```

`cookie_count` reports currently unexpired records; it may compute the count without mutating a const client. `cookies_clear` removes all jar state but does not change enable/configuration policy.

No public cookie iterator is added in this phase. That avoids freezing internal storage and avoids exposing HttpOnly state through a generic inspection API.

## 7. Explicit Cookie override

If a request supplies any explicit caller `Cookie` header, automatic Cookie generation is skipped for that request. The library does not string-merge caller and jar values.

Response Set-Cookie fields from that request are still ingested into the jar.

This makes manual per-request override deterministic and avoids duplicate-name ambiguity such as `sid=user` plus `sid=stored`.

The automatic path emits at most one Cookie field. Existing caller-provided header rules remain unchanged.

## 8. Internal record and identity

Each record stores at least:

```text
name
value
domain
path
creation sequence
last-access sequence
host-only flag
secure-only flag
http-only flag
same-site mode
persistent flag
expiry kind + expiry value
used flag
```

Cookie names remain case-sensitive.

Replacement identity is exactly:

```text
(name, domain, host-only-flag, path)
```

Unknown attributes are ignored for forward compatibility. Known attributes are normalized; raw Set-Cookie strings do not become a second truth source.

## 9. Canonical request/response cookie context

Derive cookie context from existing CHTTP request facts:

- host: `authority` host component, excluding port, canonicalized to lower-case ASCII/ACE;
- path: `target` path component, excluding query;
- secure: true only for the request's established TLS path;
- API type: HTTP.

IPv6 brackets/ports must not become part of the cookie domain. Cookie matching is port-independent, including `__Host-` cookies.

Reuse existing URI/domain parsing where possible. Raw Unicode Domain attributes are not accepted unless already in an ASCII-compatible encoding required by the protocol baseline.

## 10. Set-Cookie processing

Each Set-Cookie field is parsed independently. A rejected field does not fail the HTTP response.

Never split Set-Cookie on comma because Expires contains commas and multiple cookies are separate fields.

### 10.1 Name/value

- follow the target algorithm around the first `=` and surrounding WSP;
- enforce control-character rules;
- ignore the field when name+value exceeds 4096 octets;
- preserve cookie-name case;
- support the draft's nameless-cookie behavior and protected-prefix mimic rejection.

### 10.2 Domain

Without Domain:

- `host_only = true`;
- domain = canonical response host.

With Domain:

- use the last valid Domain attribute selected by the target algorithm;
- ignore a leading dot;
- lowercase/canonicalize;
- reject raw non-ASCII Domain values;
- reject if response host does not domain-match;
- do not widen an IP literal with Domain matching;
- set `host_only = false` only after the Domain attribute is accepted.

### 10.3 Public suffix protection

Reject Domain attributes resolving to public suffixes, except the exact-host handling permitted by the target algorithm.

Do not vendor PSL data. Add `libpsl` as a private CHTTP client implementation dependency. The repository's pinned vcpkg baseline provides libpsl 0.21.5 and a pinned Mozilla PSL snapshot.

`libpsl` must not become part of the public Chttp package dependency surface unless static/export mechanics prove that unavoidable. Platform support must be verified before production merge.

### 10.4 Path

If Path is missing, empty, invalid, or does not begin with `/`, compute the protocol default-path from the request path.

Path-match is true only for:

- exact path;
- cookie path prefix ending in `/`;
- cookie path prefix where the next request-path character is `/`.

A naive string-prefix check is incorrect.

### 10.5 Max-Age, Expires, and clocks

Max-Age and Expires use different internal clocks so wall-clock changes do not corrupt relative TTLs:

- a valid controlling `Max-Age > 0` stores a **monotonic deadline** computed from receipt monotonic time;
- `Max-Age <= 0` deletes the matching cookie;
- valid controlling `Expires` stores an **absolute UTC/wall-clock deadline**;
- Max-Age takes precedence when both are present;
- a session cookie has no expiry deadline;
- arithmetic saturates rather than wrapping.

Expiry checks evaluate the corresponding clock kind. The implementation must not convert a Max-Age cookie into a wall-clock deadline just for convenience.

Reuse `Salts::DateTimeParser` for cookie-date when it covers required legacy HTTP-date forms. Otherwise use a cookie-local compatibility parser; do not broaden unrelated public date behavior.

Unparseable Expires does not reject the cookie; without a valid controlling persistence attribute it remains a session cookie.

### 10.6 Secure and secure-cookie integrity

Secure cookies are accepted only from secure response contexts and retrieved only on secure requests.

An insecure response must not overwrite or shadow an existing overlapping Secure cookie when the target algorithm's name/domain/path conditions protect it.

### 10.7 HttpOnly

HttpOnly is stored and sent normally because CHTTP is an HTTP API. No non-HTTP access API exists.

### 10.8 SameSite

Store:

```text
unspecified/default
lax
strict
none
```

Unknown values use the target default mode. `SameSite=None` must satisfy the current Secure requirement.

CHTTP does not fabricate browser navigation context. SameSite metadata is therefore retained without adding top-level-site/navigation APIs to this feature.

### 10.9 Protected prefixes

Enforce `__Secure-` and `__Host-` using the target user-agent algorithm.

Prefix requirement detection is case-insensitive even though cookie-name identity is case-sensitive.

`__Secure-` requires a secure response and Secure attribute.

`__Host-` requires:

- secure response;
- Secure attribute;
- host-only scope/no accepted Domain;
- an explicit Path attribute whose effective path is exactly `/`.

Nameless cookies whose value begins with a protected prefix are rejected as required by the draft.

## 11. Replacement and deletion

Before insert:

1. purge expired records;
2. compute `(name, domain, host-only, path)`;
3. enforce secure-overwrite protection;
4. if an existing exact identity exists, preserve its creation sequence/time when required and replace remaining fields;
5. if the new cookie is already expired, remove the exact old identity instead of inserting.

Deletion never removes same-name cookies at other scopes.

## 12. Bounded storage and deterministic eviction

Storage is preallocated or lazily allocated to fixed configured limits. No response can cause unbounded growth.

When pressure exists:

1. purge expired;
2. enforce per-domain capacity;
3. enforce total capacity;
4. evict least-recently-accessed candidates;
5. break ties by older creation sequence, then stable slot order.

Eviction does not fail the HTTP response. A deliberately smaller configured profile is explicit caller policy.

The default profile meets the general-user-agent minimums.

## 13. Retrieval and serialization

Before request serialization:

1. explicit caller Cookie present -> skip automatic retrieval;
2. derive canonical host/path/secure context;
3. purge expired;
4. require host-only/domain match;
5. require path-match;
6. exclude Secure cookies on plaintext;
7. apply HTTP-only semantics;
8. apply the generic-client SameSite policy above;
9. update access sequence for selected records;
10. sort by longer path first, then earlier creation sequence;
11. serialize `name=value; name2=value2` into one automatic Cookie field.

Never truncate. If the generated field exceeds cookie-policy or existing request/header bounds, admission returns `SALTS_EMSGSIZE` before any request bytes are sent.

## 14. H1/H2 integration

### HTTP/1.1

Generate Cookie before final request header serialization. Ingest every Set-Cookie field before terminal completion is delivered, so a subsequently admitted request sees the updated jar. The owning response still exposes the original response headers unchanged.

### HTTP/2

Use the same jar functions/store. H2 response mutations occur on the existing CHTTP owner thread in deterministic event-processing order. Each stream gets the cookie snapshot present at its own header admission; later Set-Cookie responses do not mutate already-submitted stream headers.

One automatic Cookie field is sufficient semantically even though H2 can split Cookie fields for compression.

### Repeated response headers

Cookie ingestion must iterate the complete header array. `chttp_response_header()` and `chttp_response_view_header()` may continue returning the first matching header as convenience APIs; they are not used for Set-Cookie ingestion.

If any parser path currently coalesces repeated Set-Cookie values, that path must be corrected before cookie-jar integration.

## 15. Error semantics

Response-side cookie rejection does not fail the response for:

- malformed Set-Cookie;
- invalid Domain;
- public-suffix rejection;
- insecure Secure cookie;
- protected-prefix violation;
- oversized cookie;
- unparseable Expires;
- eviction.

Request admission fails before transport for:

- invalid cookie context derived from otherwise inconsistent authority/target state;
- generated Cookie overflow;
- lazy jar allocation failure;
- internal invariant failure.

Use existing Salts/CHTTP errors such as `SALTS_EINVAL`, `SALTS_EMSGSIZE`, `SALTS_ENOMEM`, and `SALTS_EBUSY`.

## 16. Source layout

Keep protocol logic out of the already-large client state machine:

```text
http_client/src/
  chttp_cookie_jar.c
  chttp_cookie_jar.h
```

The module owns parsing, storage, expiry, replacement, domain/path/security matching, PSL validation, eviction, ordering, and Cookie serialization.

Public declarations remain in `http_client/include/http_client/http.h`.

Expected integration touches are limited to the client owner, H1 request/response path, H2 header/completion path, tests, client CMake, and vcpkg manifest.

## 17. Dependency impact

Add `libpsl` privately to `CHttp::Client`. The pinned vcpkg port uses libidn2 on non-Windows and ICU on Windows and embeds a pinned Mozilla PSL snapshot.

Before production merge, verify dependency/build behavior on the repository's supported Linux, macOS, Windows, and Android paths. No PSL data is committed to CHTTP.

`Salts::DateTimeParser` remains the preferred date parser when its accepted grammar is sufficient.

## 18. Testing strategy

### RED gate

Before production implementation, add behavior tests that fail on current master because automatic client cookie state does not exist. A source-text-only failure is insufficient.

### Unit tests

Cover at least:

- basic and nameless Set-Cookie;
- control characters;
- 4096 boundary;
- host-only vs Domain;
- parent-domain acceptance and unrelated-domain rejection;
- IP literals;
- PSL rejection;
- leading-dot normalization;
- default Path and path-match boundaries;
- Max-Age precedence, positive TTL, zero/negative deletion;
- Expires compatibility and invalid Expires session fallback;
- monotonic Max-Age unaffected by wall-clock test jump;
- absolute Expires responding to wall-clock test time;
- Secure creation/retrieval;
- insecure overwrite protection;
- HttpOnly;
- SameSite modes and None+Secure;
- case-insensitive `__Secure-` / `__Host-` requirement detection;
- nameless prefix-mimic rejection;
- unknown attributes;
- same-name different scopes;
- host-only flag in replacement identity;
- deterministic purge and LRU eviction;
- per-domain and total pressure.

### Request synthesis

Cover:

- no match -> no Cookie;
- one/many matches;
- domain/path matching;
- longer path first;
- creation-order tie break;
- expired omitted;
- Secure omitted on plaintext;
- explicit Cookie complete override;
- Set-Cookie still ingested after an overridden request;
- generated overflow -> `SALTS_EMSGSIZE` with zero bytes sent.

### H1 integration

A deterministic server returns multiple Set-Cookie fields, verifies automatic replay, updates/deletes cookies, and verifies caller response visibility remains unchanged.

### H2 integration

Verify one stream's Set-Cookie affects a later-admitted stream, already-admitted concurrent streams retain their original headers, duplicate Set-Cookie fields survive HPACK/header storage, and H1/H2 selection uses identical jar semantics.

### ABI/header regression

Compile existing-style callers with unchanged public config layouts. Add C and C++ compile tests for the new versioned options/manage APIs.

### Full regression

After cookie-specific tests pass, run full CHTTP build + full CTest. WebSocket, JWT, S3, file streaming, cancellation, connection reuse, and explicit headers remain green.

## 19. CI acceptance gate

Production code is not ready to merge until exact-head evidence shows:

1. valid RED existed before implementation;
2. cookie unit tests pass;
3. H1 integration passes;
4. H2 integration passes;
5. C/C++ public-header tests pass;
6. full CHTTP build passes;
7. full CTest passes;
8. required platform/dependency coverage passes, including libpsl on the active matrix;
9. exact-head / clean tracked source check passes.

Do not add CMake install-verification production code. Verification remains in CI/workflow logic.

## 20. Compatibility

### Source/binary compatibility

Existing public struct layouts do not change. New functions and the new versioned options struct are additive.

### Behavioral compatibility

Existing callers that never receive Set-Cookie behave as before. A client that receives Set-Cookie and later addresses a matching scope now automatically sends Cookie unless explicitly disabled or overridden with a request Cookie header. That behavior change is intentional and is the feature requirement.

### Package compatibility

libpsl is private implementation detail unless export/link mechanics demonstrate otherwise. The implementation plan must verify the actual CMake target/export behavior rather than guessing.

## 21. Security properties

The design prevents:

- invalid Domain widening;
- public-suffix cookie leakage;
- plaintext Secure-cookie creation;
- plaintext overwrite/shadowing of protected Secure cookies;
- invalid protected-prefix cookies;
- silent cookie-header truncation;
- ambiguous caller/jar same-name merge;
- connection reuse changing cookie scope;
- divergent H1/H2 cookie truth sources.

Path is routing scope, not a security boundary.

## 22. Performance and memory

Cookie work is bounded by configured capacity. A flat preallocated record array is acceptable as the first implementation if measurement at the default 3000-record capacity shows request selection cost is negligible relative to HTTP/network cost. Do not introduce mutable multi-index complexity without evidence.

No unbounded per-request allocation is allowed. Generated Cookie serialization uses a bounded request-owned buffer derived from policy/header limits.

## 23. State and failure invariants

The jar is the sole fact source for automatic client cookie state.

- responses mutate it only on the client owner thread;
- H1/H2 do not maintain shadow jars;
- request serialization reads one consistent store;
- configuration cannot race with admitted work;
- rejected Set-Cookie leaves unrelated valid state unchanged;
- request Cookie serialization failure sends no partial request;
- clear removes records atomically from the owner perspective.

## 24. Implementation gate

This document is the committed design gate only.

After user review approval, the next step is a separate implementation plan. Production code does not begin before that plan is written and reviewed under the repository workflow.
