#include <chttp_app/application.h>
#include "chttp_method_runtime.h"
#include "chttp_application_worker.h"
#include "chttp_invocation_internal.h"
#include "chttp_web_registration.h"
#include "../../http_server/src/chttp_server_engine.h"
#include "../../http_server/rpc/chttp_rpc_mount.h"
#include <core/clock.h>
#include <threads/threads.h>
#include <stdlib.h>
#include <string.h>

CHTTP_ACTION(chttp_expected_action);
CHTTP_INVOCATION_ACTION(chttp_expected_invocation);
CHTTP_ADMISSION_ACTION(chttp_expected_admission);
CHTTP_ASSET_POLICY_ACTION(chttp_expected_asset_policy);
CHTTP_UPLOAD_OPEN_ACTION(chttp_expected_upload_open);
CHTTP_WEBSOCKET_EVENT_ACTION(chttp_expected_websocket_event);
CHTTP_INVOCATION_ACTION(chttp_application_asset_invoke);

typedef struct application_invocation_hook { chttp_invocation_chain_hook hook; } application_invocation_hook;
typedef struct application_worker_action {
  chttp_application_work *work;
  chttp_application_work_task task;
  DataBindNativeExecution execution;
  void *receiver;
  chttp_invocation_action_fn invoke;
  chttp_application_work_completion_fn completed;
  application_invocation_hook invocation_hooks[CHTTP_APPLICATION_WORK_MAX_HOOKS];
  chttp_application_work_chain_hook hooks[CHTTP_APPLICATION_WORK_MAX_HOOKS];
  chttp_application_upload_callbacks upload;
  size_t upload_chunk_bytes;
  chttp_application_websocket_callbacks websocket;
} application_worker_action;

typedef struct application_worker_assets {
  chttp_application_work *work;
  chttp_web_asset_mount mount;
  chttp_application_asset_policy_fn policy;
  void *receiver;
  chttp_application_work_completion_fn completed;
  chttp_application_work_task task;
  application_invocation_hook invocation_hooks[CHTTP_APPLICATION_WORK_MAX_HOOKS];
  chttp_application_work_chain_hook hooks[CHTTP_APPLICATION_WORK_MAX_HOOKS];
} application_worker_assets;

typedef struct application_action { const chttp_action *action; void *receiver; } application_action;
typedef struct application_impl {
  chttp_server server;
  chttp_method_runtime methods;
  chttp_application_work workers;
  chttp_binding_limits worker_binding_limits;
  chttp_server_rpc_config worker_rpc_config;
  bool worker_rpc_ready;
  size_t worker_rpc_completion_capacity;
  application_worker_action *worker_actions;
  application_worker_assets *worker_assets;
  size_t worker_action_count;
  size_t worker_route_capacity;
  bool worker_prototype_allowed;
  bool configuration_closed;
  chttp_application_admission_fn admission_invoke;
  void *admission_receiver;
  salts_configurator_context graph;
  salts_configurator_instance *instances;
  salts_configurator_dependency *dependencies;
  size_t *order;
  size_t component_count;
  application_action *actions;
  size_t action_count;
  DataBindNativeExecution *executions;
  size_t execution_count;
  chttp_dispatch_chain_hook hooks[16];
  size_t hook_count;
  chttp_web_security_policy security;
  chttp_server_cors_options cors;
  chttp_web_asset_mount assets;
  int shutdown_status;
} application_impl;

chttp_application_config chttp_application_config_default(void) {
  enum { CONNECTIONS = 8, COMMANDS = 32, HEADER_BYTES = 4096,
         BODY_BYTES = 8192, WIRE_BYTES = 65536, TIMEOUT_MS = 5000 };
  chttp_application_config config = {0};
  config.bindings = (chttp_binding_limits)CHTTP_BINDING_LIMITS_INIT;
  config.rpc = (chttp_server_rpc_config)CHTTP_SERVER_RPC_CONFIG_INIT;
  config.http = (chttp_server_config){.host = "127.0.0.1", .backlog = CONNECTIONS,
      .network = {.backend =
#if defined(_WIN32)
          NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
          NATIVE_IO_BACKEND_EPOLL,
#else
          NATIVE_IO_BACKEND_KQUEUE,
#endif
          .connection_capacity = CONNECTIONS, .command_capacity = COMMANDS,
          .request_capacity = 16u, .completion_batch_capacity = 8u, .event_capacity = COMMANDS,
          .max_send_bytes = WIRE_BYTES, .receive_buffer_bytes = HEADER_BYTES,
          .connect_timeout_ms = TIMEOUT_MS, .read_timeout_ms = TIMEOUT_MS, .write_timeout_ms = TIMEOUT_MS},
      .route_capacity = 16u, .middleware_capacity = 8u, .max_route_middleware_count = 8u,
      .max_route_param_count = 8u, .max_route_param_bytes = 256u, .max_target_bytes = 1024u,
      .max_header_count = 32u, .max_header_bytes = HEADER_BYTES, .max_request_body_bytes = BODY_BYTES,
      .max_response_header_count = 32u, .max_response_header_bytes = HEADER_BYTES,
      .max_response_body_bytes = BODY_BYTES, .poll_slice_ms = 2u,
      .enable_http2 = 1, .h2_stream_capacity = 4u, .h2_input_buffer_bytes = WIRE_BYTES,
      .h2_output_buffer_bytes = WIRE_BYTES, .h2_hpack_dynamic_table_bytes = HEADER_BYTES,
      .h2_max_settings_count = 16u};
  return config;
}

static int application_graph_status(salts_configurator_status s) {
  switch (s) {
  case SALTS_CONFIGURATOR_OK: return SALTS_OK;
  case SALTS_CONFIGURATOR_CAPACITY_EXCEEDED: return SALTS_ENOBUFS;
  default: return SALTS_EINVAL;
  }
}

