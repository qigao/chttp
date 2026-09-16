# CHTTP Client Cookie Jar Design

Date: 2026-09-17
Status: committed design gate
Base: `master` at `d2643228b458ee6438dc856261f7434143c8eccf`

## 1. Context

`qigao/chttp` currently transports arbitrary request/response headers, so callers can manually send `Cookie` and inspect `Set-Cookie`. The server also has a cookie-backed bounded session store. The client does not currently own cookie state: it has no Set-Cookie ingestion, domain/path matching, expiry handling, secure-cookie filtering, or automatic Cookie replay.

The requirement is that the client support HTTP cookies as a first-class capability rather than as manually managed header strings. The cookie implementation must preserve the repository's existing constraints:

- bounded memory and explicit capacity limits;
- one owner for mutable client state;
- HTTP/1.1 and HTTP/2 sharing one semantic implementation;
- fail-fast request admission when a request cannot be serialized safely;
- no silent truncation;
- no public ABI break in existing structs;
- no duplicate implementation of protocol parsing or public-suffix data when a maintained dependency is available.

## 2. Protocol baseline

The implementation targets the user-agent algorithms of RFC 6265 together with the current HTTP Working Group successor, `draft-ietf-httpbis-rfc6265bis-22`, for behavior that is applicable to a generic HTTP client.

The successor draft is still work in progress, so implementation tests must encode the intended behavior explicitly instead of assuming that a future RFC will remain byte-for-byte identical.

The following draft-22 details are normative for this design:

- one response may contain multiple independent `Set-Cookie` fields and they must not be comma-combined;
- cookie name plus value is limited to 4096 octets;
- Domain and Path attribute values longer than 1024 octets are not used as valid scope attributes;
- `Max-Age` takes precedence over `Expires`;
- secure cookies cannot be created by a non-secure origin;
- an insecure response cannot overwrite an overlapping secure cookie;
- cookie uniqueness is `(name, domain, host-only-flag, path)`;
- cookie names are case-sensitive, while `__Secure-` and `__Host-` prefix enforcement uses the draft's case-insensitive prefix test;
- implementations should reject public-suffix Domain cookies when a current public-suffix implementation is available;
- a general-use implementation should be capable of at least 50 cookies per domain and 3000 cookies in total.

Browser navigation policy is deliberately separated from generic HTTP cookie state. CHTTP has no browsing context, top-level navigation, document, script API, or first-/third-party concept. SameSite is parsed and retained, but a normal CHTTP request is treated as an HTTP retrieval with no browser client/site-for-cookies context. CHTTP therefore does not invent browser-only cross-site navigation state.

## 3. Goals

1. Automatically ingest all valid `Set-Cookie` response fields from HTTP/1.1 and HTTP/2.
2. Store cookies in one client-owned bounded jar.
3. Automatically attach matching cookies to later HTTP/1.1 and HTTP/2 requests.
4. Correctly implement host-only, Domain, Path, Secure, HttpOnly, SameSite, Max-Age, Expires, deletion, replacement, prefixes, and public-suffix rejection.
5. Preserve distinct cookies with the same name when their scope identity differs.
6. Preserve deterministic retrieval ordering.
7. Allow callers to override automatic cookies for one request with an explicit `Cookie` header.
8. Keep response success independent from malformed or rejected individual Set-Cookie fields.
9. Keep the jar bounded and deterministic under pressure.
10. Preserve current CHTTP public ABI and existing behavior for callers that do not rely on cookies.

## 4. Non-goals

The first implementation does not add:

- durable disk persistence across process/client destruction;
- JavaScript/non-HTTP cookie APIs;
- browser navigation, top-level-site, third-party-cookie, tracking-prevention, or storage-partition policy;
- a public iterator exposing HttpOnly cookie contents;
- HTTP/3;
- a second public-suffix parser or vendored PSL snapshot;
- CMake install-verification production code.

Persistent cookies still honor Expires/Max-Age for the lifetime of the client-owned jar. Durable persistence is a separate storage-policy feature and is not required for automatic HTTP cookie correctness.

## 5. Architecture

Each `chttp_client` / `chttp_async_client` owner contains exactly one cookie jar. The jar is not owned by a connection, HTTP/1 slot, HTTP/2 session, or request.

