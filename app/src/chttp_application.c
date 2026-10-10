#include <chttp_app/application.h>
#include <stdlib.h>
#include <string.h>

enum { APPLICATION_MAX_METHODS = 1024, APPLICATION_MAX_POLICIES = 64,
       APPLICATION_MAX_COMPONENTS = 128, APPLICATION_MAX_DEPENDENCIES = 4096 };

typedef struct application_method {
  DataBindNativeTypeBinding request, response;
  DataBindServiceNativeBinding native;
  DataBindHttpMethodPlan *plan;
  DataBindNativeExecution execution;
  size_t component_index;
} application_method;

typedef struct application_components {
  salts_component_context context;
  salts_component_deployment deployments[APPLICATION_MAX_COMPONENTS];
  salts_component_instance instances[APPLICATION_MAX_COMPONENTS];
  size_t activation_order[APPLICATION_MAX_COMPONENTS];
  salts_component_dependency dependencies[APPLICATION_MAX_DEPENDENCIES];
  salts_component_selection selections[APPLICATION_MAX_DEPENDENCIES];
} application_components;

typedef struct application_state {
  chttp_server server;
  chttp_service service;
  DataBind *codec;
  salts_component_context *components;
  application_components *owned_components;
  size_t method_count;
  application_method *methods;
} application_state;

chttp_application_options chttp_application_options_default(void) {
  chttp_application_options options = {0};
  options.size = sizeof(options);
  options.service = (chttp_service_config)CHTTP_SERVICE_CONFIG_INIT;
  options.server = (chttp_server_config){
      .host = "127.0.0.1", .backlog = 8u,
      .network = {
#if defined(_WIN32)
          .backend = NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
          .backend = NATIVE_IO_BACKEND_EPOLL,
#else
          .backend = NATIVE_IO_BACKEND_KQUEUE,
#endif
          .connection_capacity = 8u, .command_capacity = 32u,
          .request_capacity = 16u, .completion_batch_capacity = 8u,
          .event_capacity = 32u, .max_send_bytes = 65536u,
          .receive_buffer_bytes = 4096u, .connect_timeout_ms = 5000u,
          .read_timeout_ms = 5000u, .write_timeout_ms = 5000u},
      .middleware_capacity = 2u, .max_route_middleware_count = 2u,
      .max_route_param_count = 8u, .max_route_param_bytes = 1024u,
      .max_target_bytes = 4096u, .max_header_count = 32u,
      .max_header_bytes = 8192u, .max_request_body_bytes = 65536u,
      .max_response_header_count = 32u, .max_response_header_bytes = 8192u,
      .max_response_body_bytes = 4096u,
      .max_buffered_response_body_bytes = 4096u, .poll_slice_ms = 2u};
  return options;
}

static cmeta_status application_no_store(void *context,
    const chttp_service_call *call, bool *proceed) {
  (void)context;
  *proceed = true;
  return chttp_server_response_set_header(call->response, "Cache-Control", "no-store") == SALTS_OK
      ? CMETA_OK : CMETA_INVALID_ARGUMENT;
}

typedef struct application_selection {
  const chttp_application_definition *definition;
  const chttp_application_options *options;
  const chttp_application_operation *operation;
} application_selection;

static int application_select(void *context, const DataBindBindingPlan *binding,
    const DataBindServiceNativeBinding *native, chttp_service_interceptor_hook *hooks,
    size_t capacity, size_t *count) {
  const application_selection *selection = context;
  (void)binding;
  (void)native; /* The mount has already admitted the exact native capability. */
  *count = 0u;
  for (size_t i = 0u; i <= CHTTP_SERVICE_MAX_INTERCEPTORS; ++i) {
    const char *name = selection->definition->policy_at(selection->operation->service,
        selection->operation->operation, i);
    if (name == NULL) return SALTS_OK;
    if (i >= capacity || i == CHTTP_SERVICE_MAX_INTERCEPTORS) return SALTS_ENOBUFS;
    if (strcmp(name, "no_store") == 0) {
      hooks[i] = (chttp_service_interceptor_hook){NULL, application_no_store, NULL, NULL};
    } else {
      size_t j = 0u;
      for (; j < selection->options->policy_count; ++j)
        if (strcmp(selection->options->policies[j].name, name) == 0) break;
      if (j == selection->options->policy_count) return SALTS_ENOENT;
      hooks[i] = selection->options->policies[j].hook;
    }
    ++*count;
  }
  return SALTS_ENOBUFS;
}

