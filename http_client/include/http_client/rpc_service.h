#ifndef HTTP_CLIENT_RPC_SERVICE_H
#define HTTP_CLIENT_RPC_SERVICE_H

#include <http_client/rpc.h>
#include <data_bind_method_plan.h>
#include <data_bind_native.h>
#include <data_bind_native_binding.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum chttp_client_rpc_outcome_kind {
  CHTTP_CLIENT_RPC_NONE = 0,
  CHTTP_CLIENT_RPC_SUCCESS = 1,
  CHTTP_CLIENT_RPC_TYPED_ERROR = 2,
  CHTTP_CLIENT_RPC_REMOTE_ERROR = 3
} chttp_client_rpc_outcome_kind;

typedef struct chttp_client_rpc_outcome {
  size_t size;
  chttp_client_rpc_outcome_kind kind;
  int64_t remote_code;
  size_t typed_error_index;
  const char *typed_error;
} chttp_client_rpc_outcome;

#define CHTTP_CLIENT_RPC_OUTCOME_INIT \
  { sizeof(chttp_client_rpc_outcome), \
    CHTTP_CLIENT_RPC_NONE, 0, SIZE_MAX, NULL }

/**
 * One blocking typed RPC call over an already initialized HTTP client.
 *
 * method_plan/native_binding are immutable generated producer artifacts.
 * native_options supplies caller-owned bounded workspace for CMeta-native
 * encode/decode. request is borrowed for the call. response and typed_error are
 * caller-owned native storage.
 *
 * The adapter does not interpret DataBind schema/IDL. It uses only compiled
 * BindingPlan entries and canonical CMeta descriptors:
 *
 *   native request -> plan-driven params -> CRPC -> plan-driven native result
 *
 * The current slice mirrors the mounted RpcService server capability:
 * scalar params, at most one scalar result, and scalar typed-error payloads.
 * Unsupported mixed array/object selectors or structured values fail closed.
 */
typedef struct chttp_client_rpc_call_options {
  size_t size;
  chttp_client_rpc_limits limits;
  const char *connection_uri;
  const char *authority;
  const char *target;
  uint64_t request_id;
  const crpc_metadata *metadata;
  size_t metadata_count;
  uint32_t deadline_ms;
  const chttp_tls_profile *tls;
  chttp_protocol protocol;

  const DataBindRpcMethodPlan *method_plan;
  const DataBindServiceNativeBinding *native_binding;
  const DataBindNativeOptions *native_options;

  const void *request;
  size_t request_bytes;
  void *response;
  size_t response_bytes;
  void *typed_error;
  size_t typed_error_bytes;
} chttp_client_rpc_call_options;

#define CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT \
  { sizeof(chttp_client_rpc_call_options), \
    CHTTP_CLIENT_RPC_LIMITS_INIT, NULL, NULL, NULL, 0u, NULL, 0u, 0u, NULL, CHTTP_HTTP_1_1, \
    NULL, NULL, NULL, NULL, 0u, NULL, 0u, NULL, 0u }

/**
 * Execute one generated typed RPC call without handwritten params/result
 * encoders. SALTS_OK means a valid JSON-RPC response was mapped to outcome.
 * Transport/envelope/native decode failures return an error and fill out_error.
 */
int chttp_client_rpc_call(
    chttp_client *client,
    const chttp_client_rpc_call_options *options,
    chttp_client_rpc_outcome *outcome,
    crpc_error *out_error);


#ifdef __cplusplus
}
#endif
#endif /* HTTP_CLIENT_RPC_SERVICE_H */