/* Bind one explicit component identity once on the control plane. ACE's public
 * instance records remain the sole object/lifetime authority; routes borrow it. */
static int application_receiver(application_impl *app, const char *id,
    const cmeta_data_desc *data, void **out) {
  if (id == NULL || !cmeta_data_desc_valid(data)) return SALTS_EINVAL;
  for (size_t i = 0u; i < app->component_count; ++i) {
    const salts_configurator_provider_binding *provider = app->graph.deployments[i].provider;
    if (strcmp(provider->component->stable_id, id) != 0) continue;
    const cmeta_object_ref *object = &app->instances[i].object;
    if (!cmeta_object_ref_valid(object) || !cmeta_data_desc_equal(data, object->data))
      return SALTS_EINVAL;
    *out = object->object;
    return SALTS_OK;
  }
  return SALTS_ENOENT;
}

static int application_method_hooks(application_impl *app,
    const chttp_application_method_interceptor *hooks, size_t count, chttp_method_hook *out) {
  if (count > CHTTP_METHOD_MAX_HOOKS || (count && !hooks)) return SALTS_EINVAL;
  for (size_t i = 0u; i < count; ++i) {
    if (!hooks[i].before && !hooks[i].after && !hooks[i].on_error && !hooks[i].after_completion) return SALTS_EINVAL;
    void *receiver = NULL;
    const int status = application_receiver(app, hooks[i].component_id, hooks[i].receiver, &receiver);
    if (status != SALTS_OK) return status;
    out[i] = (chttp_method_hook){receiver, hooks[i].before, hooks[i].after, hooks[i].on_error, hooks[i].after_completion};
  }
  return SALTS_OK;
}
int chttp_application_worker_current(const chttp_application *owner) {
  const application_impl *app = owner ? owner->impl : NULL;
  return app && chttp_application_work_is_current(&app->workers);
}
int chttp_application_worker_stats(chttp_application *owner, chttp_application_work_stats *stats) {
  application_impl *app = owner ? owner->impl : NULL;
  return app ? chttp_application_work_get_stats(&app->workers, stats) : SALTS_EINVAL;
}
static int application_worker_dispatch(void *context,
    const chttp_server_request_view *request, chttp_server_response *response) {
  application_worker_action *action = context;
  const int status = chttp_application_work_submit(action->work, request, response, &action->task);
  return chttp_application_work_admission_response(response, status);
}
static int application_invocation_run(void *context, const chttp_server_request_view *request,
    chttp_application_work_reply *reply) {
  const application_worker_action *action = context;
  const chttp_invocation_state state = chttp_invocation_state_make(request, reply, NULL);
  const chttp_invocation invocation = {&state};
  return action->invoke(action->receiver, &invocation);
}
static cmeta_status application_invocation_before(void *context, const chttp_application_work_exchange *exchange, bool *proceed) {
  const application_invocation_hook *hook = context;
  const chttp_invocation_state state = chttp_invocation_state_make(exchange->request, exchange->response, exchange->result);
  const chttp_invocation invocation = {&state};
  return hook->hook.before(hook->hook.context, &invocation, proceed);
}
static void application_invocation_after(void *context, const chttp_application_work_exchange *exchange,
    const chttp_application_work_result *result) {
  const application_invocation_hook *hook = context;
  const chttp_invocation_state state = chttp_invocation_state_make(exchange->request, exchange->response, NULL);
  const chttp_invocation invocation = {&state};
  hook->hook.after(hook->hook.context, &invocation, result);
}
static void application_invocation_error(void *context, const chttp_application_work_exchange *exchange, cmeta_status status) {
  const application_invocation_hook *hook = context;
  const chttp_invocation_state state = chttp_invocation_state_make(exchange->request, exchange->response, NULL);
  const chttp_invocation invocation = {&state};
  hook->hook.on_error(hook->hook.context, &invocation, status);
}
static void application_invocation_completed(void *context, const chttp_server_request_view *request,
    const chttp_application_work_outcome *outcome) {
  const application_worker_action *action = context;
  action->completed(action->receiver, request, outcome);
}
int chttp_application_worker_prepare(chttp_application *owner,
    const chttp_application_work_config *config) {
  application_impl *app = owner ? owner->impl : NULL;
  if (!app || !config) return SALTS_EINVAL;
  if (app->configuration_closed) return SALTS_EBUSY;
  if (!app->worker_prototype_allowed) return SALTS_ENOTSUP;
  if (app->workers.impl) return SALTS_EALREADY;
  if (app->worker_route_capacity > SIZE_MAX / sizeof(*app->worker_actions)) return SALTS_ERANGE;
  application_worker_action *actions = calloc(app->worker_route_capacity, sizeof(*actions));
  if (!actions) return SALTS_ENOMEM;
  const int status = chttp_application_work_init(&app->workers, config);
  if (status != SALTS_OK) { free(actions); return status; }
  app->worker_actions = actions;
  return SALTS_OK;
}
static int application_admission_begin(void *context, const chttp_server_request_view *request,
    chttp_server_admission_gate *gate) {
  application_impl *app = context;
  return chttp_application_work_admit(&app->workers, request, gate, app->admission_invoke, app->admission_receiver);
}
int chttp_application_worker_admission(chttp_application *owner,
    const chttp_application_admission_binding *binding) {
  application_impl *app = owner ? owner->impl : NULL;
  if (!app || !binding || !binding->invoke || !app->workers.impl) return SALTS_EINVAL;
  if (app->configuration_closed) return SALTS_EBUSY;
  if (app->admission_invoke) return SALTS_EALREADY;
  if (!cmeta_function_abi_contract_compatible(FunctionAbi(chttp_expected_admission), binding->abi)) return SALTS_EINVAL;
  void *receiver = NULL;
  int status = application_receiver(app, binding->component_id, binding->receiver, &receiver);
  if (status != SALTS_OK) return status;
  status = chttp_server_set_admission_async(&app->server, application_admission_begin, app);
  if (status != SALTS_OK) return status;
  app->admission_invoke = binding->invoke; app->admission_receiver = receiver;
  return SALTS_OK;
}
static int application_worker_hooks(application_impl *app,
    const chttp_invocation_interceptor *sources, size_t count,
    application_invocation_hook *records, chttp_application_work_chain_hook *hooks) {
  if (count > CHTTP_APPLICATION_WORK_MAX_HOOKS || (count && !sources)) return SALTS_EINVAL;
  for (size_t i = 0u; i < count; ++i) {
    const chttp_invocation_interceptor *source = &sources[i];
    if (!source->before && !source->after && !source->on_error) return SALTS_EINVAL;
    void *receiver = NULL;
    const int status = application_receiver(app, source->component_id, source->receiver, &receiver);
    if (status != SALTS_OK) return status;
    records[i].hook = (chttp_invocation_chain_hook){receiver, source->before, source->after, source->on_error};
    hooks[i] = (chttp_application_work_chain_hook){&records[i],
        source->before ? application_invocation_before : NULL, source->after ? application_invocation_after : NULL,
        source->on_error ? application_invocation_error : NULL};
  }
  return SALTS_OK;
}
static int application_upload_open(void *user, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_body_sink *sink) {
  application_worker_action *action = user;
  return chttp_application_work_upload_open(action->work, request, response, &action->task,
      &action->upload, action->upload_chunk_bytes, sink);
}
static void application_upload_close(void *user, chttp_body_sink *sink, int status) {
  (void)user; chttp_application_work_upload_close(sink->user, status);
}
static int application_upload_dispatch(void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  (void)user;
  return chttp_application_work_admission_response(response,
      chttp_application_work_upload_finish(request->body_sink_user, request, response));
}
static int application_websocket_open(void *user, chttp_websocket *websocket,
    const chttp_server_request_view *request, chttp_server_response *response) {
  application_worker_action *action = user;
  return chttp_application_work_admission_response(response, chttp_application_work_websocket_open(
      action->work, websocket, request, response, &action->task, &action->websocket));
}
static int application_worker_mount(chttp_application *owner,
    const chttp_application_worker_binding *binding, const chttp_application_upload_binding *upload,
    const chttp_application_websocket_binding *websocket) {
  application_impl *app = owner ? owner->impl : NULL;
  if (!app || !binding || !binding->invoke || !binding->path || !app->workers.impl ||
      binding->hook_count > CHTTP_APPLICATION_WORK_MAX_HOOKS ||
      (binding->hook_count && !binding->hooks)) return SALTS_EINVAL;
  if (app->configuration_closed) return SALTS_EBUSY;
  if (!cmeta_function_abi_contract_compatible(FunctionAbi(chttp_expected_invocation), binding->abi))
    return SALTS_EINVAL;
  if (upload && (!upload->open || !upload->close || !upload->cleanup ||
      !cmeta_function_abi_contract_compatible(FunctionAbi(chttp_expected_upload_open), upload->open_abi))) return SALTS_EINVAL;
  if (websocket && (!websocket->event || !websocket->event_capacity || binding->method != CHTTP_METHOD_GET ||
      !cmeta_function_abi_contract_compatible(FunctionAbi(chttp_expected_websocket_event), websocket->event_abi))) return SALTS_EINVAL;
  if (app->worker_action_count >= app->worker_route_capacity) return SALTS_ENOBUFS;
  void *receiver = NULL;
  int status = application_receiver(app, binding->component_id, binding->receiver, &receiver);
  if (status != SALTS_OK) return status;
  application_worker_action *action = &app->worker_actions[app->worker_action_count];
  *action = (application_worker_action){.work = &app->workers,
      .receiver = receiver, .invoke = binding->invoke, .completed = binding->after_completion,
      .task = {.run = application_invocation_run, .user = action,
               .after_completion = binding->after_completion ? application_invocation_completed : NULL,
               .hooks = action->hooks, .hook_count = binding->hook_count}};
  status = application_worker_hooks(app, binding->hooks, binding->hook_count, action->invocation_hooks, action->hooks);
  if (status != SALTS_OK) return status;
  if (websocket) {
    action->websocket = (chttp_application_websocket_callbacks){websocket->event, websocket->event_abi->function,
        receiver, websocket->event_capacity};
    const chttp_server_websocket_options route = {.size = sizeof(route), .path = binding->path,
        .max_frame_bytes = websocket->max_frame_bytes, .max_message_bytes = websocket->max_message_bytes,
        .max_buffered_input_bytes = websocket->max_buffered_input_bytes,
        .on_open = application_websocket_open, .user = action, .worker = true, .operation = binding->abi->function};
    status = chttp_server_websocket_with(&app->server, &route);
    if (status == SALTS_OK) ++app->worker_action_count;
    return status;
  }
  if (upload) {
    status = chttp_server_worker_body_chunk_bound(&app->server, &action->upload_chunk_bytes);
    if (status != SALTS_OK) return status;
    action->upload = (chttp_application_upload_callbacks){upload->open, upload->close, upload->cleanup, receiver};
  }
  const chttp_server_route_options route = {.method = binding->method, .path = binding->path,
      .handler = upload ? application_upload_dispatch : application_worker_dispatch, .user = action, .operation = binding->abi->function,
      .worker_body_open = upload ? application_upload_open : NULL,
      .worker_body_poll = upload ? chttp_application_work_upload_poll : NULL,
      .body_close = upload ? application_upload_close : NULL, .worker_body_chunk_bytes = action->upload_chunk_bytes};
  status = chttp_server_route_with(&app->server, &route);
  if (status == SALTS_OK) ++app->worker_action_count;
  return status;
}
int chttp_application_worker_mount(chttp_application *owner, const chttp_application_worker_binding *binding) {
  return application_worker_mount(owner, binding, NULL, NULL);
}
int chttp_application_worker_upload(chttp_application *owner, const chttp_application_upload_binding *binding) {
  return binding ? application_worker_mount(owner, &binding->controller, binding, NULL) : SALTS_EINVAL;
}
int chttp_application_worker_websocket(chttp_application *owner, const chttp_application_websocket_binding *binding) {
  return binding ? application_worker_mount(owner, &binding->controller, NULL, binding) : SALTS_EINVAL;
}

