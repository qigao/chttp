#ifndef CHTTP_RPC_SERVICE_SERVICE_H
#define CHTTP_RPC_SERVICE_SERVICE_H

#include <http_client/rpc.h>
#include <http_server/rpc.h>

#include <data_bind_method_plan.h>
#include <data_bind_native.h>
#include <data_bind_native_binding.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chttp_rpc_service {
  void *impl;
} chttp_rpc_service;

typedef struct chttp_rpc_service_config {
  size_t size;
  size_t method_capacity;
  size_t max_output_value_bytes;
  size_t max_call_frame_bytes;
  size_t native_workspace_bytes;
  size_t native_max_depth;
  size_t native_max_items;
  size_t native_max_owned_bytes;
} chttp_rpc_service_config;

#define CHTTP_RPC_SERVICE_CONFIG_INIT \
  { sizeof(chttp_rpc_service_config), 0u, 4096u, 65536u, 16384u, 16u, 256u, 65536u }

/**
 * One generated RPC operation mount.
 *
 * Native semantic identity is producer-owned:
 * - method_plan owns RPC wire/binding/error projection;
 * - native_binding owns request/response/error storage layout;
 * - execution owns FunctionDesc/FunctionAbi + generated exact adapter.
 *
 * Mount validates these artifacts plus the canonical generated Service
 * ownership shape: VALUE status result, IN|BORROWED request, and OUT|BORROWED
 * response/error storage. UNKNOWN or alternate ownership semantics fail closed
 * before registering the CRPC method.
 */
typedef struct chttp_rpc_service_mount_options {
  size_t size;
  /** Fixed CRPC HTTP endpoint, for example "/rpc". Borrowed until register returns. */
  const char *target;
  const DataBindRpcMethodPlan *method_plan;
  const DataBindServiceNativeBinding *native_binding;
  const DataBindNativeExecution *execution;
} chttp_rpc_service_mount_options;

#define CHTTP_RPC_SERVICE_MOUNT_OPTIONS_INIT \
  { sizeof(chttp_rpc_service_mount_options), NULL, NULL, NULL, NULL }

/**
 * Initialize bounded owner-thread execution storage for generated RPC MethodPlans.
 *
 * Current slice supports independent name/ordinal params selection and one
 * scalar/string/bytes result or typed-error data value. Structured output
 * remains fail-closed until shared producer-owned composition is qualified.
 */
int chttp_rpc_service_init(
    chttp_rpc_service *service,
    const chttp_rpc_service_config *config);

/**
 * Register one already-compiled DataBind RPC MethodPlan on CRPC.
 *
 * method_plan, native_binding, execution, and all referenced descriptors are
 * borrowed until chttp_rpc_service_destroy(). Function/ABI/layout admission is
 * performed once before CRPC method registration; the call hot path performs no
 * reflection lookup or dynamic ABI reconstruction.
 */
int chttp_rpc_service_mount(
    chttp_rpc_service *service,
    crpc_server *server,
    const chttp_rpc_service_mount_options *mount);

/**
 * Release service-owned method records and bounded staging.
 *
 * The CRPC server must be stopped and destroyed before this call.
 */
int chttp_rpc_service_destroy(chttp_rpc_service *service);

typedef enum chttp_rpc_service_client_outcome_kind {
  CHTTP_RPC_SERVICE_CLIENT_NONE = 0,
  CHTTP_RPC_SERVICE_CLIENT_SUCCESS = 1,
  CHTTP_RPC_SERVICE_CLIENT_TYPED_ERROR = 2,
  CHTTP_RPC_SERVICE_CLIENT_REMOTE_ERROR = 3
} chttp_rpc_service_client_outcome_kind;

typedef struct chttp_rpc_service_client_outcome {
  size_t size;
  chttp_rpc_service_client_outcome_kind kind;
  int64_t remote_code;
  size_t typed_error_index;
  const char *typed_error;
} chttp_rpc_service_client_outcome;

#define CHTTP_RPC_SERVICE_CLIENT_OUTCOME_INIT \
  { sizeof(chttp_rpc_service_client_outcome), \
    CHTTP_RPC_SERVICE_CLIENT_NONE, 0, SIZE_MAX, NULL }

/**
 * One blocking typed RPC call over an already initialized low-level CRPC client.
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
typedef struct chttp_rpc_service_client_call_options {
  size_t size;
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
} chttp_rpc_service_client_call_options;

#define CHTTP_RPC_SERVICE_CLIENT_CALL_OPTIONS_INIT \
  { sizeof(chttp_rpc_service_client_call_options), \
    NULL, NULL, NULL, 0u, NULL, 0u, 0u, NULL, CHTTP_HTTP_1_1, \
    NULL, NULL, NULL, NULL, 0u, NULL, 0u, NULL, 0u }

/**
 * Execute one generated typed RPC call without handwritten params/result
 * encoders. SALTS_OK means a valid JSON-RPC response was mapped to outcome.
 * Transport/envelope/native decode failures return an error and fill out_error.
 */
int chttp_rpc_service_client_call(
    crpc_client *client,
    const chttp_rpc_service_client_call_options *options,
    chttp_rpc_service_client_outcome *outcome,
    crpc_error *out_error);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_RPC_SERVICE_SERVICE_H */
