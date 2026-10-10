#include "http_example_app.h"
#include "http_example.http.h"
#include "http_example.service_native.h"
#include "http_example_templates.h"
#include <salts/clock.h>
#include <salts/thread.h>

#include <string.h>

static native_io_backend_kind example_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static crpc_server_config example_config(void) {
  crpc_server_config config = {0};
  config.http.host = "127.0.0.1";
  config.http.backlog = 8u;
  config.http.network.backend = example_backend();
  config.http.network.connection_capacity = 8u;
  config.http.network.command_capacity = 32u;
  config.http.network.request_capacity = 16u;
  config.http.network.completion_batch_capacity = 8u;
  config.http.network.event_capacity = 32u;
  config.http.network.max_send_bytes = 64u * 1024u;
  config.http.network.receive_buffer_bytes = 4096u;
  config.http.network.connect_timeout_ms = HTTP_EXAMPLE_TIMEOUT_MS;
  config.http.network.read_timeout_ms = HTTP_EXAMPLE_TIMEOUT_MS;
  config.http.network.write_timeout_ms = HTTP_EXAMPLE_TIMEOUT_MS;
  config.http.route_capacity = 4u;
  config.http.middleware_capacity = 4u;
  config.http.max_route_middleware_count = 4u;
  config.http.max_route_param_count = 4u;
  config.http.max_route_param_bytes = 128u;
  config.http.max_target_bytes = 256u;
  config.http.max_header_count = 16u;
  config.http.max_header_bytes = 4096u;
  config.http.max_request_body_bytes = HTTP_EXAMPLE_BODY_BYTES;
  config.http.max_response_header_count = 16u;
  config.http.max_response_header_bytes = 4096u;
  config.http.max_response_body_bytes = HTTP_EXAMPLE_BODY_BYTES;
  config.http.poll_slice_ms = 2u;
  config.http.max_buffered_response_body_bytes = HTTP_EXAMPLE_BODY_BYTES;
  config.method_capacity = 8u;
  config.max_method_bytes = 64u;
  config.max_json_depth = 8u;
  config.max_batch_items = 4u;
  return config;
}

http_example_config http_example_config_default(void) {
  const http_example_config config = {
      .title = "CHttp server example",
      .shutdown_timeout_ms = HTTP_EXAMPLE_TIMEOUT_MS,
      .deadlines = {.headers_ms = HTTP_EXAMPLE_TIMEOUT_MS, .body_ms = HTTP_EXAMPLE_TIMEOUT_MS,
          .handler_ms = HTTP_EXAMPLE_TIMEOUT_MS},
      .rate = {.scope = CHTTP_RATE_LIMIT_PEER_IP, .group_capacity = 64u,
          .burst = 100u, .refill_tokens = 50u, .refill_period_ms = 1000u}};
  return config;
}

static int example_plugin_status(cmeta_plugin_status status) {
  switch (status) {
    case CMETA_PLUGIN_OK: return SALTS_OK;
    case CMETA_PLUGIN_ALLOCATION_FAILED: return SALTS_ENOMEM;
    case CMETA_PLUGIN_CAPACITY_EXCEEDED: return SALTS_ENOBUFS;
    case CMETA_PLUGIN_BUSY: return SALTS_EBUSY;
    case CMETA_PLUGIN_LOAD_FAILED:
    case CMETA_PLUGIN_UNLOAD_FAILED: return SALTS_EIO;
    default: return SALTS_EINVAL;
  }
}

static int example_plugin_close(http_example_app *app, uint64_t started) {
  if (cmeta_plugin_lease_valid(app->plugin_client.lease)) {
    const int status = example_plugin_status(
        databind_plugin_client_11_HttpExample_16_CalculatorPlugin_close(&app->plugin_client));
    if (status != SALTS_OK) return status;
  }
  if (cmeta_plugin_ref_valid(app->plugin_ref)) {
    cmeta_plugin_lifecycle_info lifecycle = {0};
    int status = example_plugin_status(cmeta_plugin_registry_get_lifecycle(
        &app->plugins, app->plugin_ref, &lifecycle));
    if (status != SALTS_OK) return status;
    if (lifecycle.state == CMETA_PLUGIN_LIFECYCLE_STARTED) {
      status = example_plugin_status(cmeta_plugin_registry_request_stop(&app->plugins, app->plugin_ref));
      if (status != SALTS_OK) return status;
    }
    /* A load whose start failed can be unloaded directly. */
    if (lifecycle.state != CMETA_PLUGIN_LIFECYCLE_LOADED) {
      bool quiet = false;
      do {
        status = example_plugin_status(cmeta_plugin_registry_poll_quiescent(
            &app->plugins, app->plugin_ref, &quiet));
        if (status != SALTS_OK) return status;
        if (quiet) break;
        if (cmeta_monotonic_ms() - started >= app->shutdown_timeout_ms) return SALTS_ETIMEDOUT;
        cmeta_sleep_ms(1u);
      } while (!quiet);
    }
    status = example_plugin_status(cmeta_plugin_registry_unload(&app->plugins, app->plugin_ref));
    if (status != SALTS_OK) return status;
    app->plugin_ref = (cmeta_plugin_ref){0};
  }
  return app->plugins.impl != NULL
      ? example_plugin_status(cmeta_plugin_registry_destroy(&app->plugins)) : SALTS_OK;
}