```text
chttp_client / chttp_async_client
        |
        +-- connection/session pool
        +-- request slots
        +-- cookie jar
              +-- bounded records
              +-- expiry / lazy purge
              +-- domain/path/security matcher
              +-- Set-Cookie parser
              +-- Cookie serializer
```

The same jar feeds both transports:

```text
request authority + target + connection security
        -> canonical request cookie context
        -> purge expired
        -> select matching cookies
        -> deterministic sort
        -> one generated Cookie header
        -> H1 serializer or H2 HPACK
```

Responses use the inverse path:

```text
all Set-Cookie fields
        -> parse independently
        -> validate response origin and scope
        -> delete / replace / insert
        -> same client-owned jar
```

This separation is required because cookie scope is based on host/domain/path/security, not on physical connection identity. HTTP/2 multiplexing and connection reuse make connection-owned cookie state incorrect.

## 6. Public API and ABI

### 6.1 Existing structs remain layout-compatible

`chttp_client_config` does not currently contain a `size` or version field. Appending cookie fields would make a new shared library read beyond an older caller's struct. Therefore this design does **not** change the layout of `chttp_client_config`, `chttp_options`, or `chttp_request_options`.

### 6.2 Default behavior

Automatic cookie support is enabled by default for newly initialized clients. Existing callers gain standards-compatible cookie replay without changing request code.

The implementation must not impose unbounded memory. Default limits are named library constants and meet the current general-user-agent minimums:

- total cookie capacity: at least 3000;
- per-domain retained capacity: at least 50;
- per-cookie name+value acceptance: 4096 octets as required by the target algorithm;
- Domain/Path accepted attribute value: at most 1024 octets;
- generated Cookie header: additionally bounded by CHTTP's existing request/header byte limits.

### 6.3 Adjustable policy without ABI break

Because repository policy requires capacity limits to be adjustable, add a new versioned options type rather than extending an existing struct:

```c
typedef struct chttp_cookie_jar_options {
  size_t size;
  size_t cookie_capacity;
  size_t cookies_per_domain;
  size_t max_cookie_header_bytes;
} chttp_cookie_jar_options;
```

The exact names may be adjusted to existing naming conventions during implementation, but semantics are fixed:

- `size` versions the new structure;
- zero values select documented defaults;
- a caller may explicitly set `cookie_capacity == 0` only through an explicit disable flag or dedicated API, not ambiguously through a default-valued field;
- configured limits may be stricter than browser-style minimums if the caller deliberately chooses a constrained embedded profile;
- `max_cookie_header_bytes == 0` derives from existing CHTTP request header limits.

Configuration is applied before the first admitted request. The implementation may defer allocation until first request/first cookie. Reconfiguration after request admission or after cookie state exists returns `SALTS_EBUSY` rather than silently discarding state.

Provide symmetric blocking and advanced-client entry points, for example:

```c
int chttp_client_set_cookie_jar_options(
    chttp_client *client,
    const chttp_cookie_jar_options *options);

int chttp_async_client_set_cookie_jar_options(
    chttp_async_client *client,
    const chttp_cookie_jar_options *options);
```

Also provide minimal management APIs:

```c
int chttp_client_cookies_clear(chttp_client *client);
int chttp_async_client_cookies_clear(chttp_async_client *client);

int chttp_client_cookie_count(const chttp_client *client, size_t *out_count);
int chttp_async_client_cookie_count(const chttp_async_client *client,
                                    size_t *out_count);
```

No public cookie iterator is included in this phase. This avoids prematurely freezing the internal cookie representation and avoids exposing HttpOnly state through a generic inspection API.

## 7. Explicit Cookie header override

A request that contains an explicit caller-provided `Cookie` header is a complete per-request override.

For that request only:

- the jar does not append or merge automatic cookies;
- the caller's existing header serialization rules apply unchanged;
- response Set-Cookie fields are still ingested into the jar.

This avoids ambiguous duplicate-name behavior such as an explicit `sid=user` colliding with a stored `sid=jar`.

The jar itself generates at most one Cookie header. CHTTP may receive multiple Cookie fields from callers if existing header validation permits them, but the automatic path never produces an ambiguous partial merge.

## 8. Internal cookie record