/* Called only once the server has released all routes and callbacks. */
static void application_release(application_state *state) {
  free(state->owned_components);
  free(state->methods);
  free(state);
}

static int application_component_status(const chttp_application_options *options,
    salts_component_context *context, salts_component_status status) {
  if (options->component_diagnostic != NULL) {
    options->component_diagnostic->status = status;
    options->component_diagnostic->failure = context->failure;
  }
  switch (status) {
    case SALTS_COMPONENT_OK: return SALTS_OK;
    case SALTS_COMPONENT_CAPACITY_EXCEEDED: return SALTS_ENOBUFS;
    case SALTS_COMPONENT_MISSING_PROVIDER: return SALTS_ENOENT;
    default: return SALTS_EINVAL;
  }
}

static int application_components_prepare(application_state *state,
    const chttp_application_definition *definition, const chttp_application_options *options) {
  size_t count = options->provider_count;
  int automatic = count != 0u || options->selection_count != 0u;
  for (size_t i = 0u; i < state->method_count; ++i) {
    state->methods[i].component_index = SALTS_COMPONENT_INDEX_NONE;
    if (definition->operations[i].component != NULL) automatic = 1;
  }
  if (automatic && options->components != NULL) return SALTS_EINVAL;
  if (automatic) {
    application_components *graph = calloc(1u, sizeof(*graph));
    if (graph == NULL) return SALTS_ENOMEM;
    state->owned_components = graph;
    state->components = &graph->context;
    for (size_t i = 0u; i < count; ++i) graph->deployments[i] = options->providers[i];
    for (size_t i = 0u; i < options->selection_count; ++i) graph->selections[i] = options->selections[i];
    for (size_t i = 0u; i < state->method_count; ++i) {
      const chttp_application_operation *operation = &definition->operations[i];
      if (operation->component == NULL) continue;
      if (operation->bind_execution == NULL) return SALTS_EINVAL;
      const salts_component_provider_binding *provider = operation->component();
      if (!salts_component_provider_binding_valid(provider)) return SALTS_EINVAL;
      size_t index = options->provider_count;
      /* Multiple methods of a service share its one generated consumer.
       * Compare stable identity; descriptor addresses are not type identity. */
      for (; index < count; ++index)
        if (strcmp(provider->component->stable_id,
            graph->deployments[index].provider->component->stable_id) == 0) break;
      if (index == count) {
        if (count == APPLICATION_MAX_COMPONENTS) return SALTS_ENOBUFS;
        graph->deployments[count++] = (salts_component_deployment){provider, NULL, NULL};
      } else if (provider->create != graph->deployments[index].provider->create) {
        /* Equal names cannot authorize a different factory to replace a live consumer. */
        return SALTS_EINVAL;
      }
      state->methods[i].component_index = index;
    }
    salts_component_status status = salts_component_context_init(&graph->context,
        graph->deployments, count, graph->selections, options->selection_count,
        graph->instances, APPLICATION_MAX_COMPONENTS, graph->dependencies, APPLICATION_MAX_DEPENDENCIES,
        graph->activation_order, APPLICATION_MAX_COMPONENTS);
    int result = application_component_status(options, &graph->context, status);
    if (result != SALTS_OK) return result;
  } else {
    state->components = options->components;
  }
  if (state->components == NULL) return SALTS_OK;
  int result = application_component_status(options, state->components,
      salts_component_context_resolve(state->components));
  if (result != SALTS_OK) return result;
  return application_component_status(options, state->components,
      salts_component_context_start(state->components));
}

static int application_bind_status(DataBindStatus status) {
  switch (status) {
    case DATA_BIND_OK: return SALTS_OK;
    case DATA_BIND_ERR_OOM: return SALTS_ENOMEM;
    case DATA_BIND_ERR_LIMIT: case DATA_BIND_ERR_BUFFER_TOO_SMALL: return SALTS_ENOBUFS;
    case DATA_BIND_ERR_IO: case DATA_BIND_ERR_RUNTIME: return SALTS_EIO;
    default: return SALTS_EINVAL;
  }
}

