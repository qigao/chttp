#ifndef CHTTP_SERVICE_SERVICE_H
#define CHTTP_SERVICE_SERVICE_H

#include <http_server/http.h>

#include <data_bind_method_plan.h>
#include <data_bind_native.h>

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
  size_t native_workspace_bytes;
  size_t native_max_depth;
  size_t native_max_items;
  size_t native_max_owned_bytes;
} chttp_service_config;

#define CHTTP_SERVICE_CONFIG_INIT \
  { sizeof(chttp_service_config), 0u, 1024u, 4096u, 16384u, 16u, 256u, 65536u }

/**
 * Exact native operation adapter.
 *
 * HTTP projection is already compiled into method_plan. The callback must not
 * inspect DataBind Service HTTP annotations or compile another projection.
 *
 * A typical generated/exact adapter:
 *   1. creates exact request/param/return/error staging;
 *   2. calls data_bind_binding_plan_bind_inputs();
 *   3. invokes the exact C signature;
 *   4. calls data_bind_binding_plan_write_outcome();
 *   5. returns DATA_BIND_OK with the canonical outcome.
 *
 * provider/native_options/plan are borrowed for this synchronous callback only.
 */
typedef DataBindStatus (*chttp_service_exact_http_fn)(
    void *user,
    const DataBindBindingPlan *plan,
    const DataBindBindingProvider *provider,
    const DataBindNativeOptions *native_options,
    DataBindBindingOutcome *outcome,
    DataBindBindingPlanDiagnostic *diagnostic);

typedef struct chttp_service_http_mount {
  size_t size;
  const DataBindHttpMethodPlan *method_plan;
  chttp_service_exact_http_fn invoke;
  void *user;
  const chttp_server_middleware *middleware;
  size_t middleware_count;
} chttp_service_http_mount;

#define CHTTP_SERVICE_HTTP_MOUNT_INIT \
  { sizeof(chttp_service_http_mount), NULL, NULL, NULL, NULL, 0u }

/**
 * Initialize bounded owner-thread scratch for generated HTTP MethodPlan mounts.
 *
 * Phase 1 supports path/query/header/cookie scalar ingress and a single scalar
 * success/typed-error response body. Structured body/egress is intentionally
 * fail-closed until the shared DataBind FormatPlan writer is mounted.
 */
int chttp_service_init(
    chttp_service *service, const chttp_service_config *config);

/**
 * Mount one already-compiled DataBind HTTP MethodPlan on CHttp::Server.
 *
 * The MethodPlan and every native descriptor referenced by its BindingPlan are
 * borrowed until chttp_service_destroy(). The server copies route metadata, but
 * its route user pointer references service-owned method storage.
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