The internal representation stores at least:

```text
name
value
domain
path
expiry time
creation sequence
last-access sequence
host-only flag
secure-only flag
http-only flag
same-site mode
persistent flag
used flag
```

Cookie names remain case-sensitive.

Cookie uniqueness and replacement use exactly:

```text
(name, domain, host-only-flag, path)
```

The host-only flag is intentionally part of identity because draft-22 changes the older RFC 6265 replacement identity in this area.

Unknown cookie attributes are ignored as required for forward compatibility. Known attributes are normalized into flags/values; raw Set-Cookie strings are not retained after successful parsing.

## 9. Request cookie context

Every cookie operation derives a normalized context from the request/response already known to CHTTP:

- host: from `authority`, excluding port and normalizing DNS case;
- request path: path component of `target`, excluding query;
- secure channel: true for the established TLS request path, false for plaintext TCP;
- HTTP API: always true for normal CHTTP requests/responses.

IPv6 bracket syntax and optional authority ports must be handled without treating the port as part of the cookie domain.

The cookie algorithm is port-independent, including `__Host-` cookies.

ASCII/ACE host canonicalization must reuse existing URI/domain parsing where possible. Unicode DNS names are not accepted as raw cookie Domain strings unless they have already been converted to the ASCII-compatible form required by the target cookie algorithm.

## 10. Set-Cookie field processing

Each Set-Cookie field is processed independently. A malformed field is ignored without failing the HTTP response.

The parser must not split Set-Cookie on comma because Expires values contain commas and multiple Set-Cookie values are separate fields.

### 10.1 Name/value

- process the first `=` according to the target user-agent algorithm;
- trim the algorithm-defined surrounding whitespace;
- reject control characters as required by draft-22;
- ignore a Set-Cookie field when name+value exceeds 4096 octets;
- preserve cookie-name case;
- support the draft's nameless-cookie parsing behavior, including the prefix-mimic rejection rule.

### 10.2 Domain

If Domain is absent:

- set `host_only = true`;
- store the canonical response host.

If Domain is present:

- use the last valid Domain attribute according to the target algorithm;
- ignore a leading dot;
- lowercase/canonicalize it;
- reject non-ASCII raw Domain values;
- reject if the response host does not domain-match it;
- reject widened Domain scope for IP literals;
- set `host_only = false` only when the Domain attribute is accepted.

### 10.3 Public suffix protection

Full domain-cookie support must reject Domain attributes that resolve to public suffixes, except for the exact-host behavior permitted by the target algorithm.

Do not vendor a Public Suffix List in CHTTP. Add `libpsl` as a **private** client implementation dependency and use its built-in PSL data. The current repository vcpkg baseline already contains `libpsl` 0.21.5 and pins a Mozilla Public Suffix List snapshot.

This dependency must remain private to `CHttp::Client`; it must not be added to the public Chttp package dependency surface unless static/export mechanics demonstrably require it.

### 10.4 Path

If Path is absent, empty, invalid, or does not begin with `/`, compute the RFC default-path from the request path.

Retrieval uses RFC path-match:

- exact path, or
- cookie path is a prefix ending in `/`, or
- cookie path is a prefix and the next request-path character is `/`.

Plain prefix matching is insufficient and must not be used.

### 10.5 Max-Age and Expires

- the last valid Max-Age controls expiry when present;
- Max-Age takes precedence over Expires;
- non-positive Max-Age removes the matching stored cookie;
- valid Expires creates a persistent cookie when no controlling Max-Age exists;
- unparseable Expires does not invalidate the cookie; it becomes a session cookie unless another valid persistence attribute controls it;
- arithmetic must saturate rather than overflow;
- expired records are removed lazily before insert/retrieval and may also be purged during maintenance operations.

Reuse `Salts::DateTimeParser` when it can express the cookie-date compatibility grammar. If it cannot cover the required historical HTTP-date forms, add a cookie-local compatibility parser rather than changing unrelated public date parsing behavior.

### 10.6 Secure and secure-cookie integrity

A Secure cookie is accepted only from a secure response context.

Additionally, an insecure response must not overwrite or shadow an existing secure cookie when the target algorithm says the existing secure cookie overlaps by name/domain/path rules. This is required to protect secure-cookie integrity against active plaintext origins.

