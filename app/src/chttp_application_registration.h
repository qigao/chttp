#ifndef CHTTP_APPLICATION_REGISTRATION_H
#define CHTTP_APPLICATION_REGISTRATION_H
/* Internal control-plane registration; public configuration calls these once. */
#include <chttp_app/application.h>
#include "chttp_application_work.h"
typedef chttp_admission chttp_application_admission_binding;
typedef chttp_asset_policy_result chttp_application_asset_policy_result;
typedef chttp_asset_policy_fn chttp_application_asset_policy_fn;
typedef chttp_assets chttp_application_asset_binding;
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

typedef struct chttp_application_upload_binding {
  chttp_application_worker_binding controller;
  const cmeta_function_abi_desc *open_abi;
  chttp_application_upload_open_fn open;
  void (*close)(void *, chttp_body_sink *, int);
  void (*cleanup)(void *, chttp_body_sink *, const chttp_application_work_outcome *);
} chttp_application_upload_binding;

typedef struct chttp_application_websocket_binding {
  chttp_application_worker_binding controller;
  const cmeta_function_abi_desc *event_abi;
  chttp_application_websocket_event_fn event;
  size_t event_capacity; /* Required; includes the running event. Close has its own reserved slot. */
  size_t max_frame_bytes, max_message_bytes, max_buffered_input_bytes;
  const cmeta_function_abi_desc *ready_abi;
  chttp_application_websocket_event_fn ready;
} chttp_application_websocket_binding;

#ifdef __cplusplus
extern "C" {
#endif
/* Control-thread only, before the first start attempt. abi must describe the
 * exact Invocation action, including canonical argument identity and ownership.
 * Hook rows resolve canonical Bean contexts and are copied during mount.
 * Binding metadata/code
 * and hook contexts are borrowed through Application close. Receiver ownership
 * is resolved from the canonical ACE graph; callers cannot provide a raw Bean. */
/* Private ordinary-H1/H2 header callback. Resolves a canonical Bean and exact
 * ABI before start; no caller-supplied receiver/context or owner callback.
 * Body/Session are absent, views expire on return. Return the existing copied
 * status/Retry-After decision. Same-pool close is forbidden as for actions. */
int chttp_application_worker_admission(chttp_application *, const chttp_application_admission_binding *);
int chttp_application_worker_mount(chttp_application *, const chttp_application_worker_binding *);
/* Resolves one canonical Bean for header open/body close/resource cleanup and
 * the final Invocation. Open input is callback-scoped and has no Session/body.
 * A successful open transfers its sink resource through cleanup; close must
 * preserve it through final action/terminal. All user stages run on workers.
 * The copied sink user is exposed only to the final streamed request view. */
int chttp_application_worker_upload(chttp_application *, const chttp_application_upload_binding *);
/* One canonical Bean, worker-only opening Invocation and serial event methods.
 * Opening explicitly accepts or returns an ordinary buffered denial. Events
 * have copied payload/request metadata, no mutable Session and callback-scoped
 * views. Queue storage is charged to the shared retained budget at admission.
 * after_completion joins native terminal, native producer retirement and every
 * admitted event including Close; metadata/code borrow Application lifetime. */
int chttp_application_worker_websocket(chttp_application *, const chttp_application_websocket_binding *);
/* One private asset/SPA mount before start, using the same bounded Invocation
 * pool and entered hooks as Controllers. Descriptor/hook rows are copied;
 * strings/code/metadata borrow Application lifetime. Policy/after_completion resolve a canonical Bean.
 * Policy returns status; its optional ETag borrows live Bean storage until
 * immediate copy, never stack storage. Files/root topology stay immutable.
 * The framework asset operation is visible to Invocation reflection. */
int chttp_application_worker_assets(chttp_application *, const chttp_application_asset_binding *);
/* Generated synchronous direct/CFlow/Plugin HTTP methods, using the same pool.
 * Binding/execution metadata is borrowed through close; receiver is canonical
 * ACE state. CFlow retains producer-owned captures and requires no borrowed
 * executor. Plugin resolves under a mount-owned lease; registry/code stay live
 * through close. The required codec must match the MethodPlan; it is borrowed
 * only while mounting and compiling owned JSON input/output FormatPlans.
 * One required record may bind the whole JSON body, with scalar HTTP inputs;
 * nonempty bodies require application/json before method hooks execute. */
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
int chttp_application_is_worker(const chttp_application *);
int chttp_application_worker_stats(chttp_application *, chttp_application_work_stats *);
#ifdef __cplusplus
}
#endif
#endif
