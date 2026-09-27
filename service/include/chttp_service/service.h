#ifndef CHTTP_SERVICE_SERVICE_H
#define CHTTP_SERVICE_SERVICE_H

#include <http_server/http.h>

#include <data_bind_method_plan.h>
#include <data_bind_native.h>
#include <data_bind_native_binding.h>

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
  size_t max_response_body_bytes;
  size_t max_call_frame_bytes;
  size_t native_workspace_bytes;
  size_t native_max_depth;
  size_t native_max_items;
  size_t native_max_owned_bytes;
} chttp_service_config;

#define CHTTP_SERVICE_CONFIG_INIT \
  { sizeof(chttp_service_config), 0u, 1024u, 4096u, 65536u, 16384u, 16u, 256u, 65536u }

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
} chttp_service_http_mount;

#define CHTTP_SERVICE_HTTP_MOUNT_INIT \
  { sizeof(chttp_service_http_mount), NULL, NULL, NULL, NULL, 0u }

/**
 * Initialize bounded owner-thread scratch for generated HTTP MethodPlan mounts.
 *
 * Phase S1/S2 supports fixed-width scalar/enum path/query/header/cookie ingress
 * and one fixed-width scalar/enum response body. Owned string/bytes,
 * structured body/egress, requested HTTP context, and async execution remain
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
 */
int chttp_service_mount_http(
    chttp_service *service,
    chttp_server *server,
    const chttp_service_http_mount *mount);

/**
 * Release service-owned bounded scratch/method records.
 *
 * Every server containing routes mounted by this service must be stopped and
 * destroyed before this call.
 */
int chttp_service_destroy(chttp_service *service);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_SERVICE_SERVICE_H */