A Secure cookie is only retrieved for secure requests.

### 10.7 HttpOnly

HttpOnly is stored and transmitted normally because CHTTP's jar is an HTTP API. No non-HTTP script API is exposed.

### 10.8 SameSite

Store one of:

```text
unspecified/default
lax
strict
none
```

Unknown SameSite values use the target algorithm's default enforcement value.

`SameSite=None` must satisfy the Secure requirement of the current target behavior.

CHTTP does not fabricate browser navigation context. Normal CHTTP retrieval is treated as an HTTP retrieval with no browsing client/site-for-cookies context, so SameSite metadata is preserved without introducing a fake top-level-site API in this feature.

### 10.9 Cookie name prefixes

Enforce `__Secure-` and `__Host-` requirements using the target user-agent algorithm.

Important draft-22 detail: prefix requirement detection is case-insensitive even though cookie names themselves are case-sensitive.

`__Secure-` requires:

- secure response origin;
- Secure attribute.

`__Host-` requires:

- secure response origin;
- Secure attribute;
- host-only scope (no accepted Domain attribute);
- an explicit Path attribute whose effective value is exactly `/`.

Nameless cookies whose value begins with a protected prefix are rejected as required by the draft.

## 11. Replacement and deletion

Before inserting a new cookie:

1. purge expired records;
2. compute canonical cookie identity `(name, domain, host-only, path)`;
3. if a record with that identity exists, preserve the old creation time/sequence where the protocol requires it, replace the remaining fields, and refresh access metadata;
4. if the new cookie is already expired, remove the matching old record instead of inserting a new record;
5. enforce secure-cookie overwrite protection before modifying state.

A deletion Set-Cookie only deletes the exact cookie identity selected by the protocol; it does not delete same-name cookies at other paths/domains/host-only scope.

## 12. Bounded storage and eviction

Storage is preallocated or lazily allocated to fixed configured bounds. No request or response may cause unbounded cookie allocation.

When inserting and capacity is exhausted:

1. purge expired cookies;
2. enforce per-domain capacity;
3. if still over a bound, evict deterministically by least-recent access;
4. break ties by older creation sequence, then stable slot order.

Eviction is not an HTTP response error. User agents are allowed to evict cookies, and failing an otherwise valid response because its cookie could not be retained would couple application success to cache-like state.

The implementation must maintain at least the configured minimum per-domain and total capability. A deliberately smaller embedded configuration is explicit caller policy rather than silent library degradation.

## 13. Cookie retrieval and serialization

Before serializing a request:

1. if caller supplied an explicit Cookie field, skip automatic retrieval;
2. derive canonical host/path/secure context;
3. purge expired records;
4. select records that satisfy domain/host-only matching;
5. require RFC path-match;
6. exclude Secure cookies on plaintext requests;
7. apply HTTP-only semantics (all normal CHTTP requests qualify);
8. apply the generic-client SameSite policy described above;
9. update last-access sequence for selected records;
10. sort selected cookies by longer path first, then earlier creation sequence;
11. serialize one `Cookie` field using `name=value; name2=value2`.

The implementation must not truncate a Cookie field to fit a request.

If the generated field would exceed either the cookie policy bound or CHTTP's existing request/header serialization bound, request admission returns `SALTS_EMSGSIZE` before bytes are sent.

This fail-fast behavior prevents silent loss of authentication/session cookies.

## 14. HTTP/1.1 integration

H1 request construction calls the jar before final header serialization.

H1 response completion passes every stored `Set-Cookie` field to the jar before delivering the terminal completion callback/owning response to the caller. Therefore a subsequent request admitted after completion observes the newly stored cookies.

The owning response still exposes the original Set-Cookie fields unchanged.

Cookie processing does not change connection keep-alive decisions.

## 15. HTTP/2 integration

H2 uses the same jar functions and data store as H1.

Response Set-Cookie fields are applied on the CHTTP owner thread in deterministic completion/header-processing order. Concurrent H2 streams never mutate the jar from multiple threads.

Automatic Cookie generation occurs at stream admission/header construction. Each admitted stream receives the cookie snapshot selected at that point; later Set-Cookie responses do not retroactively alter headers already submitted on another stream.

