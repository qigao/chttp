#ifndef HTTP_SERVER_RPC_H
#define HTTP_SERVER_RPC_H

#include <http_common/rpc.h>
#include <http_server/http.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Background JSON-RPC server owner; callers never drive a poller. */
typedef struct crpc_server {
  void *impl;
} crpc_server;

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
  /** Internal callback-scoped selector context; never inspect or retain. */
  void *params_selection_context;
} crpc_server_request_view;

/** Handler-scoped response completion handle. */
typedef struct crpc_server_response {
  void *impl;
} crpc_server_response;

typedef int (*crpc_server_method_fn)(void *user, const crpc_server_request_view *request,
                                     crpc_server_response *response);

/**
 * Independent callback-scoped reader over one selected JSON-RPC params value.
 * The reader borrows the request DOM and must be closed before the handler
 * returns. A zero/closed lease is accepted by close().
 */
typedef struct crpc_server_param_reader {
  size_t size;
  cserde_reader *reader;
} crpc_server_param_reader;

#define CRPC_SERVER_PARAM_READER_INIT \
  { sizeof(crpc_server_param_reader), NULL }

/**
 * Open one params member/item as an independent CSerde reader.
 *
 * For object params, name must be non-empty and ordinal is ignored.
 * For array params, ordinal must not be SIZE_MAX and name is ignored.
 * Missing params/member/item returns SALTS_OK with *present == 0.
 * Duplicate object keys or malformed request-scoped state return SALTS_EPROTO.
 */
int crpc_server_request_param_open(
    const crpc_server_request_view *request,
    const char *name,
    size_t ordinal,
    crpc_server_param_reader *out_reader,
    int *present);

/** Close one reader lease; zero/closed leases are accepted. */
void crpc_server_request_param_close(crpc_server_param_reader *reader);

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

#endif /* HTTP_SERVER_RPC_H */
