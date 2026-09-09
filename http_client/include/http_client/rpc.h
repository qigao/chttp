#ifndef HTTP_CLIENT_RPC_H
#define HTTP_CLIENT_RPC_H

#include <http_common/rpc.h>
#include <http_client/http.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Blocking request/reply JSON-RPC 2.0 client over CHTTP. */
typedef struct crpc_client {
  void *impl;
} crpc_client;

/** Advanced caller-driven JSON-RPC client for executors and event loops. */
typedef struct crpc_async_client {
  void *impl;
} crpc_async_client;

/** Generation-checked local request handle; distinct from the JSON-RPC id. */
typedef struct crpc_request {
  uint32_t slot;
  uint32_t generation;
} crpc_request;

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

typedef void (*crpc_complete_fn)(void *user, crpc_request request,
                                 const crpc_response_view *response, const crpc_error *error);

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
 * All capacities are hard bounds. max_json_depth includes the JSON-RPC
 * envelope root; therefore it must be at least two.
 */
typedef struct crpc_client_config {
  chttp_client_config http;
  size_t request_capacity;
  size_t max_method_bytes;
  size_t max_json_depth;
} crpc_client_config;

/** Initializes an ordinary sequential request/reply client. */
int crpc_client_init(crpc_client *client, const crpc_client_config *config);

/**
 * Performs one blocking JSON-RPC request/reply and returns an owning response.
 * The caller never supplies a poller, executor, or worker thread.
 */
int crpc_request_reply(crpc_client *client, const crpc_options *options,
                       crpc_response *out_response, crpc_error *out_error);

/** Releases the JSON owner and reader retained by a request/reply response. */
void crpc_response_destroy(crpc_response *response);

/** Stops/drains the internal HTTP client and destroys the request/reply owner. */
int crpc_client_destroy(crpc_client *client, uint32_t timeout_ms);

/** Initializes one advanced caller-driven RPC owner. */
int crpc_async_client_init(crpc_async_client *client, const crpc_client_config *config);

/**
 * Encodes and copies one JSON-RPC 2.0 call into CHTTP. Success guarantees one
 * later terminal callback; immediate admission failure produces no callback.
 * `options` is borrowed until submit returns; `user` remains borrowed until
 * the exactly-once terminal callback returns. request_id must be unique among
 * active requests in the same async client.
 */
int crpc_async_client_submit(crpc_async_client *client, const crpc_options *options,
                             crpc_complete_fn on_complete, void *user, crpc_request *out_request);

/** Requests cancellation; terminal completion is delivered later. */
int crpc_async_request_cancel(crpc_async_client *client, crpc_request request);

/** Advances deadlines, CHTTP, response decoding, and user callbacks. */
int crpc_async_client_poll(crpc_async_client *client, uint32_t timeout_ms, size_t *out_completions);

/** Stops admission and drains all accepted requests through terminal callbacks. */
int crpc_async_client_stop(crpc_async_client *client, uint32_t timeout_ms);

/** Requires a completed stop; a null implementation is already destroyed. */
int crpc_async_client_destroy(crpc_async_client *client);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_RPC_H */