int chttp_application_asset_invoke(void *self, const chttp_invocation *invocation) {
  application_worker_assets *assets = self;
  chttp_web_asset_selection selected;
  int status = chttp_web_assets_select(&assets->mount, chttp_invocation_request(invocation), &selected);
  if (status != SALTS_OK) return status;
  switch (selected.kind) {
  case CHTTP_WEB_ASSET_NEXT: return SALTS_EPROTO; /* Matched before admission. */
  case CHTTP_WEB_ASSET_NOT_FOUND:
    return chttp_invocation_respond(invocation, 404u, "text/plain", "Not Found", 9u);
  case CHTTP_WEB_ASSET_METHOD_NOT_ALLOWED:
    status = chttp_invocation_set_header(invocation, "Allow", "GET, HEAD");
    return status == SALTS_OK ? chttp_invocation_respond(invocation, 405u, NULL, NULL, 0u) : status;
  case CHTTP_WEB_ASSET_FILE: break;
  }
  chttp_application_asset_policy_result policy = {0};
  if (assets->policy) {
    status = assets->policy(assets->receiver, selected.relative, &policy);
    if (status != SALTS_OK) return status;
  }
  const char *cache = assets->mount.cache_control ? assets->mount.cache_control : "no-cache";
  if (policy.immutable && assets->mount.immutable_cache_control) cache = assets->mount.immutable_cache_control;
  status = chttp_invocation_set_header(invocation, "Cache-Control", cache);
  if (status != SALTS_OK) return status;
  const chttp_server_file_options file = {selected.path, NULL, policy.etag};
  return chttp_invocation_serve_file(invocation, &file);
}
static int application_assets_run(void *user, const chttp_server_request_view *request,
    chttp_application_work_reply *reply) {
  const chttp_invocation_state state = chttp_invocation_state_make(request, reply, NULL);
  const chttp_invocation invocation = {&state};
  return chttp_application_asset_invoke(user, &invocation);
}
static void application_assets_completed(void *user, const chttp_server_request_view *request,
    const chttp_application_work_outcome *outcome) {
  const application_worker_assets *assets = user;
  assets->completed(assets->receiver, request, outcome);
}
static int application_assets_dispatch(void *user, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_server_next *next) {
  application_worker_assets *assets = user;
  if (!chttp_web_assets_matches(&assets->mount, request->path)) return chttp_server_next_call(next);
  chttp_server_request_view snapshot = *request;
  snapshot.operation = FunctionAbi(chttp_application_asset_invoke)->function;
  const int status = chttp_application_work_submit(assets->work, &snapshot, response, &assets->task);
  return chttp_application_work_admission_response(response, status);
}
int chttp_application_worker_assets(chttp_application *owner, const chttp_application_asset_binding *binding) {
  application_impl *app = owner ? owner->impl : NULL;
  if (!app || !binding || !app->workers.impl) return SALTS_EINVAL;
  if (app->configuration_closed) return SALTS_EBUSY;
  if (app->worker_assets) return SALTS_EALREADY;
  int status = chttp_web_assets_validate(binding->mount);
  if (status != SALTS_OK) return status;
  if (binding->mount->immutable || binding->mount->etag || binding->mount->user ||
      (binding->policy != NULL) != (binding->policy_abi != NULL)) return SALTS_EINVAL;
  if (binding->policy && !cmeta_function_abi_contract_compatible(
      FunctionAbi(chttp_expected_asset_policy), binding->policy_abi)) return SALTS_EINVAL;
  void *receiver = NULL;
  if (binding->policy || binding->after_completion || binding->component_id || binding->receiver) {
    status = application_receiver(app, binding->component_id, binding->receiver, &receiver);
    if (status != SALTS_OK) return status;
  }
  application_worker_assets *assets = calloc(1u, sizeof(*assets));
  if (!assets) return SALTS_ENOMEM;
  assets->work = &app->workers; assets->mount = *binding->mount;
  assets->receiver = receiver; assets->policy = binding->policy; assets->completed = binding->after_completion;
  status = application_worker_hooks(app, binding->hooks, binding->hook_count, assets->invocation_hooks, assets->hooks);
  if (status != SALTS_OK) { free(assets); return status; }
  assets->task = (chttp_application_work_task){.run = application_assets_run, .user = assets,
      .hooks = assets->hooks, .hook_count = binding->hook_count,
      .after_completion = binding->after_completion ? application_assets_completed : NULL};
  status = chttp_server_use(&app->server, application_assets_dispatch, assets);
  if (status != SALTS_OK) { free(assets); return status; }
  app->worker_assets = assets;
  return SALTS_OK;
}

