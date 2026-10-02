#ifndef CHTTP_SERVICE_SERVICE_H
#define CHTTP_SERVICE_SERVICE_H

#include <http_server/http.h>

#include <data_bind_method_plan.h>
#include <data_bind_native.h>
#include <data_bind_native_binding.h>
#include <cflow/executor.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chttp_service {
  void *impl;
} chttp_service;

typedef struct chttp_service_config {
  size_t size;
  size_t method_capacity;
  size_t max_binding_value_bytes;
  /** Hard bound for one DataBind/CSerde retained response materialization. */
  size_t max_response_body_bytes;
  size_t max_call_frame_bytes;
  size_t native_workspace_bytes;
  size_t native_max_depth;
  size_t native_max_items;
  size_t native_max_owned_bytes;
} chttp_service_config;

#define CHTTP_SERVICE_CONFIG_INIT \
  { sizeof(chttp_service_config), 0u, 1024u, 4096u, 65536u, 16384u, 16u, 256u, 65536u }

typedef enum chttp_service_execution_mode {
  /** Execute the admitted DataBindNativeExecution on the HTTP owner callback. */
  CHTTP_SERVICE_EXECUTION_INLINE_DIRECT = 0,
  /**
   * Bind/materialize on the HTTP owner callback, defer the response, then run
   * the admitted DataBindNativeExecution on the borrowed bounded executor.
   */
  CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT = 1
} chttp_service_execution_mode;

/**
 * One admitted generated HTTP operation.
 *
 * All semantic/native identity comes from DataBind/CMeta producer artifacts:
 * - method_plan owns HTTP projection + BindingPlan semantics;
 * - native_binding owns exact request/response/error storage layout;
 * - execution owns FunctionMeta + FunctionAbi + generated exact adapter.
 *
 * Mount validates these three artifacts before publishing the route. The
 * synchronous request hot path performs no reflection lookup/equality work.
 */
typedef struct chttp_service_http_mount {
  size_t size;
  const DataBindHttpMethodPlan *method_plan;
  const DataBindServiceNativeBinding *native_binding;
  const DataBindNativeExecution *execution;
  const chttp_server_middleware *middleware;
  size_t middleware_count;
  chttp_service_execution_mode execution_mode;
  /**
   * Borrowed bounded executor required by DEFERRED_DIRECT.
   *
   * The executor must outlive all accepted Service tasks. After server stop
   * succeeds, the application must wait for this executor to become idle
   * before destroying the server or Service.
   */
  cflow_executor *executor;
} chttp_service_http_mount;

#define CHTTP_SERVICE_HTTP_MOUNT_INIT \
  { sizeof(chttp_service_http_mount), NULL, NULL, NULL, NULL, 0u, \
    CHTTP_SERVICE_EXECUTION_INLINE_DIRECT, NULL }

/**
 * Initialize bounded runtime storage for generated HTTP MethodPlan mounts.
 *
 * Fixed-width scalar/enum ingress uses per-invocation bounded scratch and
 * native workspace. Mounted operations retain only immutable/control metadata;
 * request/response/error staging is allocated for each invocation so independent
 * calls do not share mutable native state.
 *
 * Egress is transaction-local: DataBind/CSerde materializes directly into a
 * pooled mem_buffer_t bounded by max_response_body_bytes, explicitly finishes
 * the writer, then publishes through chttp_server_reply_buffer(). No shared
 * response scratch is reused across connections or H2 streams.
 *
 * Structured body/egress, requested HTTP context, and async execution remain
 * fail-closed until their producer-owned lifecycle/FormatPlan slices land.
 */
int chttp_service_init(
    chttp_service *service, const chttp_service_config *config);

/**
 * Mount one already-compiled DataBind HTTP MethodPlan on CHttp::Server.
 *
 * method_plan, native_binding, execution, and every descriptor they reference
 * are borrowed until chttp_service_destroy(). Mount performs canonical CMeta
 * semantic/ABI admission exactly once before route publication. The server
 * copies route metadata, while its route user pointer references immutable
 * service-owned mounted-operation storage.
 *
 * INLINE_DIRECT requires executor == NULL. DEFERRED_DIRECT requires a valid
 * borrowed cflow_executor and keeps request/CNet views callback-scoped: only
 * the already-materialized native frame crosses to the worker.
 */
int chttp_service_mount_http(
    chttp_service *service,
    chttp_server *server,
    const chttp_service_http_mount *mount);

/**
 * Release service-owned bounded scratch/method records.
 *
 * Every server containing routes mounted by this service must be stopped and
 * destroyed before this call. For DEFERRED_DIRECT mounts, accepted executor
 * work must also be idle before server destruction; this prevents a worker
 * from outliving its generation-checked deferred target.
 */
int chttp_service_destroy(chttp_service *service);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_SERVICE_SERVICE_H */
