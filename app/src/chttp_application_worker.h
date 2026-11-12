#ifndef CHTTP_APPLICATION_WORKER_H
#define CHTTP_APPLICATION_WORKER_H
/* Internal buffered Controller prototype for the migration in
 * docs/HTTP_APPLICATION_DESIGN.md. Not installed; not an alternate public API.
 * Only empty Controller declarations (no owner callbacks), no assets/
 * custom owner admission are admitted. The completed replacement removes this
 * temporary private mounting surface when public Invocation admission is ready. */
#include <chttp_app/application.h>
#include "chttp_application_work.h"
#include "../../http_common/http/chttp_method_hooks.h"

/* Private method scope. Mount copies descriptor rows; callback code/contexts
 * borrow canonical ACE graph lifetime. Call DTO/Session views are read-only. */
typedef struct chttp_application_method_interceptor {
  const char *component_id;
  const cmeta_data_desc *receiver;
  chttp_method_chain_before_fn before;
  chttp_method_chain_after_fn after;
  chttp_method_chain_error_fn on_error;
} chttp_application_method_interceptor;

typedef struct chttp_application_worker_binding {
  const char *component_id;
  const cmeta_data_desc *receiver;
  chttp_method method;
  const char *path;
  chttp_application_work_fn invoke;
  chttp_application_work_completion_fn after_completion;
  const chttp_application_work_chain_hook *hooks;
  size_t hook_count;
} chttp_application_worker_binding;
/* Control-thread only, before the first start attempt. Binding metadata/code
 * and hook contexts are borrowed through Application close. Receiver ownership
 * is resolved from the canonical ACE graph; callers cannot provide a raw Bean. */
int chttp_application_worker_prepare(chttp_application *, const chttp_application_work_config *);
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
#endif
