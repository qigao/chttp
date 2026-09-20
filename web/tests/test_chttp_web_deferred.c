#include <chttp_web/web.h>
#include "tinytest.h"

#include <http_client/http.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <vstr.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
  WEB_DEFERRED_TIMEOUT_MS = 5000,
  WEB_DEFERRED_STOP_TIMEOUT_MS = 20,
  WEB_DEFERRED_ITERATIONS = 16
};

typedef struct web_deferred_model {
  vstr value;
} web_deferred_model;

static const cmeta_type_identity WEB_DEFERRED_MODEL_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.deferred.test.model");

static const cmeta_type_desc WEB_DEFERRED_MODEL_TYPE = {
    "web_deferred_model", sizeof(web_deferred_model), _Alignof(web_deferred_model),
    CMETA_T_OBJECT, NULL, NULL, &WEB_DEFERRED_MODEL_ID};

static const cmeta_field_desc WEB_DEFERRED_MODEL_LAYOUT_FIELDS[] = {
    {"value", "vstr", offsetof(web_deferred_model, value),
     sizeof(((web_deferred_model *)0)->value), _Alignof(vstr), NULL, NULL}};

static const cmeta_struct_desc WEB_DEFERRED_MODEL_LAYOUT = {
    "web_deferred_model", sizeof(web_deferred_model), _Alignof(web_deferred_model),
    WEB_DEFERRED_MODEL_LAYOUT_FIELDS, 1u};

static cmeta_data_desc web_deferred_model_desc(
    cmeta_data_field_desc fields[1], cmeta_data_struct_shape *shape) {
  fields[0] = (cmeta_data_field_desc){
      "chttp.web.deferred.test.model.value", "value",
      offsetof(web_deferred_model, value), chttp_web_vstr_cmeta_data()};
  *shape = (cmeta_data_struct_shape){
      &WEB_DEFERRED_MODEL_LAYOUT, fields, 1u};
  return (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "chttp.web.deferred.test.model.data",
      .display_name = "CHttp Web deferred test model",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &WEB_DEFERRED_MODEL_TYPE,
      .shape = shape};
}

typedef enum web_deferred_mode {
  WEB_DEFERRED_REPLY,
  WEB_DEFERRED_HOLD,
  WEB_DEFERRED_GATED
} web_deferred_mode;

typedef struct web_deferred_job {
  chttp_server_deferred deferred;
  salts_thread_t thread;
  web_deferred_mode mode;
  atomic_int acquired;
  atomic_int release;
  atomic_int done;
  int web_status;
  int native_status;
  char value[64];
} web_deferred_job;

typedef struct web_deferred_app {
  web_deferred_job job;
} web_deferred_app;

typedef struct web_deferred_client_call {
  uint16_t port;
  char target[128];
  int status;
  unsigned int status_code;
  size_t body_size;
  char body[256];
} web_deferred_client_call;

static native_io_backend_kind web_deferred_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config web_deferred_network(size_t connections) {
  const cnet_client_config config = {
      .backend = web_deferred_backend(),
      .connection_capacity = connections,
      .command_capacity = 16u,
      .request_capacity = 8u,
      .completion_batch_capacity = 8u,
      .event_capacity = 16u,
      .max_send_bytes = 8192u,
      .receive_buffer_bytes = 2048u,
      .connect_timeout_ms = WEB_DEFERRED_TIMEOUT_MS,
      .read_timeout_ms = WEB_DEFERRED_TIMEOUT_MS,
      .write_timeout_ms = WEB_DEFERRED_TIMEOUT_MS};
  return config;
}

static chttp_server_config web_deferred_server_config(void) {
  const chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = web_deferred_network(4u),
      .route_capacity = 4u,
      .middleware_capacity = 1u,
      .max_route_middleware_count = 1u,
      .max_route_param_count = 2u,
      .max_route_param_bytes = 128u,
      .max_target_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 1024u,
      .max_request_body_bytes = 512u,
      .max_response_header_count = 16u,
      .max_response_header_bytes = 1024u,
      .max_response_body_bytes = 1024u,
      .max_buffered_response_body_bytes = 1024u,
      .buffer_capacity_bytes = 64u * 1024u,
      .poll_slice_ms = 1u};
  return config;
}

