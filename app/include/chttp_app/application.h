#ifndef CHTTP_APPLICATION_H
#define CHTTP_APPLICATION_H
#include <chttp_app/invocation.h>
#include <http_server/rpc.h>
#include <cmeta/configurator.h>
#include <cflow/function_projection.h>
#include <data_bind_method_plan.h>
#include <data_bind_native_binding.h>
#include <cmeta/plugin.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct chttp_application { void *impl; } chttp_application;
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

static const cmeta_type_identity chttp_websocket_event_scope_identity = CMETA_TYPE_ID_ATOM_INIT("chttp.websocket.event-scope");
static const cmeta_type_desc chttp_websocket_event_scope_type = {"chttp_websocket_event_view",
    sizeof(chttp_websocket_event_view), CMETA_ALIGNOF(chttp_websocket_event_view),
    CMETA_T_OBJECT, NULL, NULL, &chttp_websocket_event_scope_identity};
static const cmeta_type_desc chttp_websocket_event_scope_pointer = {"const chttp_websocket_event_view *",
    sizeof(void *), CMETA_ALIGNOF(void *), CMETA_T_POINTER, &chttp_websocket_event_scope_type, NULL, NULL};
#define CHTTP_WEBSOCKET_EVENT_ACTION(name_) \
  FunctionDeclAsAbiResult(io, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, name_, \
    (void *, self, CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_RECEIVER, &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER), \
    (const chttp_websocket_event_view *, event, CMETA_PARAM_IN | CMETA_PARAM_BORROWED, &chttp_websocket_event_scope_pointer, CMETA_ABI_OBJECT_POINTER))

static const cmeta_type_identity chttp_asset_relative_identity = CMETA_TYPE_ID_ATOM_INIT("chttp.asset.relative-char");
static const cmeta_type_identity chttp_asset_relative_const_identity = CMETA_TYPE_ID_CONST_INIT(&chttp_asset_relative_identity);
static const cmeta_type_identity chttp_asset_relative_pointer_identity = CMETA_TYPE_ID_POINTER_INIT(&chttp_asset_relative_const_identity);
static const cmeta_type_identity chttp_asset_policy_identity = CMETA_TYPE_ID_ATOM_INIT("chttp.asset.policy-result");
static const cmeta_type_desc chttp_asset_relative_const_type = {"const char", sizeof(char), CMETA_ALIGNOF(char),
    CMETA_T_INTEGER, NULL, NULL, &chttp_asset_relative_const_identity};
static const cmeta_type_desc chttp_asset_relative_pointer = {"const char *", sizeof(const char *),
    CMETA_ALIGNOF(const char *), CMETA_T_POINTER, &chttp_asset_relative_const_type, NULL, &chttp_asset_relative_pointer_identity};
static const cmeta_type_desc chttp_asset_policy_type = {"chttp_asset_policy_result",
    sizeof(chttp_asset_policy_result), CMETA_ALIGNOF(chttp_asset_policy_result),
    CMETA_T_OBJECT, NULL, NULL, &chttp_asset_policy_identity};
static const cmeta_type_desc chttp_asset_policy_pointer = {"chttp_asset_policy_result *", sizeof(void *),
    CMETA_ALIGNOF(void *), CMETA_T_POINTER, &chttp_asset_policy_type, NULL, NULL};
#define CHTTP_ASSET_POLICY_ACTION(name_) \
  FunctionDeclAsAbiResult(io, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, name_, \
    (void *, self, CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_RECEIVER, &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER), \
    (const char *, relative_path, CMETA_PARAM_IN | CMETA_PARAM_BORROWED, &chttp_asset_relative_pointer, CMETA_ABI_OBJECT_POINTER), \
    (chttp_asset_policy_result *, result, CMETA_PARAM_OUT | CMETA_PARAM_BORROWED, &chttp_asset_policy_pointer, CMETA_ABI_OBJECT_POINTER))

/* Reflected header-only worker callback on a canonical Bean. No Session/body;
 * copied status/Retry-After decides admission. Errors propagate, never allow. */
typedef struct chttp_admission {
  const char *component_id;
  const cmeta_data_desc *receiver;
  const cmeta_function_abi_desc *abi;
  chttp_admission_fn invoke;
} chttp_admission;
/* Reflected open and serial worker write/close/cleanup. Failed open transfers
 * nothing. Successful open owns its sink through exactly-once cleanup; close
 * must keep sink.user live for the final Invocation and cleanup. No throw/jump. */
