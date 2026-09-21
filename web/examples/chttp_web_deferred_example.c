#include <chttp_web/web.h>

#include <cmeta/struct.h>
#include <salts/error_codes.h>
#include <salts/thread.h>
#include <salts/thread_pool.h>
#include <vstr.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  WEB_DEFERRED_TIMEOUT_MS = 5000,
  WEB_DEFERRED_CONNECTIONS = 8,
  WEB_DEFERRED_COMMANDS = 32,
  WEB_DEFERRED_HEADERS = 16,
  WEB_DEFERRED_HEADER_BYTES = 4096,
  WEB_DEFERRED_BODY_BYTES = 64 * 1024,
  WEB_DEFERRED_SEND_BYTES = 128 * 1024,
  WEB_DEFERRED_BUFFER_CAPACITY_BYTES = 2 * 1024 * 1024,
  WEB_DEFERRED_PATH_BYTES = 1024,
  WEB_DEFERRED_JOB_CAPACITY = 8,
  WEB_DEFERRED_QUEUE_CAPACITY = 7,
  WEB_DEFERRED_VALUE_BYTES = 64
};

typedef struct web_deferred_model {
  vstr value;
} web_deferred_model;

static const cmeta_type_identity WEB_DEFERRED_MODEL_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.deferred.example.model");

static const cmeta_type_desc WEB_DEFERRED_MODEL_TYPE = {
    "web_deferred_model",
    sizeof(web_deferred_model),
    _Alignof(web_deferred_model),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &WEB_DEFERRED_MODEL_ID};

static const cmeta_field_desc WEB_DEFERRED_MODEL_LAYOUT_FIELDS[] = {
    {"value", "vstr", offsetof(web_deferred_model, value),
     sizeof(((web_deferred_model *)0)->value), _Alignof(vstr), NULL, NULL}};

static const cmeta_struct_desc WEB_DEFERRED_MODEL_LAYOUT = {
    "web_deferred_model",
    sizeof(web_deferred_model),
    _Alignof(web_deferred_model),
    WEB_DEFERRED_MODEL_LAYOUT_FIELDS,
    1u};

static cmeta_data_desc web_deferred_model_desc(
    cmeta_data_field_desc fields[1], cmeta_data_struct_shape *shape) {
  fields[0] = (cmeta_data_field_desc){
      "chttp.web.deferred.example.model.value",
      "value",
      offsetof(web_deferred_model, value),
      chttp_web_vstr_cmeta_data()};
  *shape = (cmeta_data_struct_shape){
      &WEB_DEFERRED_MODEL_LAYOUT,
      fields,
      1u};
  return (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "chttp.web.deferred.example.model.data",
      .display_name = "CHttp Web deferred example model",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &WEB_DEFERRED_MODEL_TYPE,
      .shape = shape};
}

typedef enum web_deferred_job_stage {
  WEB_DEFERRED_JOB_WAITING = 0,
  WEB_DEFERRED_JOB_READY = 1,
  WEB_DEFERRED_JOB_ABORTED = 2
} web_deferred_job_stage;

struct web_deferred_app;

typedef struct web_deferred_job {
  struct web_deferred_app *app;
  chttp_server_deferred deferred;
  atomic_bool in_use;
  atomic_int stage;
  char value[WEB_DEFERRED_VALUE_BYTES];
} web_deferred_job;

typedef struct web_deferred_app {
  chttp_web_renderer renderer;
  cmeta_data_field_desc model_fields[1];
  cmeta_data_struct_shape model_shape;
  cmeta_data_desc model_desc;
  salts_threadpool_t *pool;
  web_deferred_job jobs[WEB_DEFERRED_JOB_CAPACITY];
} web_deferred_app;