static int application_worker_rpc_output(void *user, unsigned int code, const char *type,
    const void *body, size_t size) {
  return chttp_application_work_respond(user, code, type, body, size);
}
static int application_worker_rpc_run(void *user, const chttp_server_request_view *request,
    chttp_application_work_reply *reply) {
  application_impl *app = user;
  const chttp_method_completion_sink sink = chttp_application_work_method_sink(reply);
  return chttp_server_rpc_evaluate(&app->server, request, application_worker_rpc_output, reply, &sink);
}
static int application_worker_rpc_dispatch(void *user,
    const chttp_server_request_view *request, chttp_server_response *response) {
  application_impl *app = user;
  const chttp_application_work_task task = {.run = application_worker_rpc_run, .user = app,
      .method_capacity = app->worker_rpc_completion_capacity};
  const int status = chttp_application_work_submit(&app->workers, request, response, &task);
  return chttp_application_work_admission_response(response, status);
}
int chttp_application_worker_mount_rpc(chttp_application *owner, const char *component_id,
    const cmeta_data_desc *data, const chttp_rpc_method *method, DataBind *codec,
    const chttp_application_method_interceptor *hooks, size_t hook_count) {
  application_impl *app = owner ? owner->impl : NULL;
  if (!app || !method || !method->execution || !codec || !app->workers.impl) return SALTS_EINVAL;
  if (app->configuration_closed) return SALTS_EBUSY;
  if ((method->plugin_registry == NULL) != !cmeta_plugin_ref_valid(method->plugin_ref))
    return SALTS_EINVAL;
  if (app->worker_action_count >= app->worker_route_capacity) return SALTS_ENOBUFS;
  void *receiver = NULL;
  int status = application_receiver(app, component_id, data, &receiver);
  if (status != SALTS_OK) return status;
  chttp_method_hook admitted[CHTTP_METHOD_MAX_HOOKS];
  status = application_method_hooks(app, hooks, hook_count, admitted);
  if (status != SALTS_OK) return status;
  if (!app->worker_rpc_ready) {
    status = chttp_server_rpc_configure(&app->server, &app->worker_rpc_config);
    if (status != SALTS_OK) return status;
    status = chttp_server_rpc_set_dispatch_adapter(&app->server, application_worker_rpc_dispatch, app);
    if (status != SALTS_OK) return status;
    app->worker_rpc_ready = true;
  }
  application_worker_action *action = &app->worker_actions[app->worker_action_count];
  action->execution = *method->execution;
  if (!method->plugin_registry) action->execution.context = receiver;
  const chttp_server_rpc_mount_options mount = {sizeof(mount), method->path, method->plan,
      method->binding, &action->execution, method->plugin_registry, method->plugin_ref, admitted, hook_count, codec};
  status = chttp_server_rpc_mount(&app->server, &mount);
  if (status == SALTS_OK) {
    ++app->worker_action_count;
    if (chttp_method_hooks_have_completion(admitted, hook_count))
      app->worker_rpc_completion_capacity = app->worker_rpc_config.max_batch_items;
  }
  return status;
}