int chttp_application_close(chttp_application *app, uint32_t timeout_ms) {
  if (app == NULL) return SALTS_EINVAL;
  application_state *state = app->impl;
  if (state == NULL) return SALTS_OK;
  int status = SALTS_OK;
  if (state->server.impl != NULL) {
    status = chttp_server_stop(&state->server, timeout_ms);
    if (status == SALTS_ETIMEDOUT || status == SALTS_EBUSY) return status;
    /* Operational stop errors may accompany a fully joined server. Destroy is
     * the authoritative gate; on EBUSY keep every borrowed provider alive. */
    int destroyed = chttp_server_destroy(&state->server);
    if (destroyed != SALTS_OK) return status != SALTS_OK ? status : destroyed;
  }
  int result = chttp_service_destroy(&state->service);
  if (result != SALTS_OK) return result;
  /* Release plans before provider shutdown, while the descriptor domain lives. */
  for (size_t i = 0u; i < state->method_count; ++i) {
    data_bind_http_method_plan_free(state->methods[i].plan);
    state->methods[i].plan = NULL;
  }
  data_bind_free(state->codec);
  state->codec = NULL;
  if (state->components != NULL &&
      state->components->state == SALTS_COMPONENT_CONTEXT_ACTIVE) {
    if (salts_component_context_stop(state->components) != SALTS_COMPONENT_OK)
      return SALTS_EIO;
  }
  application_release(state);
  app->impl = NULL;
  return status;
}

