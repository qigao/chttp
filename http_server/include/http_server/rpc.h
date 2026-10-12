#ifndef HTTP_SERVER_RPC_H
#define HTTP_SERVER_RPC_H

#include <http_common/rpc.h>
#include <http_server/http.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/**
 * Optional JSON-RPC registry attached to an existing chttp_server.
 * Configuration is pre-start, bounded, and shares the HTTP listener, routes,
 * middleware, fixed connection Owners and stop/destroy lifecycle. No second
 * transport runtime is created. Generated-plan mounting belongs to
 * <http_server/rpc_service.h> on the same HTTP Server owner.
 */
typedef struct chttp_server_rpc_config {
  size_t size;
  size_t method_capacity;
  size_t max_method_bytes;
  size_t max_json_depth;
  size_t max_batch_items;
  /** Per-invocation generated DataBind runtime budgets (no separate service owner). */
  size_t max_output_value_bytes;
  size_t max_call_frame_bytes;
  size_t native_workspace_bytes;
  size_t native_max_depth;
  size_t native_max_items;
  size_t native_max_owned_bytes;
  size_t max_inflight_calls;
} chttp_server_rpc_config;

#define CHTTP_SERVER_RPC_CONFIG_INIT \
  { sizeof(chttp_server_rpc_config), 8u, 64u, 8u, 4u, \
    4096u, 65536u, 16384u, 16u, 256u, 65536u, 4u }

/** Allocate a stopped, host-owned registry before any RPC method is registered. */
int chttp_server_rpc_configure(chttp_server *server,
                              const chttp_server_rpc_config *config);

/** Register one fixed-path JSON-RPC method on the existing HTTP server. */
int chttp_server_rpc_register(chttp_server *server, const char *target,
                              const crpc_method *method,
                              crpc_server_method_fn handler, void *user);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_SERVER_RPC_H */
