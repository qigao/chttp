# CHTTP HTTP/2 Deferred Response Design

## Decision

`chttp_server_response_defer()` uses one generation-checked deferred control block per admitted
request slot. HTTP/1.1 owns one block per connection; HTTP/2 owns one block per fixed-capacity
stream slot. A worker may only claim the control block and copy a bounded terminal response. The
server owner thread remains the only code allowed to mutate the HTTP/2 protocol engine.

This extends the existing public handle and completion functions without introducing a protocol
fallback, an unbounded queue, or a second HTTP/2 execution owner.

## Context and alternatives

The current HTTP/1.1 implementation stores its generation token on the connection and stages a
worker response in a separate builder. HTTP/2 already preallocates stable stream slots, but its
request builder is owned by the server callback and its protocol state is owner-thread-only.

Alternatives considered:

- A server-wide deferred command queue adds a second capacity and copies response data twice.
- Letting workers call the HTTP/2 engine violates its single-owner contract.
- Retaining callback request or response views creates dangling pointers after dispatch.
- Allocating a detached stream object for every defer weakens the fixed-capacity memory model.

The per-slot control block reuses the existing bounds and stable storage while keeping protocol
mutation serialized.

## Data and ownership protocol

The unit of ownership transfer is one `chttp_server_deferred` handle. Its private target is a
control block with:

- an atomic `(generation, state)` token;
- immutable pointers to the server, connection, and request state;
- a protocol kind used by the owner-side H1/H2 terminal path;
- one independent response builder used to copy headers and body before reply succeeds;
- a scalar request snapshot with every callback-borrowed pointer cleared.

The callback is the producer of `PENDING`. A worker claims `PENDING -> WRITING`, copies the
response within `max_response_header_count`, `max_response_header_bytes`, and
`max_buffered_response_body_bytes`, then publishes `READY` with release ordering. The server owner
observes `READY` with acquire ordering, finalizes request state, submits the response on the original
stream, and retires the token. Cancellation publishes `CANCELED`; only the owner emits
`RST_STREAM(CANCEL)`.

The public handle does not keep a connection or stream alive. A copied stale handle is rejected by
the generation token. The fixed stream slot is not reusable while a worker owns `WRITING`.

## State transitions

```text
IDLE --defer/callback--> PENDING --reply/cancel claim--> WRITING
WRITING --copy succeeds--> READY --owner submit/discard--> IDLE
WRITING --copy fails--> PENDING
WRITING --cancel succeeds--> CANCELED --owner RST/discard--> IDLE
PENDING/READY/CANCELED --RST or peer close--> IDLE
WRITING --RST or peer close--> WRITING(detached) --worker publish--> IDLE
```

Exactly one reply or cancel can claim a generation. A failed reply restores `PENDING`, so the
application can explicitly retry or cancel. There is no automatic retry or downgrade.

## HTTP/2 terminal behavior

- `RST_STREAM` immediately retires that stream's pending handle. If a worker already owns
  `WRITING`, the stable stream slot is quarantined until the worker publishes a terminal state.
- Graceful server stop sends GOAWAY and continues progressing admitted deferred streams whose IDs
  are within the advertised last-stream ID. Stop completes only after their handles terminate.
- Peer connection close retires pending handles and discards ready responses. A concurrent writer
  finishes against quarantined storage before that slot can be reused.
- Cancellation emits `RST_STREAM(CANCEL)` for H2 and closes the exclusive connection for H1.
- A response-submission or application failure affects only its stream. Sibling streams and HPACK state
  stay on the existing connection.

These choices follow RFC 9113: streams are independently multiplexed, RST_STREAM fully terminates
one stream, and GOAWAY permits established streams to finish during graceful shutdown.

## Compatibility and migration

The public C struct size and function signatures remain unchanged. Documentation changes the
handle description from HTTP/1.1-only to HTTP/1.1-or-HTTP/2. Existing H1 return codes and
exactly-once behavior remain intact. HTTP/2 callers that previously received `SALTS_ENOTSUP` can
now defer regular requests; deferred streaming bodies and WebSocket handshakes remain unsupported.

Memory grows by one bounded response builder per configured H2 stream slot. Dynamic response body
storage remains governed by the existing server-wide buffer budget and per-response limit.

## Verification and rollback

Verification covers h2c and TLS ALPN `h2`, cross-thread success, copy-capacity failure and retry,
duplicate/stale handles, RST_STREAM isolation, GOAWAY stop/drain, H1 regression, public C++ header
compilation, ASan Debug, Release, and installed-package consumers.

Rollback is removal of H2 control-block initialization/progress while retaining the refactored H1
control block. No persisted data or wire format is migrated.