int http_example_close(http_example_app *app) {
  if (app == NULL) return SALTS_EINVAL;
  const uint64_t started = cmeta_monotonic_ms();
  /* Control-thread only: stop admission before observing executor quiescence. */
  if (app->rpc.impl != NULL) {
    const int status = crpc_server_stop(&app->rpc, app->shutdown_timeout_ms);
    if (status != SALTS_OK) {
      chttp_server_stats stats = {0};
      if (chttp_server_get_stats(crpc_server_http(&app->rpc), &stats) != SALTS_OK ||
          stats.running || stats.stopping)
        return status;
      /* A stopped runtime retains its first operational error. That error
       * must be reported, but must not prevent release of the stopped owner. */
      if (app->shutdown_status == SALTS_OK) app->shutdown_status = status;
    }
  }
  if (cflow_executor_valid(&app->executor)) {
    if (!cflow_executor_shutdown(&app->executor)) return SALTS_EBUSY;
    while (cflow_executor_pending(&app->executor) != 0u) {
      if (cmeta_monotonic_ms() - started >= app->shutdown_timeout_ms) return SALTS_ETIMEDOUT;
      cmeta_sleep_ms(1u);
    }
    if (!cflow_executor_wait_idle(&app->executor)) return SALTS_EBUSY;
  }
  if (app->rpc.impl != NULL) {
    const int status = crpc_server_destroy(&app->rpc);
    if (status != SALTS_OK) return status;
  }
  if (app->service.impl != NULL) {
    const int status = chttp_service_destroy(&app->service);
    if (status != SALTS_OK) return status;
  }
  if (cflow_executor_valid(&app->executor)) cflow_executor_destroy(&app->executor);
  const int plugin_status = example_plugin_close(app, started);
  if (plugin_status != SALTS_OK) return plugin_status;
  chttp_web_renderer_destroy(&app->renderer);
  chttp_rate_limiter_destroy(&app->limiter);
  data_bind_http_method_plan_free(app->method_plan);
  data_bind_free(app->contract);
  HomePage_clear(&app->page);
  tstr_free(app->file_path);
  const int shutdown_status = app->shutdown_status;
  *app = (http_example_app){0};
  return shutdown_status;
}

static int example_sample(http_example_app *app, const http_example_config *config) {
  const SumRequest_t request = {.left = 3u, .right = 4u};
  SumResponse_t response = {0};
  int status;
  if (config->plugin_path == NULL) {
    status = databind_11_HttpExample_10_Calculator_3_Add(&request, &response);
  } else {
    const cmeta_plugin_registry_config registry_config = {.capacity = 1u};
    status = example_plugin_status(cmeta_plugin_registry_init(&app->plugins, &registry_config));
    if (status == SALTS_OK)
      status = example_plugin_status(cmeta_plugin_registry_load(&app->plugins, config->plugin_path, &app->plugin_ref));
    if (status == SALTS_OK)
      status = example_plugin_status(cmeta_plugin_registry_start(&app->plugins, app->plugin_ref));
    if (status == SALTS_OK)
      status = example_plugin_status(databind_plugin_client_11_HttpExample_16_CalculatorPlugin_open(
          &app->plugins, app->plugin_ref, &app->plugin_client));
    if (status != SALTS_OK) return status;
    int native_status = SALTS_EIO;
    status = example_plugin_status(databind_11_HttpExample_10_Calculator_3_Add_plugin_client_call(
        &app->plugin_client, &request, &response, &native_status));
    if (status != SALTS_OK) return status;
    if (native_status != SALTS_OK) return native_status;
    status = example_plugin_status(
        databind_plugin_client_11_HttpExample_16_CalculatorPlugin_close(&app->plugin_client));
    if (status != SALTS_OK) return status;
    if (!cflow_executor_worker_init_with_capacity(&app->executor,
            HTTP_EXAMPLE_PLUGIN_WORKERS, HTTP_EXAMPLE_PLUGIN_QUEUE_CAPACITY))
      return SALTS_ENOMEM;
  }
  if (status == SALTS_OK) app->page.example_sum = response.sum;
  return status;
}