int chttp_application_init(chttp_application *app,
    const chttp_application_definition *definition, const chttp_application_options *options) {
  if (app == NULL || app->impl != NULL || definition == NULL ||
      definition->size != sizeof(*definition) || definition->codec == NULL ||
      definition->operations == NULL || definition->operation_count == 0u ||
      definition->operation_count > APPLICATION_MAX_METHODS || definition->http == NULL ||
      definition->policy_at == NULL || options == NULL || options->size != sizeof(*options) ||
      options->policy_count > APPLICATION_MAX_POLICIES ||
      options->provider_count > APPLICATION_MAX_COMPONENTS ||
      options->selection_count > APPLICATION_MAX_DEPENDENCIES ||
      (options->provider_count != 0u && options->providers == NULL) ||
      (options->selection_count != 0u && options->selections == NULL) ||
      (options->policy_count != 0u && options->policies == NULL) ||
      (options->components != NULL && options->components->state != SALTS_COMPONENT_CONTEXT_READY))
    return SALTS_EINVAL;
  if (options->component_diagnostic != NULL)
    *options->component_diagnostic = (chttp_application_component_diagnostic){0};
  for (size_t i = 0u; i < options->policy_count; ++i) {
    const chttp_application_policy *policy = &options->policies[i];
    if (policy->name == NULL || policy->name[0] == '\0' || strcmp(policy->name, "no_store") == 0 ||
        (policy->hook.before == NULL && policy->hook.after == NULL && policy->hook.on_error == NULL))
      return SALTS_EINVAL;
    for (size_t j = 0u; j < i; ++j)
      if (strcmp(options->policies[j].name, policy->name) == 0) return SALTS_EINVAL;
  }
  application_state *state = calloc(1u, sizeof(*state));
  if (state == NULL) return SALTS_ENOMEM;
  state->methods = calloc(definition->operation_count, sizeof(*state->methods));
  if (state->methods == NULL) { free(state); return SALTS_ENOMEM; }
  state->method_count = definition->operation_count;
  app->impl = state;
  DataBindError error = DATA_BIND_ERROR_INIT;
  int status = application_bind_status(definition->codec(&state->codec, &error));
  if (status != SALTS_OK) goto fail;
  chttp_server_config server = options->server;
  chttp_service_config service = options->service;
  if (server.route_capacity == 0u) server.route_capacity = state->method_count;
  if (service.method_capacity == 0u) service.method_capacity = state->method_count;
  status = chttp_server_init(&state->server, &server);
  if (status != SALTS_OK) goto fail;
  status = chttp_service_init(&state->service, &service);
  if (status != SALTS_OK) goto fail;
  status = application_components_prepare(state, definition, options);
  if (status != SALTS_OK) goto fail;
  for (size_t i = 0u; i < state->method_count; ++i) {
    application_method *record = &state->methods[i];
    const chttp_application_operation *operation = &definition->operations[i];
    status = SALTS_EINVAL;
    if (operation->service == NULL || operation->operation == NULL || operation->binding == NULL ||
        (operation->execution == NULL && operation->bind_execution == NULL) || operation->request == NULL) goto fail;
    const DataBindHttpProjectionConfig *projection = data_bind_http_projection_artifact_find(
        definition->http, operation->service, operation->operation);
    if (projection == NULL) goto fail;
    record->request = (DataBindNativeTypeBinding)DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    record->response = (DataBindNativeTypeBinding)DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    record->native = (DataBindServiceNativeBinding)DATA_BIND_SERVICE_NATIVE_BINDING_INIT(NULL, NULL, NULL);
    status = application_bind_status(operation->binding(&record->request, &record->response, &record->native, &error));
    if (status != SALTS_OK) goto fail;
    DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    status = application_bind_status(data_bind_http_method_plan_compile_service(state->codec,
        operation->service, operation->operation, projection, &record->native, &record->plan, &diagnostic));
    if (status != SALTS_OK) goto fail;
    const DataBindBindingPlan *binding = data_bind_http_method_plan_binding(record->plan);
    int body = 0;
    for (size_t j = 0u; j < data_bind_binding_plan_ingress_count(binding); ++j) {
      DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
      if (!data_bind_binding_plan_ingress_at(binding, j, &entry) || entry.address.space == NULL) {
        status = SALTS_EINVAL; goto fail;
      }
      if (strcmp(entry.address.space, "http.body") == 0) body = 1;
    }
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    mount.method_plan = record->plan;
    mount.native_binding = &record->native;
    if (operation->bind_execution != NULL) {
      const cmeta_object_ref *instance = record->component_index != SALTS_COMPONENT_INDEX_NONE
          ? &state->components->instances[record->component_index].object : NULL;
      cmeta_status bound = operation->bind_execution(instance, &record->execution);
      if (bound != CMETA_OK) {
        status = bound == CMETA_OUT_OF_MEMORY ? SALTS_ENOMEM : SALTS_EINVAL;
        goto fail;
      }
      mount.execution = &record->execution;
    } else mount.execution = operation->execution();
    application_selection selection = {definition, options, operation};
    if (body) {
      const DataBindMessagePlan *message = NULL;
      status = application_bind_status(data_bind_message_plan_acquire_generated(
          state->codec, operation->request(), &message, &error));
      if (status != SALTS_OK) goto fail;
      status = chttp_service_mount_http_document_body(&state->service, &state->server,
          &mount, message, application_select, &selection);
    } else {
      status = chttp_service_mount_http_document(&state->service, &state->server,
          &mount, application_select, &selection);
    }
    if (status != SALTS_OK) goto fail;
  }
  return SALTS_OK;
fail:
  /* Nothing has listened: no request or worker can retain these resources. */
  {
    int cleanup = chttp_application_close(app, 0u);
    if (cleanup != SALTS_OK) return cleanup;
  }
  return status;
}

int chttp_application_start(chttp_application *app) {
  application_state *state = app != NULL ? app->impl : NULL;
  return state != NULL ? chttp_server_start(&state->server) : SALTS_EINVAL;
}

uint16_t chttp_application_port(const chttp_application *app) {
  const application_state *state = app != NULL ? app->impl : NULL;
  uint16_t port = 0u;
  if (state != NULL && chttp_server_port(&state->server, &port) != SALTS_OK) return 0u;
  return port;
}

int chttp_application_get_stats(const chttp_application *app, chttp_server_stats *stats) {
  const application_state *state = app != NULL ? app->impl : NULL;
  return state != NULL ? chttp_server_get_stats(&state->server, stats) : SALTS_EINVAL;
}