int chttp_application_worker_mount_http(chttp_application *owner, const char *component_id,
    const cmeta_data_desc *data, const chttp_http_method *method, DataBind *codec,
    const chttp_application_method_interceptor *hooks, size_t hook_count) {
  application_impl *app = owner ? owner->impl : NULL;
  if (!app || !method || !codec || !app->workers.impl) return SALTS_EINVAL;
  if (app->configuration_closed) return SALTS_EBUSY;
  const bool flow = method->dispatch == CHTTP_DISPATCH_CFLOW;
  const bool plugin = method->dispatch == CHTTP_DISPATCH_PLUGIN;
  if ((!flow && !plugin && method->dispatch != CHTTP_DISPATCH_INLINE) || method->executor)
    return SALTS_ENOTSUP;
  if (plugin) {
    if (method->binding || method->execution || method->projection || !method->plugin_registry ||
        !cmeta_plugin_ref_valid(method->plugin_ref) || !method->plugin_export_id ||
        !method->plugin_export_id[0]) return SALTS_EINVAL;
  } else if ((flow ? (!method->projection || method->execution) : (!method->execution || method->projection)) ||
      method->plugin_registry || cmeta_plugin_ref_valid(method->plugin_ref) || method->plugin_export_id)
    return SALTS_ENOTSUP;
  if (app->worker_action_count >= app->worker_route_capacity) return SALTS_ENOBUFS;
  void *receiver = NULL;
  int status = application_receiver(app, component_id, data, &receiver);
  if (status != SALTS_OK) return status;
  chttp_method_hook admitted[CHTTP_METHOD_MAX_HOOKS];
  status = application_method_hooks(app, hooks, hook_count, admitted);
  if (status != SALTS_OK) return status;
  if (!app->methods.impl) {
    const chttp_binding_limits *b = &app->worker_binding_limits;
    if (b->size != sizeof(*b)) return SALTS_EINVAL;
    const chttp_method_runtime_config limits = {sizeof(limits), b->method_capacity,
        b->max_binding_value_bytes, b->max_response_body_bytes, b->max_call_frame_bytes,
        b->native_workspace_bytes, b->native_max_depth, b->native_max_items, b->native_max_owned_bytes};
    status = chttp_method_runtime_init(&app->methods, &limits);
    if (status != SALTS_OK) return status;
  }
  application_worker_action *action = &app->worker_actions[app->worker_action_count];
  chttp_method_runtime_http_mount mount = CHTTP_METHOD_RUNTIME_HTTP_MOUNT_INIT;
  mount.method_plan = method->plan; mount.native_binding = method->binding;
  mount.hooks = admitted; mount.hook_count = hook_count;
  mount.output_codec = codec;
  if (plugin) {
    mount.execution_mode = CHTTP_METHOD_RUNTIME_EXECUTION_DEFERRED_PLUGIN;
    mount.plugin_registry = method->plugin_registry; mount.plugin_ref = method->plugin_ref;
    mount.plugin_export_id = method->plugin_export_id;
  } else if (flow) {
    mount.execution_mode = CHTTP_METHOD_RUNTIME_EXECUTION_DEFERRED_CFLOW;
    mount.cflow_projection = method->projection;
  } else {
    action->execution = *method->execution;
    action->execution.context = receiver;
    mount.execution = &action->execution;
  }
  status = chttp_method_runtime_mount_worker_http(&app->methods, &app->server, &mount, &app->workers);
  if (status == SALTS_OK) ++app->worker_action_count;
  return status;
}

typedef struct application_dispatch {
  chttp_server_next *next;
} application_dispatch;

