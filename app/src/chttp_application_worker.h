#ifndef CHTTP_APPLICATION_WORKER_H
#define CHTTP_APPLICATION_WORKER_H
/* Internal buffered Controller prototype for the migration in
 * docs/HTTP_APPLICATION_DESIGN.md. Not installed; not an alternate public API.
 * Only empty Controller declarations (no owner callbacks), no assets/
 * custom owner admission are admitted. The completed replacement removes this
 * temporary private mounting surface when public Invocation admission is ready. */
#include <chttp_app/application.h>
#include "chttp_application_work.h"
#include "chttp_invocation.h"
#include "../../http_common/http/chttp_method_hooks.h"

static const cmeta_type_identity chttp_admission_request_identity = CMETA_TYPE_ID_ATOM_INIT("chttp.admission.request");
static const cmeta_type_identity chttp_admission_result_identity = CMETA_TYPE_ID_ATOM_INIT("chttp.admission.result");
static const cmeta_type_desc chttp_admission_request_type = {"chttp_server_request_view", sizeof(chttp_server_request_view),
    CMETA_ALIGNOF(chttp_server_request_view), CMETA_T_OBJECT, NULL, NULL, &chttp_admission_request_identity};
static const cmeta_type_desc chttp_admission_result_type = {"chttp_server_admission_result", sizeof(chttp_server_admission_result),
    CMETA_ALIGNOF(chttp_server_admission_result), CMETA_T_OBJECT, NULL, NULL, &chttp_admission_result_identity};
static const cmeta_type_desc chttp_admission_request_pointer = {"const chttp_server_request_view *", sizeof(void *),
    CMETA_ALIGNOF(void *), CMETA_T_POINTER, &chttp_admission_request_type, NULL, NULL};
static const cmeta_type_desc chttp_admission_result_pointer = {"chttp_server_admission_result *", sizeof(void *),
    CMETA_ALIGNOF(void *), CMETA_T_POINTER, &chttp_admission_result_type, NULL, NULL};
#define CHTTP_ADMISSION_ACTION(name_) \
  FunctionDeclAsAbiResult(io, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, name_, \
    (void *, self, CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_RECEIVER, &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER), \
    (const chttp_server_request_view *, request, CMETA_PARAM_IN | CMETA_PARAM_BORROWED, &chttp_admission_request_pointer, CMETA_ABI_OBJECT_POINTER), \
    (chttp_server_admission_result *, result, CMETA_PARAM_OUT | CMETA_PARAM_BORROWED, &chttp_admission_result_pointer, CMETA_ABI_OBJECT_POINTER))
typedef struct chttp_application_admission_binding {
  const char *component_id;
  const cmeta_data_desc *receiver;
  const cmeta_function_abi_desc *abi;
  chttp_application_admission_fn invoke;
} chttp_application_admission_binding;

/* Private method scope. Mount copies descriptor rows; callback code/contexts
 * borrow canonical ACE graph lifetime. Call DTO/Session views are read-only. */
typedef struct chttp_application_method_interceptor {
  const char *component_id;
  const cmeta_data_desc *receiver;
  chttp_method_chain_before_fn before;
  chttp_method_chain_after_fn after;
  chttp_method_chain_error_fn on_error;
  chttp_method_completion_fn after_completion;
} chttp_application_method_interceptor;

typedef struct chttp_application_worker_binding {
  const char *component_id;
  const cmeta_data_desc *receiver;
  chttp_method method;
  const char *path;
  const cmeta_function_abi_desc *abi;
  chttp_invocation_action_fn invoke;
  chttp_application_work_completion_fn after_completion;
  const chttp_invocation_interceptor *hooks;
  size_t hook_count;
} chttp_application_worker_binding;
#ifdef __cplusplus
extern "C" {
#endif
/* Control-thread only, before the first start attempt. abi must describe the
 * exact Invocation action, including canonical argument identity and ownership.
 * Hook rows resolve canonical Bean contexts and are copied during mount.
 * Binding metadata/code
 * and hook contexts are borrowed through Application close. Receiver ownership
 * is resolved from the canonical ACE graph; callers cannot provide a raw Bean. */
int chttp_application_worker_prepare(chttp_application *, const chttp_application_work_config *);
/* Private ordinary-H1/H2 header callback. Resolves a canonical Bean and exact
 * ABI before start; no caller-supplied receiver/context or owner callback.
 * Body/Session are absent, views expire on return. Return the existing copied
 * status/Retry-After decision. Same-pool close is forbidden as for actions. */
int chttp_application_worker_admission(chttp_application *, const chttp_application_admission_binding *);
int chttp_application_worker_mount(chttp_application *, const chttp_application_worker_binding *);
/* Generated synchronous direct/CFlow/Plugin HTTP methods, using the same pool.
 * Binding/execution metadata is borrowed through close; receiver is canonical
 * ACE state. CFlow retains producer-owned captures and requires no borrowed
 * executor. Plugin resolves under a mount-owned lease; registry/code stay live
 * through close. The required codec must match the MethodPlan; it is borrowed
 * only while mounting and compiling owned output FormatPlans. Legacy
 * borrowed-executor dispatch remains outside this slice. */
int chttp_application_worker_mount_http(chttp_application *, const char *,
    const cmeta_data_desc *, const chttp_http_method *, DataBind *,
    const chttp_application_method_interceptor *, size_t);
/* Private synchronous generated RPC envelope; batch methods run sequentially
 * within one worker invocation. Plugin-backed executions retain their provider
 * context and mount lease. The matching codec is borrowed only at mount.
 * Hooks apply per bound method, including notifications. */
int chttp_application_worker_mount_rpc(chttp_application *, const char *,
    const cmeta_data_desc *, const chttp_rpc_method *, DataBind *,
    const chttp_application_method_interceptor *, size_t);
int chttp_application_worker_current(const chttp_application *);
int chttp_application_worker_stats(chttp_application *, chttp_application_work_stats *);
#ifdef __cplusplus
}
#endif
#endif