typedef struct chttp_upload {
  const cmeta_function_abi_desc *open_abi;
  chttp_upload_open_fn open;
  void (*close)(void *, chttp_body_sink *, int);
  void (*cleanup)(void *, chttp_body_sink *, const chttp_response_outcome *);
} chttp_upload;
/* Init copies descriptor/hook rows. Code, strings and canonical metadata stay
 * borrowed through close. before/action/after share a scoped response draft;
 * after_completion joins business, Session and native/resource terminals. */
typedef struct chttp_action {
  chttp_method method;
  const char *path;
  const cmeta_function_abi_desc *abi;
  chttp_invocation_action_fn invoke;
  chttp_completion_fn after_completion;
  const chttp_invocation_interceptor *hooks;
  size_t hook_count;
  const chttp_upload *upload;
} chttp_action;
#define CHTTP_MAPPING(method_, path_, action_) \
  {method_, path_, FunctionAbi(action_), action_, NULL, NULL, 0u, NULL}
#define CHTTP_GET(path_, action_) CHTTP_MAPPING(CHTTP_METHOD_GET, path_, action_)
#define CHTTP_POST(path_, action_) CHTTP_MAPPING(CHTTP_METHOD_POST, path_, action_)
/* Per bound DTO operation (including RPC notifications), separate from the
 * enclosing request chain. Views expire on callback return; completion carries
 * frozen facts only. All callbacks run on workers and must not throw/jump. */
typedef struct chttp_application_method_interceptor {
  const char *component_id;
  const cmeta_data_desc *receiver;
  chttp_method_chain_before_fn before;
  chttp_method_chain_after_fn after;
  chttp_method_chain_error_fn on_error;
  chttp_method_completion_fn after_completion;
} chttp_application_method_interceptor;
typedef enum chttp_dispatch {
  CHTTP_DISPATCH_DIRECT = 0, CHTTP_DISPATCH_CFLOW = 2, CHTTP_DISPATCH_PLUGIN = 3
} chttp_dispatch;
/* DIRECT/CFLOW/PLUGIN run synchronously on the owned pool. No borrowed executor.
 * Codec must match plan and is borrowed only at init; canonical metadata,
 * plugin registry/code and projection captures remain live through close. */
typedef struct chttp_http_method {
  const DataBindHttpMethodPlan *plan;
  const DataBindServiceNativeBinding *binding;
  const DataBindNativeExecution *execution;
  chttp_dispatch dispatch;
  const cflow_function_typed_adapter_projection *projection;
  cmeta_plugin_registry *plugin_registry;
  cmeta_plugin_ref plugin_ref;
  const char *plugin_export_id;
  DataBind *codec; /* Same schema as plan; borrowed only during init. */
  const chttp_application_method_interceptor *hooks;
  size_t hook_count;
} chttp_http_method;
typedef struct chttp_rpc_method {
  const char *path;
  const DataBindRpcMethodPlan *plan;
  const DataBindServiceNativeBinding *binding;
  const DataBindNativeExecution *execution;
  cmeta_plugin_registry *plugin_registry;
  cmeta_plugin_ref plugin_ref;
  DataBind *codec;
  const chttp_application_method_interceptor *hooks;
  size_t hook_count;
} chttp_rpc_method;
/* Opening must be GET, without upload. Explicit accept or buffered denial.
 * Optional ready executes after acceptance, then events are serial per peer.
 * ready/event use CHTTP_WEBSOCKET_EVENT_ACTION metadata and callback-scoped peer
 * and request views, without mutable Session. Capacity includes running events;
 * Close and ready use fixed reserved state. Errors close the peer and survive
 * in the final completion outcome. All callbacks execute on workers. */
typedef struct chttp_websocket_endpoint {
  chttp_action opening;
  const cmeta_function_abi_desc *event_abi;
  chttp_websocket_event_action_fn event;
  size_t event_capacity, max_frame_bytes, max_message_bytes, max_buffered_input_bytes;
  const cmeta_function_abi_desc *ready_abi;
  chttp_websocket_event_action_fn ready; /* Optional, before events; scope.event is NULL. */
} chttp_websocket_endpoint;
/* Init copies mount/hook rows; root topology/files remain immutable until
 * native completion. Optional reflected policy runs on its canonical Bean,
 * with relative path borrowed for this call; its ETag is immediately copied
 * from live storage. Returning an ETag pointer into dead stack storage is invalid.
 * Init rejects missing/mismatched policy ABI or Bean; capacity fails explicitly. */