static cmeta_status application_continue(void *context, const chttp_exchange *exchange,
    chttp_exchange_result *result) {
  const application_dispatch *dispatch = context;
  (void)exchange;
  result->status = chttp_server_next_call(dispatch->next);
  return result->status == SALTS_OK ? CMETA_OK : CMETA_CALLBACK_ERROR;
}

static int application_intercept(void *context, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_server_next *next) {
  application_impl *app = context;
  application_dispatch dispatch = {next};
  chttp_exchange_result result = {SALTS_OK, false};
  const chttp_exchange exchange = {request, response, request->operation, &result};
  const chttp_dispatch_chain chain = {&dispatch, application_continue, app->hooks, app->hook_count};
  const cmeta_status status = chttp_dispatch_chain_invoke(&chain, &exchange, &result);
  if (result.rejected) return result.status;
  return status == CMETA_OK ? result.status : result.status != SALTS_OK ? result.status : SALTS_EIO;
}

static int application_action_call(void *context, const chttp_server_request_view *request,
    chttp_server_response *response) {
  const application_action *record = context;
  chttp_exchange_result result = {SALTS_OK, false};
  const chttp_exchange exchange = {request, response, record->action->abi->function, &result};
  return record->action->invoke(record->receiver, &exchange);
}

static int application_body_open(void *context, const chttp_server_request_view *request,
    chttp_body_sink *sink) {
  const application_action *record = context;
  return record->action->body_open(record->receiver, request, sink);
}

static void application_body_close(void *context, chttp_body_sink *sink, int status) {
  const application_action *record = context;
  record->action->body_close(record->receiver, sink, status);
}

int chttp_exchange_reject(const chttp_exchange *exchange, unsigned int status_code,
    const char *content_type, const void *body, size_t body_size) {
  if (exchange == NULL || exchange->result == NULL || status_code < 400u || status_code > 599u)
    return SALTS_EINVAL;
  const int status = chttp_server_reply(exchange->response, status_code, content_type, body, body_size);
  if (status == SALTS_OK) {
    exchange->result->status = SALTS_OK;
    exchange->result->rejected = true;
  }
  return status;
}

static int application_bind(application_impl *app, const chttp_application_config *config) {
  size_t action_count = 0u, method_count = 0u, rpc_count = 0u;
  int status;
  for (size_t i = 0u; i < config->controller_count; ++i) {
    const chttp_controller *c = &config->controllers[i];
    if ((c->action_count != 0u && c->actions == NULL) ||
        (c->http_method_count != 0u && c->http_methods == NULL) ||
        (c->rpc_method_count != 0u && c->rpc_methods == NULL) ||
        (c->websocket_count != 0u && c->websockets == NULL)) return SALTS_EINVAL;
    if (c->action_count > SIZE_MAX - action_count || c->http_method_count > SIZE_MAX - method_count ||
        c->rpc_method_count > SIZE_MAX - rpc_count) return SALTS_ERANGE;
    action_count += c->action_count; method_count += c->http_method_count; rpc_count += c->rpc_method_count;
  }
  if (action_count > config->http.route_capacity || method_count > config->http.route_capacity ||
      rpc_count > config->rpc.method_capacity) return SALTS_ENOBUFS;
  if (action_count > SIZE_MAX / sizeof(*app->actions) || rpc_count > SIZE_MAX - method_count ||
      method_count + rpc_count > SIZE_MAX / sizeof(*app->executions)) return SALTS_ERANGE;
  if (action_count != 0u) {
    app->actions = calloc(action_count, sizeof(*app->actions));
    if (app->actions == NULL) return SALTS_ENOMEM;
  }
  if (method_count + rpc_count != 0u) {
    app->executions = calloc(method_count + rpc_count, sizeof(*app->executions));
    if (app->executions == NULL) return SALTS_ENOMEM;
  }
  if (method_count != 0u) {
    const chttp_binding_limits *b = &config->bindings;
    if (b->size != sizeof(*b) || method_count > b->method_capacity) return SALTS_EINVAL;
    const chttp_method_runtime_config limits = {sizeof(limits), b->method_capacity,
        b->max_binding_value_bytes, b->max_response_body_bytes, b->max_call_frame_bytes,
        b->native_workspace_bytes, b->native_max_depth, b->native_max_items, b->native_max_owned_bytes};
    status = chttp_method_runtime_init(&app->methods, &limits);
    if (status != SALTS_OK) return status;
  }
  if (rpc_count != 0u) {
    status = chttp_server_rpc_configure(&app->server, &config->rpc);
    if (status != SALTS_OK) return status;
  }
  for (size_t i = 0u; i < config->controller_count; ++i) {
    const chttp_controller *c = &config->controllers[i];
    void *receiver = NULL;
    status = application_receiver(app, c->component_id, c->receiver, &receiver);
    if (status != SALTS_OK) return status;
    for (size_t j = 0u; j < c->action_count; ++j) {
      const chttp_action *a = &c->actions[j];
      if (a->invoke == NULL || !cmeta_function_abi_contract_compatible(
          FunctionAbi(chttp_expected_action), a->abi)) return SALTS_EINVAL;
      application_action *record = &app->actions[app->action_count++];
      *record = (application_action){a, receiver};
      const chttp_server_route_options route = {.method = a->method, .path = a->path,
          .handler = application_action_call, .user = record,
          .body_open = a->body_open != NULL ? application_body_open : NULL,
          .body_close = a->body_close != NULL ? application_body_close : NULL,
          .operation = a->abi->function};
      status = chttp_server_route_with(&app->server, &route);
      if (status != SALTS_OK) return status;
    }
    for (size_t j = 0u; j < c->http_method_count; ++j) {
      const chttp_http_method *m = &c->http_methods[j];
      chttp_method_runtime_http_mount mount = CHTTP_METHOD_RUNTIME_HTTP_MOUNT_INIT;
      mount.method_plan = m->plan; mount.native_binding = m->binding;
      if (m->execution != NULL) {
        DataBindNativeExecution *execution = &app->executions[app->execution_count++];
        *execution = *m->execution; execution->context = receiver;
        mount.execution = execution;
      }
      mount.execution_mode = (chttp_method_runtime_execution_mode)m->dispatch;
      mount.executor = m->executor; mount.cflow_projection = m->projection;
      mount.plugin_registry = m->plugin_registry; mount.plugin_ref = m->plugin_ref;
      mount.plugin_export_id = m->plugin_export_id;
      status = chttp_method_runtime_mount_http(&app->methods, &app->server, &mount);
      if (status != SALTS_OK) return status;
    }
    for (size_t j = 0u; j < c->rpc_method_count; ++j) {
      const chttp_rpc_method *m = &c->rpc_methods[j];
      if (m->execution == NULL) return SALTS_EINVAL;
      DataBindNativeExecution *execution = &app->executions[app->execution_count++];
      *execution = *m->execution; execution->context = receiver;
      const chttp_server_rpc_mount_options mount = {sizeof(mount), m->path, m->plan,
          m->binding, execution, m->plugin_registry, m->plugin_ref, NULL, 0u, NULL};
      status = chttp_server_rpc_mount(&app->server, &mount);
      if (status != SALTS_OK) return status;
    }
    for (size_t j = 0u; j < c->websocket_count; ++j) {
      const chttp_websocket_endpoint *endpoint = &c->websockets[j];
      const chttp_server_websocket_options options = {.size = sizeof(options),
          .path = endpoint->path, .max_frame_bytes = endpoint->max_frame_bytes,
          .max_message_bytes = endpoint->max_message_bytes,
          .max_buffered_input_bytes = endpoint->max_buffered_input_bytes,
          .on_open = endpoint->on_open, .on_event = endpoint->on_event, .user = receiver};
      status = chttp_server_websocket_with(&app->server, &options);
      if (status != SALTS_OK) return status;
    }
  }
  return SALTS_OK;
}

