#ifndef CHTTP_APPLICATION_H
#define CHTTP_APPLICATION_H

#include <http_server/http.h>
#include <http_server/rpc.h>
#include <chttp_app/web.h>
#include <cmeta/configurator.h>
#include <cmeta/function.h>
#include <cmeta/ace_interceptor.h>
#include <cflow/function_projection.h>
#include <cflow/executor.h>
#include <data_bind_method_plan.h>
#include <data_bind_native_binding.h>
#include <cmeta/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chttp_application { void *impl; } chttp_application;

typedef struct chttp_exchange_result {
  int status;
  bool rejected;
} chttp_exchange_result;

/** Borrowed for one dispatch only. Never retain the view. Generated deferred
 * invocation and retained response sources follow their own terminal ownership
 * contracts; arbitrary background work cannot retain an unowned Bean pointer. */
typedef struct chttp_exchange {
  const chttp_server_request_view *request;
  chttp_server_response *response;
  const cmeta_function_desc *operation;
  chttp_exchange_result *result;
} chttp_exchange;

typedef int (*chttp_action_fn)(void *, const chttp_exchange *);
/* Static metadata keeps C11 FunctionDecl initializers constant across DLLs.
 * CMeta compares the explicit semantic identity, never descriptor addresses. */
static const cmeta_type_identity chttp_exchange_identity = CMETA_TYPE_ID_ATOM_INIT("chttp.exchange");
static const cmeta_type_desc chttp_exchange_type = {"chttp_exchange", sizeof(chttp_exchange),
    CMETA_ALIGNOF(chttp_exchange), CMETA_T_OBJECT, NULL, NULL, &chttp_exchange_identity};
static const cmeta_type_desc chttp_exchange_pointer_type = {"const chttp_exchange *",
    sizeof(chttp_exchange *), CMETA_ALIGNOF(chttp_exchange *), CMETA_T_POINTER,
    &chttp_exchange_type, NULL, NULL};

/** Declare the native action and its canonical reflection from one signature.
 * The receiver is the ACE-owned Controller object; all arguments are borrowed. */
#define CHTTP_ACTION(name_) \
  FunctionDeclAsAbiResult(io, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, name_, \
    (void *, self, CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_RECEIVER, \
      &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER), \
    (const chttp_exchange *, exchange, CMETA_PARAM_IN | CMETA_PARAM_BORROWED, \
      &chttp_exchange_pointer_type, CMETA_ABI_OBJECT_POINTER))

typedef struct chttp_action {
  chttp_method method;
  const char *path;
  const cmeta_function_abi_desc *abi;
  chttp_action_fn invoke;
  chttp_server_body_open_fn body_open;
  chttp_server_body_close_fn body_close;
} chttp_action;
#define CHTTP_MAPPING(method_, path_, action_) \
  {method_, path_, FunctionAbi(action_), action_, NULL, NULL}
#define CHTTP_GET(path_, action_) CHTTP_MAPPING(CHTTP_METHOD_GET, path_, action_)
#define CHTTP_POST(path_, action_) CHTTP_MAPPING(CHTTP_METHOD_POST, path_, action_)

CMETA_INTERCEPTOR_TYPE(chttp_dispatch_chain, chttp_exchange, chttp_exchange_result);

/** Each hook context is another explicitly selected ACE Bean. Hook code and
 * metadata are borrowed through complete Application shutdown. Global hooks
 * execute in declaration order and unwind in reverse, using ACE semantics. */
typedef struct chttp_interceptor {
  const char *component_id;
  const cmeta_data_desc *receiver;
  chttp_dispatch_chain_before_fn before;
  chttp_dispatch_chain_after_fn after;
  chttp_dispatch_chain_error_fn on_error;
} chttp_interceptor;

typedef enum chttp_dispatch {
  CHTTP_DISPATCH_INLINE = 0,
  CHTTP_DISPATCH_DEFERRED = 1,
  CHTTP_DISPATCH_CFLOW = 2,
  CHTTP_DISPATCH_PLUGIN = 3
} chttp_dispatch;

typedef struct chttp_http_method {
  const DataBindHttpMethodPlan *plan;
  const DataBindServiceNativeBinding *binding;
  const DataBindNativeExecution *execution;
  chttp_dispatch dispatch;
  cflow_executor *executor;
  const cflow_function_typed_adapter_projection *projection;
  cmeta_plugin_registry *plugin_registry;
  cmeta_plugin_ref plugin_ref;
  const char *plugin_export_id;
} chttp_http_method;

typedef struct chttp_rpc_method {
  const char *path;
  const DataBindRpcMethodPlan *plan;
  const DataBindServiceNativeBinding *binding;
  const DataBindNativeExecution *execution;
  cmeta_plugin_registry *plugin_registry;
  cmeta_plugin_ref plugin_ref;
} chttp_rpc_method;

/** WebSocket endpoint on an ACE-owned Controller. Callback user is the Controller
 * receiver; capacities and callback lifetimes follow the HTTP WebSocket contract. */
typedef struct chttp_websocket_endpoint {
  const char *path;
  size_t max_frame_bytes;
  size_t max_message_bytes;
  size_t max_buffered_input_bytes;
  chttp_websocket_open_fn on_open;
  chttp_websocket_event_fn on_event;
} chttp_websocket_endpoint;

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

/** All provider/Controller/Interceptor metadata remains borrowed until close
 * settles. Construction config_value is borrowed only by ACE create().
 * Limits and policy values are copied. Objects are owned by the ACE graph,
 * whose lifetime covers HTTP terminal, streams and deferred task finalizers. */
typedef struct chttp_application_config {
  chttp_server_config http;
  const salts_configurator_deployment *components;
  size_t component_count;
  const salts_configurator_selection *selections;
  size_t selection_count;
  size_t dependency_capacity;
  const chttp_controller *controllers;
  size_t controller_count;
  const chttp_interceptor *interceptors;
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
  chttp_server_admission_fn admission;
  void *admission_context;
  const chttp_web_security_policy *security;
  const chttp_web_asset_mount *assets;
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

/** Explicit handled rejection for an Interceptor before-hook. It must also set
 * proceed=false. ACE unwinds entered error hooks; HTTP retains this response. */
int chttp_exchange_reject(const chttp_exchange *, unsigned int status_code,
    const char *content_type, const void *body, size_t body_size);

#ifdef __cplusplus
}
#endif
#endif