static chttp_client_config web_deferred_client_config(void) {
  const chttp_client_config config = {
      .network = web_deferred_network(2u),
      .request_capacity = 1u,
      .max_start_line_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 1024u,
      .max_request_body_bytes = 512u,
      .max_response_body_bytes = 1024u,
      .max_informational_responses = 2u};
  return config;
}

static void web_deferred_job_reset(
    web_deferred_job *job, web_deferred_mode mode) {
  memset(job, 0, sizeof(*job));
  job->deferred = (chttp_server_deferred)CHTTP_SERVER_DEFERRED_INIT;
  job->mode = mode;
  job->web_status = CHTTP_WEB_INVALID_ARGUMENT;
  job->native_status = SALTS_OK;
  atomic_init(&job->acquired, 0);
  atomic_init(&job->release, mode == WEB_DEFERRED_GATED ? 0 : 1);
  atomic_init(&job->done, 0);
}

static void web_deferred_worker(void *user) {
  static const char page[] = "<p>{{ value }}</p>";
  static const chttp_web_template templates[] = {
      {"page.html", page, sizeof(page) - 1u}};
  web_deferred_job *job = (web_deferred_job *)user;
  chttp_web_renderer renderer = {0};
  chttp_web_renderer_config config =
      (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  cmeta_data_field_desc fields[1];
  cmeta_data_struct_shape shape;
  cmeta_data_desc desc = web_deferred_model_desc(fields, &shape);
  web_deferred_model model = {.value = vstr_from_cstr(job->value)};

  while (atomic_load_explicit(&job->release, memory_order_acquire) == 0)
    salts_thread_yield();

  job->web_status = chttp_web_renderer_init(
      &renderer, templates, 1u, &config, &error);
  if (job->web_status == CHTTP_WEB_OK)
    job->web_status = chttp_web_deferred_render_reply(
        &renderer, &job->deferred, "page.html", &desc, &model,
        200u, NULL, &error);
  job->native_status = error.native_status;
  chttp_web_renderer_destroy(&renderer);
  atomic_store_explicit(&job->done, 1, memory_order_release);
}

static int web_deferred_handler(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_deferred_app *app = (web_deferred_app *)user;
  web_deferred_job *job = &app->job;
  const char *value = chttp_server_request_param(request, "value");
  size_t value_size;
  int status;

  if (!value) return SALTS_EINVAL;
  value_size = strlen(value);
  if (value_size >= sizeof(job->value)) return SALTS_EMSGSIZE;
  memcpy(job->value, value, value_size + 1u);

  status = chttp_server_response_defer(response, &job->deferred);
  if (status != SALTS_OK) return status;

  if (job->mode != WEB_DEFERRED_HOLD) {
    status = salts_thread_create(&job->thread, web_deferred_worker, job);
    if (status != SALTS_OK) {
      (void)chttp_server_deferred_cancel(&job->deferred);
      return status;
    }
  }
  atomic_store_explicit(&job->acquired, 1, memory_order_release);
  return SALTS_OK;
}

static int web_deferred_server_start(
    web_deferred_app *app, chttp_server *server, uint16_t *out_port) {
  chttp_server_config config = web_deferred_server_config();
  int status = chttp_server_init(server, &config);
  if (status == SALTS_OK)
    status = chttp_server_get(
        server, "/work/:value", web_deferred_handler, app);
  if (status == SALTS_OK) status = chttp_server_start(server);
  if (status == SALTS_OK) status = chttp_server_port(server, out_port);
  return status;
}

static int web_deferred_wait(
    atomic_int *value, int expected, uint32_t timeout_ms) {
  const uint64_t deadline = salts_monotonic_ms() + timeout_ms;
  while (atomic_load_explicit(value, memory_order_acquire) != expected) {
    if (salts_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
    salts_thread_yield();
  }
  return SALTS_OK;
}

static int web_deferred_wait_inactive(
    chttp_server *server, uint32_t timeout_ms) {
  const uint64_t deadline = salts_monotonic_ms() + timeout_ms;
  for (;;) {
    chttp_server_stats stats = {0};
    const int status = chttp_server_get_stats(server, &stats);
    if (status != SALTS_OK) return status;
    if (stats.active_connections == 0u) return SALTS_OK;
    if (salts_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
    salts_thread_yield();
  }
}

static int web_deferred_call(
    chttp_client *client, uint16_t port, const char *target,
    chttp_response *response) {
  char uri[64];
  const int written =
      snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port);
  chttp_options options = {0};
  chttp_error error = {0};
  if (written < 0 || (size_t)written >= sizeof(uri)) return SALTS_EMSGSIZE;
  options.connection_uri = uri;
  options.authority = "127.0.0.1";
  options.target = target;
  options.timeout_ms = WEB_DEFERRED_TIMEOUT_MS;
  return chttp_get(client, &options, response, &error);
}

static void web_deferred_client_thread(void *user) {
  web_deferred_client_call *call = (web_deferred_client_call *)user;
  chttp_client client = {0};
  chttp_client_config config = web_deferred_client_config();
  chttp_response response = {0};

  call->status = chttp_client_init(&client, &config);
  if (call->status == SALTS_OK)
    call->status =
        web_deferred_call(&client, call->port, call->target, &response);
  if (call->status == SALTS_OK) {
    call->status_code = response.status_code;
    call->body_size = response.body_size < sizeof(call->body) - 1u
                          ? response.body_size
                          : sizeof(call->body) - 1u;
    if (call->body_size != 0u)
      memcpy(call->body, response.body, call->body_size);
    call->body[call->body_size] = '\0';
  }
  chttp_response_destroy(&response);
  if (client.impl)
    (void)chttp_client_destroy(&client, WEB_DEFERRED_TIMEOUT_MS);
}

spec("CHttp::Web deferred worker rendering") {
  it("renders and replies from repeated worker contexts without state leakage") {
    chttp_server server = {0};
    chttp_client client = {0};
    web_deferred_app app;
    chttp_client_config client_config = web_deferred_client_config();
    uint16_t port = 0u;

    memset(&app, 0, sizeof(app));
    check_equal(web_deferred_server_start(&app, &server, &port), SALTS_OK);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    for (size_t i = 0u; i < WEB_DEFERRED_ITERATIONS; ++i) {
      char value[32];
      char target[64];
      char expected[64];
      chttp_response response = {0};
      const int value_size =
          snprintf(value, sizeof(value), "worker-%zu", i);
      check_true(value_size > 0 && (size_t)value_size < sizeof(value));
      check_true(snprintf(target, sizeof(target), "/work/%s", value) > 0);
      check_true(snprintf(expected, sizeof(expected), "<p>%s</p>", value) > 0);

      web_deferred_job_reset(&app.job, WEB_DEFERRED_REPLY);
      check_equal(
          web_deferred_call(&client, port, target, &response), SALTS_OK);
      check_equal(salts_thread_join(&app.job.thread), SALTS_OK);
      salts_thread_destroy(&app.job.thread);
      check_equal(app.job.web_status, CHTTP_WEB_OK);
      check_equal(app.job.native_status, SALTS_OK);
      check_equal(response.status_code, 200u);
      check_equal(response.body_size, strlen(expected));
      check_equal(response.body, expected, response.body_size);
      chttp_response_destroy(&response);
    }

    check_equal(
        chttp_client_destroy(&client, WEB_DEFERRED_TIMEOUT_MS), SALTS_OK);
    check_equal(
        chttp_server_stop(&server, WEB_DEFERRED_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("reports a canceled handle as stale without publishing rendered bytes") {
    chttp_server server = {0};
    web_deferred_app app;
    web_deferred_client_call call = {0};
    salts_thread_t client_thread = NULL;
    chttp_server_deferred stale;
    chttp_web_renderer renderer = {0};
    static const char page[] = "<p>{{ value }}</p>";
    static const chttp_web_template templates[] = {
        {"page.html", page, sizeof(page) - 1u}};
    chttp_web_renderer_config renderer_config =
        (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    cmeta_data_field_desc fields[1];
    cmeta_data_struct_shape shape;
    cmeta_data_desc desc = web_deferred_model_desc(fields, &shape);
    web_deferred_model model = {.value = vstr_from_cstr("late")};
    uint16_t port = 0u;

    memset(&app, 0, sizeof(app));
    web_deferred_job_reset(&app.job, WEB_DEFERRED_HOLD);
    check_equal(web_deferred_server_start(&app, &server, &port), SALTS_OK);

    call.port = port;
    snprintf(call.target, sizeof(call.target), "/work/stale");
    check_equal(
        salts_thread_create(
            &client_thread, web_deferred_client_thread, &call),
        SALTS_OK);
    check_equal(
        web_deferred_wait(
            &app.job.acquired, 1, WEB_DEFERRED_TIMEOUT_MS),
        SALTS_OK);

    stale = app.job.deferred;
    check_equal(chttp_server_deferred_cancel(&app.job.deferred), SALTS_OK);
    check_equal(
        web_deferred_wait_inactive(&server, WEB_DEFERRED_TIMEOUT_MS),
        SALTS_OK);
    check_equal(salts_thread_join(&client_thread), SALTS_OK);
    salts_thread_destroy(&client_thread);

    check_equal(
        chttp_web_renderer_init(
            &renderer, templates, 1u, &renderer_config, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_deferred_render_reply(
            &renderer, &stale, "page.html", &desc, &model,
            200u, NULL, &error),
        CHTTP_WEB_SERVER);
    check_equal(error.native_status, SALTS_ENOENT);
    chttp_web_renderer_destroy(&renderer);

    check_equal(
        chttp_server_stop(&server, WEB_DEFERRED_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("keeps stop pending until a gated worker completes its deferred terminal") {
    chttp_server server = {0};
    web_deferred_app app;
    web_deferred_client_call call = {0};
    salts_thread_t client_thread = NULL;
    uint16_t port = 0u;

    memset(&app, 0, sizeof(app));
    web_deferred_job_reset(&app.job, WEB_DEFERRED_GATED);
    check_equal(web_deferred_server_start(&app, &server, &port), SALTS_OK);

    call.port = port;
    snprintf(call.target, sizeof(call.target), "/work/stop");
    check_equal(
        salts_thread_create(
            &client_thread, web_deferred_client_thread, &call),
        SALTS_OK);
    check_equal(
        web_deferred_wait(
            &app.job.acquired, 1, WEB_DEFERRED_TIMEOUT_MS),
        SALTS_OK);

    check_equal(
        chttp_server_stop(&server, WEB_DEFERRED_STOP_TIMEOUT_MS),
        SALTS_ETIMEDOUT);
    atomic_store_explicit(&app.job.release, 1, memory_order_release);
    check_equal(
        chttp_server_stop(&server, WEB_DEFERRED_TIMEOUT_MS), SALTS_OK);

    check_equal(salts_thread_join(&app.job.thread), SALTS_OK);
    salts_thread_destroy(&app.job.thread);
    check_equal(salts_thread_join(&client_thread), SALTS_OK);
    salts_thread_destroy(&client_thread);
    check_equal(app.job.web_status, CHTTP_WEB_OK);
    check_equal(app.job.native_status, SALTS_OK);
    /* HTTP/1.1 shutdown closes the active transport before the deferred
     * terminal is consumed. The server must still wait for that terminal,
     * but the client must not observe a successful partial response. */
    check_equal(call.status, SALTS_EPROTO);
    check_equal(call.status_code, 0u);
    check_equal(call.body_size, 0u);

    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }
}