static int example_prepare_contract(http_example_app *app) {
  DataBindError error = DATA_BIND_ERROR_INIT;
  DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  if (HttpExample_codec_create(&app->contract, &error) != DATA_BIND_OK)
    return SALTS_EIO;
  if (databind_11_HttpExample_10_Calculator_3_Add__databind_native_binding(
          &app->request_native, &app->response_native, &app->native, &error) != DATA_BIND_OK)
    return SALTS_EIO;
  const DataBindHttpProjectionConfig *projection = data_bind_http_projection_artifact_find(
      &databind_http_example_http_projection, "Calculator", "Add");
  if (projection == NULL) return SALTS_EINVAL;
  if (data_bind_http_method_plan_compile_service(app->contract, "Calculator", "Add",
          projection, &app->native, &app->method_plan, &diagnostic) != DATA_BIND_OK)
    return SALTS_EINVAL;
  if (HomePage_cmeta_data(&app->page_desc, &error) != DATA_BIND_OK)
    return SALTS_EINVAL;
  return SALTS_OK;
}

static int example_prepare(http_example_app *app, const http_example_config *config) {
  static const char *const origins[] = {"http://localhost:3000"};
  /* CORS borrows the policy itself as well as its strings through destruction. */
  static const chttp_server_cors_options cors = {.origins = origins, .origin_count = 1u,
      .methods = "GET, HEAD, POST", .allowed_headers = "Content-Type, Authorization",
      .exposed_headers = "ETag, Content-Range", .max_age_seconds = 600u};
  chttp_service_config service_config = CHTTP_SERVICE_CONFIG_INIT;
  chttp_web_renderer_config render_config = CHTTP_WEB_RENDERER_CONFIG_INIT;
  chttp_web_error web_error = CHTTP_WEB_ERROR_INIT;
  crpc_server_config server_config = example_config();
  app->shutdown_timeout_ms = config->shutdown_timeout_ms;
  HomePage_init(&app->page);
  app->page.title = tstr_from_v(vstr_from_cstr(config->title));
  if (app->page.title == NULL) return SALTS_ENOMEM;
  if (config->file_path != NULL) {
    app->file_path = tstr_from_v(vstr_from_cstr(config->file_path));
    if (app->file_path == NULL) return SALTS_ENOMEM;
    app->page.file_enabled = true;
  }
  int status = example_sample(app, config);
  if (status != SALTS_OK) return status;
  status = example_prepare_contract(app);
  if (status != SALTS_OK) return status;
  render_config.max_output_bytes = HTTP_EXAMPLE_BODY_BYTES;
  if (chttp_web_renderer_init(&app->renderer, example_templates, 2u,
          &render_config, &web_error) != CHTTP_WEB_OK)
    return SALTS_EIO;
  service_config.method_capacity = 1u;
  status = chttp_service_init(&app->service, &service_config);
  if (status != SALTS_OK) return status;
  server_config.http.port = config->port;
  /* One owner serializes both the non-reentrant renderer and peer-IP limiter. */
  status = crpc_server_init(&app->rpc, &server_config);
  if (status != SALTS_OK) return status;
  chttp_server *server = crpc_server_http(&app->rpc);
  status = chttp_rate_limiter_init(&app->limiter, &config->rate);
  if (status == SALTS_OK)
    status = chttp_server_set_admission(server, chttp_rate_limiter_admit, &app->limiter);
  if (status == SALTS_OK) status = chttp_server_set_deadlines(server, &config->deadlines);
  if (status == SALTS_OK) status = chttp_server_use_cors(server, &cors);
  if (status == SALTS_OK) status = chttp_server_use(server, http_example_intercept, NULL);
  if (status == SALTS_OK) status = http_example_register_handlers(app);
  if (status != SALTS_OK) return status;
  const chttp_server_middleware policy = {http_example_no_store, NULL};
  chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
  mount.method_plan = app->method_plan;
  if (config->plugin_path != NULL) {
    mount.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_PLUGIN;
    mount.executor = &app->executor;
    mount.plugin_registry = &app->plugins;
    mount.plugin_ref = app->plugin_ref;
    mount.plugin_export_id = "HttpExample.Calculator.Add";
  } else {
    mount.native_binding = &app->native;
    mount.execution = databind_11_HttpExample_10_Calculator_3_Add__databind_execution();
  }
  mount.middleware = &policy;
  mount.middleware_count = 1u;
  return chttp_service_mount_http(&app->service, server, &mount);
}

int http_example_configure(http_example_app *app, const http_example_config *config) {
  if (app == NULL || config == NULL || config->title == NULL ||
      config->shutdown_timeout_ms == 0u ||
      (config->plugin_path != NULL && config->plugin_path[0] == '\0') ||
      config->title[0] == '\0' || strlen(config->title) > 256u ||
      (config->file_path != NULL && config->file_path[0] == '\0'))
    return SALTS_EINVAL;
  if (app->contract != NULL || app->rpc.impl != NULL || app->plugins.impl != NULL)
    return SALTS_EALREADY;
  const int status = example_prepare(app, config);
  if (status != SALTS_OK) {
    const int cleanup = http_example_close(app);
    return cleanup == SALTS_OK ? status : cleanup;
  }
  return SALTS_OK;
}
