#ifndef CRPC_CRPC_H
#define CRPC_CRPC_H

#include <chttp/chttp.h>
#include <cmeta/cmeta.h>
#include <cserde/reader.h>
#include <cserde/writer.h>
#include <salts/error_codes.h>

#include <stddef.h>
#include <stdint.h>

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

/** Background JSON-RPC server owner; callers never drive a poller. */
typedef struct crpc_server {
  void *impl;
} crpc_server;

/** Generation-checked local request handle; distinct from the JSON-RPC id. */
typedef struct crpc_request {
  uint32_t slot;
  uint32_t generation;
} crpc_request;

/**
 * RPC method identity and optional CMeta semantic metadata.
 * The wire method is `service.name` when service is non-NULL, otherwise name.
 * A supplied callable is copied and bound during submit; generators are not
 * accepted by this unary request/response client.
 */
typedef struct crpc_method {
  const char *service;
  const char *name;
  const cmeta_callable *callable;
} crpc_method;

/** HTTP-carried RPC metadata. Content-Type and Accept are owned by CRPC. */
typedef struct crpc_metadata {
  const char *name;
  const char *value;
} crpc_metadata;

/**
 * Emit exactly one JSON-RPC params value as CSerde tokens. The root must be an
 * Array or Map. CRPC owns writer finalization; the callback must not finish it.
 */
typedef cserde_status (*crpc_encode_params_fn)(void *user, cserde_writer *writer);

/** Emits exactly one JSON value. CRPC owns writer finalization. */
typedef cserde_status (*crpc_encode_value_fn)(void *user, cserde_writer *writer);

typedef enum crpc_response_kind {
  CRPC_RESPONSE_RESULT = 1,
  CRPC_RESPONSE_REMOTE_ERROR
} crpc_response_kind;

/** JSON-RPC application error whose views follow the containing response lifetime. */
typedef struct crpc_remote_error {
  int64_t code;
  cserde_slice message;
  cserde_reader *data;
} crpc_remote_error;

/**
 * A protocol-valid JSON-RPC response. The result/data reader is single-pass,
 * callback-scoped, and borrows the parsed response owner.
 */
typedef struct crpc_response_view {
  uint64_t request_id;
  unsigned int http_status;
  crpc_response_kind kind;
  const cmeta_callable *callable;
  union {
    cserde_reader *result;
    crpc_remote_error remote_error;
  } value;
} crpc_response_view;

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

/**
 * Handler-scoped request view. Every pointer and the single-pass params reader
 * become invalid when the handler returns.
 */
typedef struct crpc_server_request_view {
  const chttp_server_request_view *http;
  const char *target;
  const char *method;
  uint64_t request_id;
  int notification;
  cserde_reader *params;
  const cmeta_callable *callable;
} crpc_server_request_view;

/** Handler-scoped response completion handle. */
typedef struct crpc_server_response {
  void *impl;
} crpc_server_response;

typedef int (*crpc_server_method_fn)(void *user, const crpc_server_request_view *request,
                                     crpc_server_response *response);

/**
 * All method, JSON, HTTP, and network capacities are hard bounds. The server
 * owns one background CHTTP worker and dispatches handlers serially. CRPC
 * requires an explicit nonzero http.max_buffered_response_body_bytes large
 * enough to carry every built-in JSON-RPC protocol error.
 */
typedef struct crpc_server_config {
  chttp_server_config http;
  size_t method_capacity;
  size_t max_method_bytes;
  size_t max_json_depth;
  size_t max_batch_items;
} crpc_server_config;

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

/**
 * Initializes a stopped JSON-RPC server and its bounded method registry.
 * @return `SALTS_OK`; `SALTS_EINVAL`, `SALTS_EMSGSIZE`, or `SALTS_ERANGE`
 * for invalid bounds; `SALTS_ENOMEM` for allocation failure; otherwise the
 * underlying CHTTP initialization error.
 */
int crpc_server_init(crpc_server *server, const crpc_server_config *config);

/**
 * Returns the borrowed CHTTP owner for pre-start middleware and route setup.
 * Listener and worker lifecycle remain owned by crpc_server_*.
 */
chttp_server *crpc_server_http(crpc_server *server);

/**
 * Registers one fixed origin-form target/method pair before server start.
 * CHTTP `:segment` route patterns are rejected. Target, wire method, and bound
 * callable are copied; handler and user are borrowed through destroy.
 * @return `SALTS_OK`, validation/binding errors, `SALTS_EBUSY` after start,
 * `SALTS_EALREADY` for a duplicate key, or `SALTS_ENOBUFS` when full.
 */
int crpc_server_register(crpc_server *server, const char *target, const crpc_method *method,
                         crpc_server_method_fn handler, void *user);

/**
 * Completes one handler call with a JSON-RPC result; NULL encoder writes null.
 * The encoder runs synchronously. Notifications mark completion without bytes.
 * @return `SALTS_OK`, `SALTS_EINVAL`, `SALTS_EALREADY`, or a bounded encoder error.
 */
int crpc_server_response_result(crpc_server_response *response, crpc_encode_value_fn encode,
                                void *user);

/**
 * Completes one handler call with a JSON-RPC application error. The message
 * and optional data encoder are consumed synchronously.
 * @return `SALTS_OK`, `SALTS_EINVAL`, `SALTS_EALREADY`, or a bounded encoder error.
 */
int crpc_server_response_error(crpc_server_response *response, int64_t code, const char *message,
                               crpc_encode_value_fn encode_data, void *data_user);

/** Starts the listener and background CHTTP owner thread; propagates CHTTP errors. */
int crpc_server_start(crpc_server *server);
int crpc_server_port(const crpc_server *server, uint16_t *out_port);
/** Stops admission and drains the CHTTP owner; timeout and transport errors propagate. */
int crpc_server_stop(crpc_server *server, uint32_t timeout_ms);
/** Releases a stopped server; a zero server is already destroyed. */
int crpc_server_destroy(crpc_server *server);

#ifdef __cplusplus
}
#endif

#endif /* CRPC_CRPC_H */
