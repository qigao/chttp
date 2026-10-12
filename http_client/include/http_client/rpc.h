#ifndef HTTP_CLIENT_RPC_H
#define HTTP_CLIENT_RPC_H

#include <http_common/rpc.h>
#include <http_client/http.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Owning request/reply response. Result/error-data readers are single-pass and
 * remain valid until `crpc_response_destroy()`.
 */
typedef struct crpc_response {
  uint64_t request_id;
  unsigned int http_status;
  crpc_response_kind kind;
  const cmeta_callable *callable;
  union {
    cserde_reader *result;
    crpc_remote_error remote_error;
  } value;
  void *impl;
} crpc_response;

/** Transport, HTTP, deadline, decode, or envelope failure. */
typedef struct crpc_error {
  int status;
  int native_status;
  unsigned int http_status;
  const char *stage;
} crpc_error;

/**
 * Inputs for one RPC call. `connection_uri`, HTTP `authority`, and origin-form
 * `target` are independent so one client can call multiple endpoints at the
 * same site. A zero deadline disables the RPC overall deadline; CNet
 * connect/read/write deadlines remain separate.
 */
typedef struct crpc_options {
  const char *connection_uri;
  const char *authority;
  const char *target;
  crpc_method method;
  uint64_t request_id;
  const crpc_metadata *metadata;
  size_t metadata_count;
  uint32_t deadline_ms;
  crpc_encode_params_fn encode_params;
  void *params_user;
  /** Optional reusable TLS profile; valid only with a `tls://` connection URI. */
  const chttp_tls_profile *tls;
  /** Explicit wire protocol. HTTP/2 never falls back to HTTP/1.1. */
  chttp_protocol protocol;
} crpc_options;

/**
 * Explicit bounded projection limits for RPC over an existing HTTP client.
 * HTTP itself independently enforces its initialized buffer/header limits.
 * These cannot enlarge the HTTP client's capacities.
 */
typedef struct chttp_client_rpc_limits {
  size_t size;
  size_t max_method_bytes;
  size_t max_json_depth;
  size_t max_request_body_bytes;
  size_t max_http_header_count;
} chttp_client_rpc_limits;

#define CHTTP_CLIENT_RPC_LIMITS_INIT \
  { sizeof(chttp_client_rpc_limits), 64u, 8u, 8192u, 16u }

/**
 * Execute a JSON-RPC 2.0 call over the passed already-initialized chttp_client.
 * Reuses its HTTP/1 keepalive / HTTP/2 pool, TLS and single-owner busy rules.
 * The CSerde response is owning; destroy with crpc_response_destroy().
 * No additional RPC client/worker/poller, automatic retry or protocol fallback.
 */
int chttp_client_rpc_request_reply(
    chttp_client *client, const chttp_client_rpc_limits *limits,
    const crpc_options *options, crpc_response *out_response,
    crpc_error *out_error);

/** Direct asynchronous JSON-RPC over an existing CHttp async Client.
 * The returned chttp_request is the sole generation-safe cancel handle.
 * Progress, cancellation and shutdown use chttp_async_client_*.
 * No separate RPC owner/poller/pool is created.
 *
 * A nonzero deadline_ms is enforced by the existing HTTP Owner's bounded
 * request-slot scan. Expiry submits ordinary HTTP cancel; ETIMEDOUT is
 * delivered exactly once after native terminal completion, not on admission
 * to the cancellation operation. The HTTP request-slot table rejects
 * duplicate active JSON-RPC wire IDs as SALTS_EALREADY.
 * Callback response/readers are borrowed until the callback returns.
 */
typedef void (*chttp_async_rpc_complete_fn)(
    void *user, chttp_request request, const crpc_response_view *response,
    const crpc_error *error);

int chttp_async_client_rpc_submit(
    chttp_async_client *client, const chttp_client_rpc_limits *limits,
    const crpc_options *options, chttp_async_rpc_complete_fn on_complete,
    void *user, chttp_request *out_request);

/** Releases the owner of a blocking JSON-RPC response after consumption. */
void crpc_response_destroy(crpc_response *response);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_RPC_H */
