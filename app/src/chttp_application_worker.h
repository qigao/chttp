#ifndef CHTTP_APPLICATION_WORKER_H
#define CHTTP_APPLICATION_WORKER_H
/* Internal buffered Controller prototype for the migration in
 * docs/HTTP_APPLICATION_DESIGN.md. Not installed; not an alternate public API.
 * Only empty Controller declarations (no owner callbacks), no legacy assets/
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

static const cmeta_type_identity chttp_upload_sink_identity = CMETA_TYPE_ID_ATOM_INIT("chttp.upload.sink");
static const cmeta_type_desc chttp_upload_sink_type = {"chttp_body_sink", sizeof(chttp_body_sink),
    CMETA_ALIGNOF(chttp_body_sink), CMETA_T_OBJECT, NULL, NULL, &chttp_upload_sink_identity};
static const cmeta_type_desc chttp_upload_sink_pointer = {"chttp_body_sink *", sizeof(void *),
    CMETA_ALIGNOF(void *), CMETA_T_POINTER, &chttp_upload_sink_type, NULL, NULL};
#define CHTTP_UPLOAD_OPEN_ACTION(name_) \
  FunctionDeclAsAbiResult(io, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, name_, \
    (void *, self, CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_RECEIVER, &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER), \
    (const chttp_server_request_view *, request, CMETA_PARAM_IN | CMETA_PARAM_BORROWED, &chttp_admission_request_pointer, CMETA_ABI_OBJECT_POINTER), \
    (chttp_body_sink *, sink, CMETA_PARAM_OUT | CMETA_PARAM_BORROWED, &chttp_upload_sink_pointer, CMETA_ABI_OBJECT_POINTER))
typedef struct chttp_application_upload_binding {
  chttp_application_worker_binding controller;
  const cmeta_function_abi_desc *open_abi;
  chttp_application_upload_open_fn open;
  void (*close)(void *, chttp_body_sink *, int);
  void (*cleanup)(void *, chttp_body_sink *, const chttp_application_work_outcome *);
} chttp_application_upload_binding;

static const cmeta_type_identity chttp_websocket_event_scope_identity = CMETA_TYPE_ID_ATOM_INIT("chttp.websocket.event-scope");
static const cmeta_type_desc chttp_websocket_event_scope_type = {"chttp_application_websocket_event",
    sizeof(chttp_application_websocket_event), CMETA_ALIGNOF(chttp_application_websocket_event),
    CMETA_T_OBJECT, NULL, NULL, &chttp_websocket_event_scope_identity};
static const cmeta_type_desc chttp_websocket_event_scope_pointer = {"const chttp_application_websocket_event *",
    sizeof(void *), CMETA_ALIGNOF(void *), CMETA_T_POINTER, &chttp_websocket_event_scope_type, NULL, NULL};
#define CHTTP_WEBSOCKET_EVENT_ACTION(name_) \
  FunctionDeclAsAbiResult(io, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, name_, \
    (void *, self, CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_RECEIVER, &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER), \
    (const chttp_application_websocket_event *, event, CMETA_PARAM_IN | CMETA_PARAM_BORROWED, &chttp_websocket_event_scope_pointer, CMETA_ABI_OBJECT_POINTER))
typedef struct chttp_application_websocket_binding {
  chttp_application_worker_binding controller;
  const cmeta_function_abi_desc *event_abi;
  chttp_application_websocket_event_fn event;
  size_t event_capacity; /* Required; includes the running event. Close has its own reserved slot. */
  size_t max_frame_bytes, max_message_bytes, max_buffered_input_bytes;
} chttp_application_websocket_binding;

typedef struct chttp_application_asset_policy_result {
  bool immutable;
  const char *etag;
} chttp_application_asset_policy_result;
typedef int (*chttp_application_asset_policy_fn)(void *, const char *, chttp_application_asset_policy_result *);
static const cmeta_type_identity chttp_asset_relative_identity = CMETA_TYPE_ID_ATOM_INIT("chttp.asset.relative-char");
static const cmeta_type_identity chttp_asset_relative_const_identity = CMETA_TYPE_ID_CONST_INIT(&chttp_asset_relative_identity);
static const cmeta_type_identity chttp_asset_relative_pointer_identity = CMETA_TYPE_ID_POINTER_INIT(&chttp_asset_relative_const_identity);
static const cmeta_type_identity chttp_asset_policy_identity = CMETA_TYPE_ID_ATOM_INIT("chttp.asset.policy-result");
static const cmeta_type_desc chttp_asset_relative_const_type = {"const char", sizeof(char), CMETA_ALIGNOF(char),
    CMETA_T_INTEGER, NULL, NULL, &chttp_asset_relative_const_identity};
static const cmeta_type_desc chttp_asset_relative_pointer = {"const char *", sizeof(const char *),
    CMETA_ALIGNOF(const char *), CMETA_T_POINTER, &chttp_asset_relative_const_type, NULL, &chttp_asset_relative_pointer_identity};
static const cmeta_type_desc chttp_asset_policy_type = {"chttp_application_asset_policy_result",
    sizeof(chttp_application_asset_policy_result), CMETA_ALIGNOF(chttp_application_asset_policy_result),
    CMETA_T_OBJECT, NULL, NULL, &chttp_asset_policy_identity};
static const cmeta_type_desc chttp_asset_policy_pointer = {"chttp_application_asset_policy_result *", sizeof(void *),
    CMETA_ALIGNOF(void *), CMETA_T_POINTER, &chttp_asset_policy_type, NULL, NULL};
#define CHTTP_ASSET_POLICY_ACTION(name_) \
  FunctionDeclAsAbiResult(io, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, name_, \
    (void *, self, CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_RECEIVER, &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER), \
    (const char *, relative_path, CMETA_PARAM_IN | CMETA_PARAM_BORROWED, &chttp_asset_relative_pointer, CMETA_ABI_OBJECT_POINTER), \
    (chttp_application_asset_policy_result *, result, CMETA_PARAM_OUT | CMETA_PARAM_BORROWED, &chttp_asset_policy_pointer, CMETA_ABI_OBJECT_POINTER))
typedef struct chttp_application_asset_binding {
  const chttp_web_asset_mount *mount;
  const char *component_id;
  const cmeta_data_desc *receiver;
  const cmeta_function_abi_desc *policy_abi;
  chttp_application_asset_policy_fn policy;
  chttp_application_work_completion_fn after_completion;
  const chttp_invocation_interceptor *hooks;
  size_t hook_count;
} chttp_application_asset_binding;
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
 * strings/code/metadata borrow Application lifetime. Legacy mount callbacks and
 * raw user are rejected. Policy/after_completion resolve a canonical Bean.
 * Policy returns status; its optional ETag borrows live Bean storage until
 * immediate copy, never stack storage. Files/root topology stay immutable.
 * The framework asset operation is visible to Invocation reflection. */
int chttp_application_worker_assets(chttp_application *, const chttp_application_asset_binding *);
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
