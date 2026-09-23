#ifndef CHTTP_SERVICE_SERVICE_H
#define CHTTP_SERVICE_SERVICE_H

#include <http_server/http.h>

#include <data_bind_binding_plan.h>
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
  DataBind *contract;
  size_t method_capacity;
  size_t max_binding_value_bytes;
  size_t max_json_depth;
  size_t max_response_body_bytes;
  size_t native_workspace_bytes;
  size_t native_max_depth;
  size_t native_max_items;
  size_t native_max_owned_bytes;
} chttp_service_config;

#define CHTTP_SERVICE_CONFIG_INIT \
  { sizeof(chttp_service_config), NULL, 0u, 1024u, 16u, 4096u, 16384u, 16u, 256u, 65536u }

typedef struct chttp_service_context {
  size_t size;
  const chttp_server_request_view *http;
  /** Handler-scoped bounded output scratch owned by CHttp::Service. */
  void *response_scratch;
  size_t response_scratch_bytes;
} chttp_service_context;

#define CHTTP_SERVICE_CONTEXT_INIT \
  { sizeof(chttp_service_context), NULL, NULL, 0u }

typedef struct chttp_service_result {
  size_t size;
  unsigned int status_code;
  const char *content_type;
  const void *body;
  size_t body_size;
} chttp_service_result;

#define CHTTP_SERVICE_RESULT_INIT \
  { sizeof(chttp_service_result), 0u, NULL, NULL, 0u }

/**
 * Exact-ABI execution adapter.
 *
 * The adapter may create typed request/parameter/response storage on its stack,
 * call data_bind_binding_plan_bind_inputs(), invoke the reflected native
 * function through its exact C signature, and publish a response body into
 * context->response_scratch. result.body must either be NULL for an empty body
 * or point inside that bounded scratch. The BindingPlan/provider/native-options/
 * context pointers are borrowed only for this callback.
 *
 * Until DataBind native encode (#151) lands, response serialization remains
 * adapter-owned. The callback must not retain request/provider/context views.
 */
typedef int (*chttp_service_executor_fn)(
    void *user,
    const DataBindBindingPlan *plan,
    const DataBindBindingProvider *provider,
    const DataBindNativeOptions *native_options,
    const chttp_service_context *context,
    chttp_service_result *result,
    DataBindBindingPlanDiagnostic *diagnostic);

typedef struct chttp_service_http_method {
  size_t size;
  const char *service;
  const char *operation;
  const DataBindServiceNativeBinding *native;
  chttp_service_executor_fn execute;
  void *user;
  const chttp_server_middleware *middleware;
  size_t middleware_count;
} chttp_service_http_method;

#define CHTTP_SERVICE_HTTP_METHOD_INIT \
  { sizeof(chttp_service_http_method), NULL, NULL, NULL, NULL, NULL, NULL, 0u }

/**
 * Initialize a bounded typed-service registry.
 *
 * contract is borrowed until chttp_service_destroy(). The service owns bounded
 * method records, BindingPlans, native workspace, and HTTP scalar scratch.
 */
int chttp_service_init(chttp_service *service, const chttp_service_config *config);

/**
 * Compile and register one DataBind Service HTTP projection on server.
 *
 * Registration is transactional per method: BindingPlan/route storage is not
 * retained if server route registration fails. The service and all native
 * CMeta descriptors referenced by method->native must outlive the server route.
 */
int chttp_service_register_http(chttp_service *service,
                                chttp_server *server,
                                const chttp_service_http_method *method);

/**
 * Release compiled plans and bounded scratch.
 *
 * All servers containing routes registered by this service must be stopped and
 * destroyed before this call.
 */
int chttp_service_destroy(chttp_service *service);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_SERVICE_SERVICE_H */
