# CHTTP Deferred HTTP/1.1 Terminal Operations

## Status

Accepted for [Salts issue #224](https://github.com/qigao/salts/issues/224) and required by
[TurboFlow issue #7](https://github.com/qigao/turbo-flow/issues/7).

## Context

`chttp_server_response_defer()` transfers one admitted HTTP/1.1 request from the server callback to
an application-owned, generation-checked handle. Previously the only terminal operation was
`chttp_server_deferred_reply()`. If copying the deferred response exhausted the configured CHTTP
buffer budget, reply returned an error and restored the handle to `PENDING`. An application that
does not retry had no way to retire that request, so an unbounded server stop could wait forever.

TurboFlow deliberately uses fail-fast transport semantics: it must not retry the reply, synthesize
a response, or fall back to a synchronous path after a resource failure.

## Decision

Add `chttp_server_deferred_cancel()` as the second terminal operation for a deferred HTTP/1.1
handle.

- One atomic token containing `(generation, deferred_state)` is the connection's single source of
  truth. Generation validation and state ownership cannot be separated by slot reuse.
- Reply and cancel both claim `(generation, PENDING)` with one compare-and-exchange. Exactly one
  concurrent caller can succeed.
- Successful cancel consumes and clears the caller's handle, publishes `CANCELED`, and wakes the
  server owner.
- The owner thread aborts the admitted request, releases request and response buffers, resets the
  parser, returns the slot to `IDLE`, and closes the connection.
- Cancel sends no replacement bytes. HTTP/1.1 pipelined input on that connection is discarded
  because it cannot advance past a response that will never exist.
- A stale generation or an owner-drained handle returns `SALTS_ENOENT`. A matching generation in
  `WRITING` returns `SALTS_EALREADY`; a failed reply restores `PENDING`, while a successful terminal
  operation keeps returning `SALTS_EALREADY` until the owner drains it. Invalid handles return
  `SALTS_EINVAL`.

The public handle layout is unchanged, so this is an additive source and ABI-compatible API change.
The original HTTP/2 limitation was superseded by
[Salts issue #214](https://github.com/qigao/salts/issues/214): regular H2 requests now use the same
terminal API with one generation-checked control block per fixed stream slot. See
[`../docs/CHTTP_H2_DEFERRED_RESPONSE_DESIGN.md`](../docs/CHTTP_H2_DEFERRED_RESPONSE_DESIGN.md).

## Ownership, bounds, and shutdown

The callback-borrowed request view is still not retained. The application owns only the opaque
deferred handle and any data it copied before returning from the callback. Outstanding deferred
work remains bounded by `network.connection_capacity`; cancel allocates no response buffer.

Server stop continues to wait for admitted handles. Applications must perform exactly one
successful reply or cancel for every admitted handle before expecting a graceful stop to finish.
After a reply resource failure, cancel is the deterministic no-response termination path.

## Consequences and verification

Canceling sacrifices connection reuse and any already-pipelined requests on that HTTP/1.1
connection, but preserves bounded ownership and avoids inventing response semantics. Tests cover
normal cancellation followed by connection-slot reuse, peer disconnect, concurrent stop, forced
buffer exhaustion followed by cancellation and bounded stop, and a concurrent reply/cancel race.
C and C++ header consumers retain symbol coverage.

Rollback removes the new function and `CANCELED` state, but consumers that require fail-fast
termination after reply failure must not use such a build; there is intentionally no compatibility
fallback.
