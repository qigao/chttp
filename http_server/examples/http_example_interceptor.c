#include "http_example_app.h"
#include <string.h>

/* Prepare headers before continuing: handlers may commit their replies. */
int http_example_intercept(void *user, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_server_next *next) {
  (void)user;
  (void)request;
  int status = chttp_server_response_set_header(response, "X-Example", "rpc-middleware");
  if (status == SALTS_OK)
    status = chttp_server_response_set_header(response, "X-Content-Type-Options", "nosniff");
  return status == SALTS_OK ? chttp_server_next_call(next) : status;
}

/* Route-local policy leaves /file's conditional caching intact. */
int http_example_no_store(void *user, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_server_next *next) {
  (void)user;
  (void)request;
  const int status = chttp_server_response_set_header(response, "Cache-Control", "no-store");
  return status == SALTS_OK ? chttp_server_next_call(next) : status;
}

static cmeta_status example_no_store_before(void *context,
    const chttp_service_call *call, bool *proceed) {
  (void)context;
  *proceed = true;
  return chttp_server_response_set_header(call->response,
      "Cache-Control", "no-store") == SALTS_OK ? CMETA_OK : CMETA_INVALID_ARGUMENT;
}

int http_example_select_policies(void *context, const DataBindBindingPlan *binding,
    const DataBindServiceNativeBinding *native,
    chttp_service_interceptor_hook *hooks, size_t capacity, size_t *count) {
  (void)context;
  /* Generated operation identity is authoritative; fail closed for a new method
   * until the application's deployment policy explicitly admits it. */
  if (strcmp(data_bind_binding_plan_operation_id(binding),
          "Calculator.Add") != 0 ||
      !cmeta_function_desc_equal(data_bind_binding_plan_function(binding), native->function))
    return SALTS_ENOTSUP;
  if (capacity == 0u) return SALTS_ENOBUFS;
  hooks[0] = (chttp_service_interceptor_hook){NULL, example_no_store_before, NULL, NULL};
  *count = 1u;
  return SALTS_OK;
}