int chttp_application_init(chttp_application *owner, const chttp_application_config *config) {
  application_impl *app;
  int status;
  if (owner == NULL || config == NULL || config->component_count == 0u || config->components == NULL ||
      config->controllers == NULL || config->controller_count == 0u || config->interceptor_count > 16u ||
      (config->interceptor_count != 0u && config->interceptors == NULL)) return SALTS_EINVAL;
  if (owner->impl != NULL) return SALTS_EALREADY;
  /* Sized policy storage must cover the snapshot before a full structure read. */
  if ((config->security != NULL && config->security->size < sizeof(*config->security)) ||
      (config->assets != NULL && config->assets->size < sizeof(*config->assets))) return SALTS_EINVAL;
  if (config->component_count > SIZE_MAX / sizeof(*app->instances) ||
      config->component_count > SIZE_MAX / sizeof(*app->order) ||
      config->dependency_capacity > SIZE_MAX / sizeof(*app->dependencies)) return SALTS_ERANGE;
  app = calloc(1u, sizeof(*app));
  if (app == NULL) return SALTS_ENOMEM;
  owner->impl = app;
  if (config->security != NULL) app->security = *config->security;
  if (config->cors != NULL) app->cors = *config->cors;
  if (config->assets != NULL) app->assets = *config->assets;
  app->worker_route_capacity = config->http.route_capacity;
  app->worker_binding_limits = config->bindings;
  app->worker_rpc_config = config->rpc;
  app->worker_prototype_allowed = config->interceptor_count == 0u && !config->admission &&
      !config->assets;
  for (size_t i = 0u; i < config->controller_count; ++i) {
    const chttp_controller *controller = &config->controllers[i];
    if (controller->action_count || controller->http_method_count || controller->rpc_method_count ||
        controller->websocket_count) app->worker_prototype_allowed = false;
  }
  app->component_count = config->component_count;
  app->instances = calloc(config->component_count, sizeof(*app->instances));
  app->order = calloc(config->component_count, sizeof(*app->order));
  if (config->dependency_capacity != 0u)
    app->dependencies = calloc(config->dependency_capacity, sizeof(*app->dependencies));
  if (app->instances == NULL || app->order == NULL ||
      (config->dependency_capacity != 0u && app->dependencies == NULL)) { status = SALTS_ENOMEM; goto failed; }
  status = application_graph_status(salts_configurator_context_init(&app->graph,
      config->components, config->component_count, config->selections, config->selection_count,
      app->instances, config->component_count, app->dependencies, config->dependency_capacity,
      app->order, config->component_count));
  if (status != SALTS_OK) goto failed;
  status = application_graph_status(salts_configurator_context_resolve(&app->graph));
  if (status != SALTS_OK) goto failed;
  status = application_graph_status(salts_configurator_context_start(&app->graph));
  if (status != SALTS_OK) goto failed;
  for (size_t i = 0u; i < config->interceptor_count; ++i) {
    const chttp_interceptor *hook = &config->interceptors[i];
    void *receiver = NULL;
    if (hook->before == NULL && hook->after == NULL && hook->on_error == NULL) { status = SALTS_EINVAL; goto failed; }
    status = application_receiver(app, hook->component_id, hook->receiver, &receiver);
    if (status != SALTS_OK) goto failed;
    app->hooks[app->hook_count++] = (chttp_dispatch_chain_hook){receiver, hook->before, hook->after, hook->on_error};
  }
  status = chttp_server_init(&app->server, &config->http);
  if (status != SALTS_OK) goto failed;
#define APP_CONFIGURE(pointer_, call_) do { if ((pointer_) != NULL) { status = (call_); if (status != SALTS_OK) goto failed; } } while (0)
  APP_CONFIGURE(config->execution, chttp_server_set_execution_options(&app->server, config->execution));
  APP_CONFIGURE(config->sockets, chttp_server_set_socket_options(&app->server, config->sockets));
  APP_CONFIGURE(config->placement, chttp_server_set_owner_placement(&app->server, config->placement));
  APP_CONFIGURE(config->websocket_transport, chttp_server_set_websocket_transport(&app->server, config->websocket_transport));
  status = chttp_server_set_deadlines(&app->server, &config->deadlines);
  if (status != SALTS_OK) goto failed;
  APP_CONFIGURE(config->jwt, chttp_server_use_jwt_bearer(&app->server, config->jwt));
  APP_CONFIGURE(config->admission, chttp_server_set_admission(&app->server, config->admission, config->admission_context));
  APP_CONFIGURE(config->security, chttp_web_security_use(&app->server, &app->security));
  APP_CONFIGURE(config->cors, chttp_server_use_cors(&app->server, &app->cors));
  /* Assets may terminate dispatch, so global application policy must wrap them. */
  status = chttp_server_use(&app->server, application_intercept, app);
  if (status != SALTS_OK) goto failed;
  APP_CONFIGURE(config->assets, chttp_web_assets_use(&app->server, &app->assets));
#undef APP_CONFIGURE
  status = application_bind(app, config);
  if (status == SALTS_OK) return SALTS_OK;
failed: {
    const int close_status = chttp_application_close(owner, 0u);
    return close_status != SALTS_OK && owner->impl != NULL ? close_status : status;
  }
}