typedef struct chttp_assets {
  const chttp_web_asset_mount *mount;
  const char *component_id;
  const cmeta_data_desc *receiver;
  const cmeta_function_abi_desc *policy_abi;
  chttp_asset_policy_fn policy;
  chttp_completion_fn after_completion;
  const chttp_invocation_interceptor *hooks;
  size_t hook_count;
} chttp_assets;
typedef struct chttp_controller {
  const char *component_id;
  const cmeta_data_desc *receiver;
  const chttp_action *actions;
  size_t action_count;
  const chttp_http_method *http_methods;
  size_t http_method_count;
  const chttp_rpc_method *rpc_methods;
  size_t rpc_method_count;
  const chttp_websocket_endpoint *websockets;
  size_t websocket_count;
} chttp_controller;
typedef struct chttp_binding_limits {
  size_t size;
  size_t method_capacity;
  size_t max_binding_value_bytes;
  size_t max_response_body_bytes;
  size_t max_call_frame_bytes;
  size_t native_workspace_bytes;
  size_t native_max_depth;
  size_t native_max_items;
  size_t native_max_owned_bytes;
} chttp_binding_limits;
#define CHTTP_BINDING_LIMITS_INIT \
  {sizeof(chttp_binding_limits), 0u, 1024u, 4096u, 65536u, 16384u, 16u, 256u, 65536u}

/* Configuration metadata/code borrows Application lifetime. Init resolves
 * canonical Beans and copies policy/hook rows before listener start. Worker
 * limits are mandatory; all request business callbacks run on that pool.
 * Finite close failure retains every live owner for a later explicit close. */
typedef struct chttp_application_config {
  chttp_server_config http;
  chttp_worker_config workers;
  const salts_configurator_deployment *components;
  size_t component_count;
  const salts_configurator_selection *selections;
  size_t selection_count;
  size_t dependency_capacity;
  const chttp_controller *controllers;
  size_t controller_count;
  const chttp_invocation_interceptor *interceptors;
  size_t interceptor_count; /* ACE hard limit: 16. */
  chttp_binding_limits bindings;
  chttp_server_rpc_config rpc;
  chttp_server_deadlines deadlines;
  const chttp_server_execution_options *execution;
  const chttp_server_socket_options *sockets;
  const chttp_server_owner_placement_options *placement;
  const chttp_server_websocket_transport_options *websocket_transport;
  const chttp_server_cors_options *cors;
  chttp_jwt_bearer_validator *jwt;
  const chttp_admission *admission;
  const chttp_web_security_policy *security;
  const chttp_assets *assets;
} chttp_application_config;

/** Bounded loopback development defaults, H1/H2 enabled, ephemeral port.
 * Authentication, sessions and assets are explicitly unconfigured. */
chttp_application_config chttp_application_config_default(void);

/** Requires a zero owner. Resolve/activate Beans and publish all bindings
 * transactionally before listener start. Failure rolls back; if cleanup itself
 * fails a live impl is retained and must be closed. */
int chttp_application_init(chttp_application *, const chttp_application_config *);
int chttp_application_start(chttp_application *);
int chttp_application_port(const chttp_application *, uint16_t *);
int chttp_application_stats(const chttp_application *, chttp_server_stats *);
/** Control thread only. One fixed shutdown budget; zero waits without a deadline.
 * Stop ingress -> real I/O/deferred drain -> Server/DTO storage -> ACE graph.
 * Failure retains ownership. A fully cleaned terminal operational error may be
 * returned with impl == NULL; inspect the return status in either case. */
int chttp_application_close(chttp_application *, uint32_t timeout_ms);

typedef int (*chttp_application_body_fn)(chttp_application *, void *);
typedef struct chttp_application_outcome { int operation_status; int close_status; } chttp_application_outcome;
/** Status-aware C11 scoped execution; always closes a successfully initialized
 * application. If ownership cannot settle, it is returned in owner for explicit
 * close. Never supplies a no-fail CMeta cleanup for fallible server drain. */
int chttp_application_with(chttp_application *owner, const chttp_application_config *,
    chttp_application_body_fn, void *, chttp_application_outcome *);

/* Worker telemetry and scope query; neither drives protocol progress. */
int chttp_application_worker_stats(chttp_application *, chttp_worker_stats *);
int chttp_application_is_worker(const chttp_application *);

#ifdef __cplusplus
}
#endif
#endif