The implementation may emit one Cookie field even though HTTP/2 permits splitting Cookie fields for compression. Splitting is an optimization, not required for semantic correctness.

## 16. Response header representation requirement

The existing response parser must preserve multiple Set-Cookie header fields independently. If any current code path coalesces repeated header names or only exposes the first Set-Cookie, that behavior must be corrected for Set-Cookie before jar integration.

General `chttp_response_header()` / `chttp_response_view_header()` may continue to return the first matching header for convenience. Cookie ingestion must iterate the underlying complete header array, not call the first-header helper.

## 17. Error semantics

### Response-side cookie errors

The following do **not** fail an HTTP response:

- malformed Set-Cookie;
- invalid Domain scope;
- public-suffix rejection;
- insecure Secure cookie;
- invalid prefix contract;
- oversized cookie;
- jar eviction;
- unparseable Expires.

These affect only cookie state.

### Request-side cookie errors

Request admission fails before transport when:

- canonical request cookie context cannot be derived from an otherwise accepted authority/target invariant;
- generated Cookie serialization exceeds configured/existing header bounds;
- cookie jar internal invariants are violated;
- required allocation during lazy initialization fails.

Use existing Salts/CHTTP errors (`SALTS_EINVAL`, `SALTS_EMSGSIZE`, `SALTS_ENOMEM`, `SALTS_EBUSY`, etc.) rather than introducing ad-hoc negative codes.

## 18. Source layout

Keep cookie protocol logic out of the already large client state-machine file:

```text
http_client/src/
  chttp_cookie_jar.c
  chttp_cookie_jar.h
```

The module owns:

- cookie record storage;
- Set-Cookie parsing;
- domain/default-path/path matching;
- PSL checks;
- expiry/deletion/replacement;
- eviction;
- retrieval ordering;
- Cookie serialization.

`chttp_client.c`, H1 request serialization, and H2 header construction should only provide context and call this module.

Public API declarations remain under `http_client/include/http_client/http.h`.

## 19. Dependency impact

Add `libpsl` to `vcpkg.json` and link it privately from `chttp_client`.

The current pinned vcpkg baseline provides libpsl 0.21.5. The port itself pins a Mozilla PSL snapshot and selects libidn2 on non-Windows or ICU on Windows. Implementation planning must verify Linux, macOS, Windows, and Android compatibility before production merge.

No duplicate PSL data is committed to this repository.

Salts DateTimeParser remains the preferred date dependency when compatible with cookie-date parsing.

## 20. Testing strategy

### 20.1 RED gate

Before implementation, add tests that fail against current master because no client jar exists.

A valid RED must exercise user-visible cookie behavior, not only inspect source text.

### 20.2 Unit tests: parser/store

Cover at least:

- basic Set-Cookie;
- empty name/value and draft nameless rules;
- control characters;
- 4096 name+value boundary;
- host-only Domain absence;
- accepted Domain widening to parent domain;
- rejected unrelated Domain;
- IP literal Domain behavior;
- public suffix rejection;
- leading-dot normalization;
- default Path calculation;
- exact and directory path-match boundaries;
- Max-Age precedence;
- Max-Age zero/negative deletion;
- Expires compatibility formats;
- unparseable Expires -> session cookie;
- Secure creation/retrieval;
- insecure overwrite protection for existing Secure cookie;
- HttpOnly storage/retrieval;
- SameSite values and default;
- SameSite=None Secure requirement;
- `__Secure-` prefix, including mixed-case prefix recognition;
- `__Host-` prefix, including explicit Path requirement;
- nameless prefix-mimic rejection;
- unknown attributes ignored;
- same-name cookies at different paths;
- same name/domain/path with different host-only flags;
- exact replacement identity;
- deterministic expiry purge;
- deterministic LRU eviction;
- per-domain capacity pressure;
- total capacity pressure.

### 20.3 Request synthesis tests

Cover:

- no match -> no automatic Cookie field;
- one cookie;
- many cookies;
- domain matching;
- path matching;
- longer path sorts first;
- creation-order tie break;
- expired cookie omitted;
- Secure omitted on plaintext;
- explicit Cookie header completely overrides jar for one request;
- later response Set-Cookie still updates jar after an overridden request;
- generated field overflow -> `SALTS_EMSGSIZE` and zero request bytes sent.