int chttp_application_start(chttp_application *owner) {
  application_impl *app = owner != NULL ? owner->impl : NULL;
  if (!app) return SALTS_EINVAL;
  app->configuration_closed = true;
  return chttp_server_start(&app->server);
}
int chttp_application_port(const chttp_application *owner, uint16_t *port) {
  const application_impl *app = owner != NULL ? owner->impl : NULL;
  return app != NULL ? chttp_server_port(&app->server, port) : SALTS_EINVAL;
}
int chttp_application_stats(const chttp_application *owner, chttp_server_stats *stats) {
  const application_impl *app = owner != NULL ? owner->impl : NULL;
  return app != NULL ? chttp_server_get_stats(&app->server, stats) : SALTS_EINVAL;
}
int chttp_application_close(chttp_application *owner, uint32_t timeout_ms) {
  application_impl *app = owner != NULL ? owner->impl : NULL;
  const uint64_t started = monotonic_ms();
  int status;
  if (owner == NULL) return SALTS_EINVAL;
  if (app == NULL) return SALTS_OK;
  if (chttp_application_work_is_current(&app->workers)) return SALTS_EBUSY;
  if (app->server.impl != NULL) {
    status = chttp_server_stop(&app->server, timeout_ms);
    if (status != SALTS_OK) {
      chttp_server_stats stats = {0};
      if (chttp_server_get_stats(&app->server, &stats) != SALTS_OK || stats.running || stats.stopping)
        return status;
      /* Stop may exhaust this caller's budget just before the owner publishes
       * normal completion. That wait timeout retains ownership for a later
       * close; it is not an operational terminal to latch across calls. */
      if (status == SALTS_ETIMEDOUT && stats.terminal_status == SALTS_OK)
        return status;
      if (app->shutdown_status == SALTS_OK) app->shutdown_status = status;
    }
  }
  if (app->workers.impl) {
    const uint64_t elapsed = monotonic_ms() - started;
    if (timeout_ms != 0u && elapsed >= timeout_ms) return SALTS_ETIMEDOUT;
    const uint32_t remaining = timeout_ms == 0u ? 0u : (uint32_t)(timeout_ms - elapsed);
    status = chttp_application_work_close(&app->workers, remaining);
    if (status != SALTS_OK) {
      if (app->workers.impl) return status;
      if (app->shutdown_status == SALTS_OK) app->shutdown_status = status;
    }
  }
  if (app->server.impl != NULL) {
    /* Accepted DTO task finalizers still borrow Beans after response terminal. */
    while ((status = chttp_method_runtime_quiescent(&app->methods)) == SALTS_EBUSY) {
      if (timeout_ms != 0u && monotonic_ms() - started >= timeout_ms) return SALTS_ETIMEDOUT;
      sleep_ms(1u);
    }
    if (status != SALTS_OK) return status;
    status = chttp_server_destroy(&app->server);
    if (status != SALTS_OK) return status;
  }
  status = chttp_method_runtime_destroy(&app->methods);
  if (status != SALTS_OK) return status;
  if (app->graph.state == SALTS_CONFIGURATOR_CONTEXT_ACTIVE) {
    status = application_graph_status(salts_configurator_context_stop(&app->graph));
    if (status != SALTS_OK) return status;
  }
  if (app->graph.state == SALTS_CONFIGURATOR_CONTEXT_FAILED && app->graph.instances != NULL)
    return SALTS_EBUSY;
  const int result = app->shutdown_status;
  free(app->worker_assets);
  free(app->worker_actions);
  free(app->actions); free(app->executions); free(app->dependencies); free(app->instances); free(app->order);
  free(app); owner->impl = NULL;
  return result;
}

int chttp_application_with(chttp_application *owner, const chttp_application_config *config,
    chttp_application_body_fn body, void *context, chttp_application_outcome *out) {
  if (owner == NULL || body == NULL || out == NULL) return SALTS_EINVAL;
  *out = (chttp_application_outcome){0};
  if (owner->impl != NULL) { out->operation_status = SALTS_EALREADY; return SALTS_EALREADY; }
  out->operation_status = chttp_application_init(owner, config);
  if (out->operation_status == SALTS_OK) out->operation_status = chttp_application_start(owner);
  if (out->operation_status == SALTS_OK) out->operation_status = body(owner, context);
  out->close_status = chttp_application_close(owner, 0u);
  return out->close_status != SALTS_OK ? out->close_status : out->operation_status;
}