static native_io_backend_kind web_deferred_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_server_config web_deferred_server_config(void) {
  chttp_server_config config = {0};
  config.host = "127.0.0.1";
  config.port = 0u;
  config.backlog = WEB_DEFERRED_CONNECTIONS;
  config.network.backend = web_deferred_backend();
  config.network.connection_capacity = WEB_DEFERRED_CONNECTIONS;
  config.network.command_capacity = WEB_DEFERRED_COMMANDS;
  config.network.request_capacity = WEB_DEFERRED_COMMANDS;
  config.network.completion_batch_capacity = WEB_DEFERRED_CONNECTIONS;
  config.network.event_capacity = WEB_DEFERRED_COMMANDS;
  config.network.max_send_bytes = WEB_DEFERRED_SEND_BYTES;
  config.network.receive_buffer_bytes = WEB_DEFERRED_HEADER_BYTES;
  config.network.connect_timeout_ms = WEB_DEFERRED_TIMEOUT_MS;
  config.network.read_timeout_ms = WEB_DEFERRED_TIMEOUT_MS;
  config.network.write_timeout_ms = WEB_DEFERRED_TIMEOUT_MS;
  config.route_capacity = 2u;
  config.middleware_capacity = 1u;
  config.max_route_middleware_count = 1u;
  config.max_route_param_count = 1u;
  config.max_route_param_bytes = WEB_DEFERRED_PATH_BYTES;
  config.max_target_bytes = WEB_DEFERRED_PATH_BYTES;
  config.max_header_count = WEB_DEFERRED_HEADERS;
  config.max_header_bytes = WEB_DEFERRED_HEADER_BYTES;
  config.max_request_body_bytes = WEB_DEFERRED_BODY_BYTES;
  config.max_response_header_count = WEB_DEFERRED_HEADERS;
  config.max_response_header_bytes = WEB_DEFERRED_HEADER_BYTES;
  config.max_response_body_bytes = WEB_DEFERRED_BODY_BYTES;
  config.max_buffered_response_body_bytes = WEB_DEFERRED_BODY_BYTES;
  config.buffer_capacity_bytes = WEB_DEFERRED_BUFFER_CAPACITY_BYTES;
  config.poll_slice_ms = 1u;
  return config;
}

