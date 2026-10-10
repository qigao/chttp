#ifndef CHTTP_RPC_SERVICE_SERVICE_H
#define CHTTP_RPC_SERVICE_SERVICE_H

#include <http_client/rpc.h>
#include <http_server/rpc.h>

#include <data_bind_method_plan.h>
#include <data_bind_message_plan.h>
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
 * Supports independent name/ordinal params selection. The legacy mount returns
 * one scalar result or typed-error data value; mount_document opts into a JSON
 * object result using the producer-owned egress FormatPlan.
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

/** Opt-in JSON object result using the MethodPlan's JSON egress FormatPlan.
 * Root names follow IDL [name]. Egress wire names must be canonical; typed
 * errors and non-JSON formats fail admission. Legacy mount keeps scalar results.
 * A bounded owned CSerde token tape preserves output until the synchronous
 * JSON-RPC encoder runs. max_output_value_bytes bounds tokens plus copied
 * slices, and the server separately bounds final envelope bytes. No JSON parse
 * or intermediate JSON text is used. Consume document results through
 * client_call_document() or the low-level CRPC client/JSON reader.
 * Borrowing and server/service teardown follow mount(). Returns SALTS_OK on
 * registration, SALTS_ENOTSUP for an unsupported plan, SALTS_EINVAL for invalid
 * arguments, or the existing mount/CRPC registration error.
 */
int chttp_rpc_service_mount_document(
    chttp_rpc_service *service, crpc_server *server,
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
 * The current slice mirrors the legacy scalar RpcService mount:
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

/** Call a document-mounted operation and decode its JSON result into a C record.
 *
 * response_plan must be prepared from the same IDL contract and response native
 * binding as options->method_plan. Prepare it once with MessagePlan compile or
 * acquire_generated; the caller owns its lifetime. All plans, descriptors and
 * options are borrowed for this blocking call. Params retain the legacy scalar
 * name/ordinal rules. Non-JSON formats and typed-error operations are rejected.
 *
 * FormatPlan maps external names/aliases; MessagePlan owns required fields,
 * optional/null state, defaults, validation and bounded native materialization.
 * No JSON text reconstruction or second parse occurs. The response must be
 * fresh storage or already restored to semantic zero; never pass a live result
 * without first calling data_bind_native_clear() with its response descriptor.
 * On SUCCESS, owned strings/containers belong to the caller and survive CRPC
 * response destruction. Use the same canonical cleanup after consumption.
 *
 * Admission failure leaves response untouched. After successful initialization,
 * transport/decode failure restores semantic zero and leaves outcome NONE;
 * a valid remote error returns SALTS_OK with REMOTE_ERROR and a zero response.
 * Native workspace/item/depth/owned-byte limits remain caller supplied; HTTP
 * body/depth limits remain owned by crpc_client. Workspace and native storage
 * must be disjoint and exclusively borrowed until return.
 * Returns SALTS_EINVAL for incompatible plans/storage, SALTS_ENOTSUP for
 * unsupported format/shape, SALTS_EMSGSIZE for exhausted native limits, or the
 * existing transport/decode error. Decode errors set stage rpc-service-result.
 */
int chttp_rpc_service_client_call_document(
    crpc_client *client,
    const chttp_rpc_service_client_call_options *options,
    const DataBindMessagePlan *response_plan,
    chttp_rpc_service_client_outcome *outcome,
    crpc_error *out_error);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_RPC_SERVICE_SERVICE_H */