### 20.4 H1 integration

Use a deterministic test server flow:

1. response returns multiple Set-Cookie fields;
2. next request automatically includes all matching cookies;
3. path-specific and secure constraints are observed;
4. update/deletion responses affect later requests;
5. caller still sees original Set-Cookie response headers.

### 20.5 H2 integration

Cover:

- Set-Cookie on one stream affects a subsequently admitted stream;
- concurrent already-admitted streams keep their original header snapshot;
- multiple Set-Cookie fields survive HPACK/header representation;
- H1 and H2 requests issued through the same client owner observe the same jar where the existing client architecture permits both protocol modes under that owner;
- H2 retrieval ordering matches H1 exactly.

### 20.6 ABI/header regression

Compile existing-style callers using the unchanged `chttp_client_config` layout.

Add C and C++ header compile tests for the new versioned cookie options and management APIs.

### 20.7 Full regression

Run the complete CHTTP build and full CTest suite after cookie-specific tests pass.

WebSocket behavior, JWT, S3, file streaming, request cancellation, connection reuse, and existing explicit header behavior must remain green.

## 21. CI acceptance gate

The production change is not ready to merge until an exact-head run demonstrates all of:

1. cookie RED evidence existed before implementation;
2. cookie parser/store unit tests pass;
3. H1 cookie integration passes;
4. H2 cookie integration passes;
5. C/C++ public header compile tests pass;
6. full CHTTP build graph passes;
7. full CTest passes;
8. supported-platform dependency/build coverage required by the repository passes, including libpsl on the active platform matrix;
9. exact-head / clean tracked source check passes.

Do not add CMake install-verification production code. Verification belongs in CI/workflow logic.

## 22. Compatibility and migration

### Source compatibility

Existing callers compile without modifying existing config struct initializers because their layouts do not change.

### Binary compatibility

Existing public struct layouts are unchanged. New exported functions and a new versioned struct are additive.

### Behavioral compatibility

A client that receives Set-Cookie and later talks to a matching origin will now automatically send Cookie unless the request explicitly supplies a Cookie field or automatic cookie handling is explicitly disabled through the new cookie policy API.

This is an intentional feature behavior change required by the client-cookie requirement.

### Dependency compatibility

libpsl is private implementation detail. The implementation plan must validate CMake target naming/export behavior and platform availability before production code is merged.

## 23. Security properties

The design specifically prevents:

- Domain cookies escaping their valid origin scope;
- public-suffix cookies such as broad registry-level cookies;
- plaintext creation of Secure cookies;
- plaintext overwrite/shadowing of existing Secure cookies;
- incorrect `__Secure-` / `__Host-` acceptance;
- silent cookie truncation;
- mixing caller-explicit and automatic same-name cookies;
- connection reuse accidentally widening/narrowing cookie scope;
- H1/H2 maintaining divergent cookie truth sources.

Path is treated as a routing scope, not as a security boundary.

## 24. Performance and memory

Cookie operations are bounded by configured cookie capacity. Initial implementation may use a flat preallocated record array because the protocol minimum is only thousands of records and deterministic bounded scans are simpler to audit than a multi-index mutable structure.

Before implementation, the plan should benchmark/estimate worst-case request selection cost at the default 3000-record capacity. If a flat scan is acceptable relative to network/request costs, prefer it. Introduce indexing only if measured evidence requires it.

No per-request unbounded allocation is permitted. Cookie header serialization may use a bounded request-owned buffer sized from the configured/header limit.

## 25. State ownership and failure state

The cookie jar is the sole fact source for automatic client cookie state.

- response processing commits accepted cookie mutations on the client owner thread;
- request serialization reads from the same store;
- no connection/session maintains a shadow cookie cache;
- clear/configuration operations cannot race because public client ownership rules already prohibit concurrent mutation;
- response cookie rejection leaves the previous valid store unchanged except where the protocol explicitly defines deletion/replacement;
- request serialization failure sends no partial request.

## 26. Implementation gate

This document is the committed design gate only.

After review approval, the next step is to write a separate implementation plan. Production code must not begin before that plan is committed/reviewed under the repository workflow.