static web_deferred_job *web_deferred_acquire_job(web_deferred_app *app) {
  size_t i;
  for (i = 0u; i < WEB_DEFERRED_JOB_CAPACITY; ++i) {
    bool expected = false;
    web_deferred_job *job = &app->jobs[i];
    if (atomic_compare_exchange_strong_explicit(
            &job->in_use, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
      job->app = app;
      job->deferred =
          (chttp_server_deferred)CHTTP_SERVER_DEFERRED_INIT;
      job->value[0] = '\0';
      atomic_store_explicit(
          &job->stage, WEB_DEFERRED_JOB_WAITING, memory_order_release);
      return job;
    }
  }
  return NULL;
}

static void web_deferred_release_job(web_deferred_job *job) {
  job->value[0] = '\0';
  atomic_store_explicit(&job->in_use, false, memory_order_release);
}

static void web_deferred_worker(void *user) {
  web_deferred_job *job = (web_deferred_job *)user;
  web_deferred_model model;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status web_status;
  int stage;

  do {
    stage = atomic_load_explicit(&job->stage, memory_order_acquire);
    if (stage == WEB_DEFERRED_JOB_WAITING) salts_thread_yield();
  } while (stage == WEB_DEFERRED_JOB_WAITING);

  if (stage == WEB_DEFERRED_JOB_READY) {
    model.value = vstr_from_cstr(job->value);
    web_status = chttp_web_deferred_render_reply(
        &job->app->renderer,
        &job->deferred,
        "worker.html",
        &job->app->model_desc,
        &model,
        200u,
        NULL,
        &error);
    if (web_status != CHTTP_WEB_OK) {
      (void)chttp_server_deferred_cancel(&job->deferred);
      fprintf(
          stderr,
          "deferred render failed: status=%d native=%d message=%s\n",
          (int)web_status,
          error.native_status,
          error.message);
    }
  }

  web_deferred_release_job(job);
}

static int web_deferred_busy_reply(chttp_server_response *response) {
  static const char message[] = "deferred worker capacity exhausted";
  return chttp_server_reply(
      response,
      503u,
      "text/plain; charset=utf-8",
      message,
      sizeof(message) - 1u);
}

static int web_deferred_handler(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_deferred_app *app = (web_deferred_app *)user;
  web_deferred_job *job;
  const char *value;
  size_t value_size;
  int status;

  if (app == NULL || app->pool == NULL) return SALTS_EINVAL;

  value = chttp_server_request_param(request, "value");
  if (value == NULL) return SALTS_EINVAL;
  value_size = strlen(value);
  if (value_size >= WEB_DEFERRED_VALUE_BYTES) return SALTS_EMSGSIZE;

  job = web_deferred_acquire_job(app);
  if (job == NULL) return web_deferred_busy_reply(response);

  memcpy(job->value, value, value_size + 1u);

  status = salts_threadpool_try_submit(
      app->pool, web_deferred_worker, job);
  if (status != SALTS_OK) {
    atomic_store_explicit(
        &job->stage, WEB_DEFERRED_JOB_ABORTED, memory_order_release);
    web_deferred_release_job(job);
    return web_deferred_busy_reply(response);
  }

  status = chttp_server_response_defer(response, &job->deferred);
  if (status != SALTS_OK) {
    atomic_store_explicit(
        &job->stage, WEB_DEFERRED_JOB_ABORTED, memory_order_release);
    return status;
  }

  atomic_store_explicit(
      &job->stage, WEB_DEFERRED_JOB_READY, memory_order_release);
  return SALTS_OK;
}

int main(void) {
  static const char worker_page[] =
      "<!doctype html><html><body>"
      "<main><h1>Deferred worker</h1><p>{{ value }}</p></main>"
      "</body></html>";
  static const chttp_web_template templates[] = {
      {"worker.html", worker_page, sizeof(worker_page) - 1u}};
  const salts_threadpool_config_t pool_config = {
      .num_threads = 1,
      .queue_capacity = WEB_DEFERRED_QUEUE_CAPACITY};
  chttp_web_renderer_config renderer_config =
      (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
  chttp_web_error web_error = CHTTP_WEB_ERROR_INIT;
  web_deferred_app app = {0};
  chttp_server server = {0};
  chttp_server_config server_config = web_deferred_server_config();
  uint16_t port = 0u;
  int status;
  size_t i;

  app.model_desc =
      web_deferred_model_desc(app.model_fields, &app.model_shape);
  for (i = 0u; i < WEB_DEFERRED_JOB_CAPACITY; ++i) {
    atomic_init(&app.jobs[i].in_use, false);
    atomic_init(&app.jobs[i].stage, WEB_DEFERRED_JOB_ABORTED);
  }

  status = chttp_web_renderer_init(
      &app.renderer, templates, 1u, &renderer_config, &web_error);
  if (status != CHTTP_WEB_OK) {
    fprintf(stderr, "renderer init failed: %s\n", web_error.message);
    return EXIT_FAILURE;
  }

  app.pool = salts_threadpool_create_with_config(&pool_config);
  if (app.pool == NULL) {
    chttp_web_renderer_destroy(&app.renderer);
    return EXIT_FAILURE;
  }

  status = chttp_server_init(&server, &server_config);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/work/:value", web_deferred_handler, &app);
  if (status == SALTS_OK) status = chttp_server_start(&server);
  if (status == SALTS_OK) status = chttp_server_port(&server, &port);

  if (status == SALTS_OK) {
    printf(
        "CHttp::Web deferred example: "
        "http://127.0.0.1:%u/work/hello\n",
        (unsigned int)port);
    fflush(stdout);
    (void)getchar();
  }

  if (server.impl) {
    const int stopped =
        chttp_server_stop(&server, WEB_DEFERRED_TIMEOUT_MS);
    if (status == SALTS_OK) status = stopped;
  }

  if (app.pool != NULL) {
    const int shutdown_status = salts_threadpool_shutdown_with_policy(
        app.pool, SALTS_THREADPOOL_SHUTDOWN_DRAIN);
    const int wait_status =
        shutdown_status == SALTS_OK
            ? salts_threadpool_wait_status(app.pool)
            : shutdown_status;
    if (status == SALTS_OK) status = wait_status;
    salts_threadpool_destroy(app.pool);
    app.pool = NULL;
  }

  if (server.impl && status == SALTS_OK)
    status = chttp_server_destroy(&server);

  chttp_web_renderer_destroy(&app.renderer);

  if (status != SALTS_OK)
    fprintf(stderr, "CHttp::Web deferred example failed: %d\n", status);
  return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
