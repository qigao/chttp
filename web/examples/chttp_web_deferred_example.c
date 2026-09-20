#include <chttp_web/web.h>

#include <cmeta/struct.h>
#include <salts/error_codes.h>
#include <salts/thread.h>
#include <vstr.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  WEB_DEFERRED_EXAMPLE_TIMEOUT_MS = 5000,
  WEB_DEFERRED_EXAMPLE_CONNECTIONS = 8,
  WEB_DEFERRED_EXAMPLE_HEADERS = 16,
  WEB_DEFERRED_EXAMPLE_HEADER_BYTES = 4096,
  WEB_DEFERRED_EXAMPLE_BODY_BYTES = 64 * 1024
};

typedef struct web_deferred_example_model {
  vstr value;
} web_deferred_example_model;

typedef struct web_deferred_example_job {
  chttp_server_deferred deferred;
  salts_thread_t thread;
  atomic_int admitted;
  atomic_int done;
  char value[64];
  int web_status;
  int native_status;
} web_deferred_example_job;

static const cmeta_type_identity WEB_DEFERRED_EXAMPLE_MODEL_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.deferred.example.model");

static const cmeta_type_desc WEB_DEFERRED_EXAMPLE_MODEL_TYPE = {
    "web_deferred_example_model",
    sizeof(web_deferred_example_model),
    _Alignof(web_deferred_example_model),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &WEB_DEFERRED_EXAMPLE_MODEL_ID};

static const cmeta_field_desc WEB_DEFERRED_EXAMPLE_LAYOUT_FIELDS[] = {
    {"value", "vstr", offsetof(web_deferred_example_model, value),
     sizeof(((web_deferred_example_model *)0)->value), _Alignof(vstr),
     NULL, NULL}};

static const cmeta_struct_desc WEB_DEFERRED_EXAMPLE_LAYOUT = {
    "web_deferred_example_model",
    sizeof(web_deferred_example_model),
    _Alignof(web_deferred_example_model),
    WEB_DEFERRED_EXAMPLE_LAYOUT_FIELDS,
    1u};

static cmeta_data_desc web_deferred_example_model_desc(
    cmeta_data_field_desc fields[1], cmeta_data_struct_shape *shape) {
  fields[0] = (cmeta_data_field_desc){
      "chttp.web.deferred.example.model.value",
      "value",
      offsetof(web_deferred_example_model, value),
      chttp_web_vstr_cmeta_data()};
  *shape = (cmeta_data_struct_shape){
      &WEB_DEFERRED_EXAMPLE_LAYOUT, fields, 1u};
  return (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "chttp.web.deferred.example.model.data",
      .display_name = "CHttp Web deferred example model",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &WEB_DEFERRED_EXAMPLE_MODEL_TYPE,
      .shape = shape};
}

static void web_deferred_example_worker(void *user) {
  static const char page[] =
      "<!doctype html><html><body><h1>{{ value }}</h1></body></html>";
  static const chttp_web_template templates[] = {
      {"page.html", page, sizeof(page) - 1u}};
  web_deferred_example_job *job = (web_deferred_example_job *)user;
  chttp_web_renderer renderer = {0};
  chttp_web_renderer_config config =
      (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  cmeta_data_field_desc fields[1];
  cmeta_data_struct_shape shape;
  cmeta_data_desc desc = web_deferred_example_model_desc(fields, &shape);
  web_deferred_example_model model = {
      .value = vstr_from_cstr(job->value)};

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

static int web_deferred_example_handler(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_deferred_example_job *job = (web_deferred_example_job *)user;
  const char *value = chttp_server_request_param(request, "value");
  size_t value_size;
  int expected = 0;
  int status;

  if (job == NULL || value == NULL) return SALTS_EINVAL;
  if (!atomic_compare_exchange_strong_explicit(
          &job->admitted, &expected, 1,
          memory_order_acq_rel, memory_order_acquire))
    return chttp_server_reply(
        response, 503u, "text/plain", "worker busy", 11u);

  value_size = strlen(value);
  if (value_size >= sizeof(job->value)) {
    atomic_store_explicit(&job->admitted, 0, memory_order_release);
    return SALTS_EMSGSIZE;
  }
  memcpy(job->value, value, value_size + 1u);

  status = chttp_server_response_defer(response, &job->deferred);
  if (status != SALTS_OK) {
    atomic_store_explicit(&job->admitted, 0, memory_order_release);
    return status;
  }

  status = salts_thread_create(
      &job->thread, web_deferred_example_worker, job);
  if (status != SALTS_OK) {
    (void)chttp_server_deferred_cancel(&job->deferred);
    atomic_store_explicit(&job->admitted, 0, memory_order_release);
    return status;
  }
  return SALTS_OK;
}

static native_io_backend_kind web_deferred_example_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

int main(void) {
  web_deferred_example_job job = {
      .deferred = CHTTP_SERVER_DEFERRED_INIT};
  chttp_server server = {0};
  chttp_server_config config = {0};
  uint16_t port = 0u;
  int status;

  atomic_init(&job.admitted, 0);
  atomic_init(&job.done, 0);
  job.web_status = CHTTP_WEB_INVALID_ARGUMENT;
  job.native_status = SALTS_OK;

  config.host = "127.0.0.1";
  config.port = 0u;
  config.backlog = WEB_DEFERRED_EXAMPLE_CONNECTIONS;
  config.network.backend = web_deferred_example_backend();
  config.network.connection_capacity = WEB_DEFERRED_EXAMPLE_CONNECTIONS;
  config.network.command_capacity = 32u;
  config.network.request_capacity = 16u;
  config.network.completion_batch_capacity = 8u;
  config.network.event_capacity = 32u;
  config.network.max_send_bytes = 128u * 1024u;
  config.network.receive_buffer_bytes = WEB_DEFERRED_EXAMPLE_HEADER_BYTES;
  config.network.connect_timeout_ms = WEB_DEFERRED_EXAMPLE_TIMEOUT_MS;
  config.network.read_timeout_ms = WEB_DEFERRED_EXAMPLE_TIMEOUT_MS;
  config.network.write_timeout_ms = WEB_DEFERRED_EXAMPLE_TIMEOUT_MS;
  config.route_capacity = 2u;
  config.middleware_capacity = 1u;
  config.max_route_middleware_count = 1u;
  config.max_route_param_count = 1u;
  config.max_route_param_bytes = 128u;
  config.max_target_bytes = 256u;
  config.max_header_count = WEB_DEFERRED_EXAMPLE_HEADERS;
  config.max_header_bytes = WEB_DEFERRED_EXAMPLE_HEADER_BYTES;
  config.max_request_body_bytes = 1024u;
  config.max_response_header_count = WEB_DEFERRED_EXAMPLE_HEADERS;
  config.max_response_header_bytes = WEB_DEFERRED_EXAMPLE_HEADER_BYTES;
  config.max_response_body_bytes = WEB_DEFERRED_EXAMPLE_BODY_BYTES;
  config.max_buffered_response_body_bytes =
      WEB_DEFERRED_EXAMPLE_BODY_BYTES;
  config.buffer_capacity_bytes = 512u * 1024u;
  config.poll_slice_ms = 1u;

  status = chttp_server_init(&server, &config);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/slow/:value",
        web_deferred_example_handler, &job);
  if (status == SALTS_OK) status = chttp_server_start(&server);
  if (status == SALTS_OK) status = chttp_server_port(&server, &port);

  if (status == SALTS_OK) {
    printf(
        "CHttp::Web deferred example: http://127.0.0.1:%u/slow/hello\n"
        "This reference intentionally admits one worker job. Press Enter to stop.\n",
        (unsigned)port);
    fflush(stdout);
    (void)getchar();
  }

  if (server.impl) {
    const int stopped =
        chttp_server_stop(&server, WEB_DEFERRED_EXAMPLE_TIMEOUT_MS);
    if (status == SALTS_OK) status = stopped;
    if (stopped == SALTS_OK) {
      const int destroyed = chttp_server_destroy(&server);
      if (status == SALTS_OK) status = destroyed;
    }
  }

  if (job.thread != NULL) {
    const int joined = salts_thread_join(&job.thread);
    if (status == SALTS_OK) status = joined;
    salts_thread_destroy(&job.thread);
  }

  if (atomic_load_explicit(&job.done, memory_order_acquire) != 0 &&
      job.web_status != CHTTP_WEB_OK) {
    fprintf(stderr, "deferred render failed: web=%d native=%d\n",
            job.web_status, job.native_status);
    status = job.native_status != SALTS_OK
                 ? job.native_status
                 : SALTS_EIO;
  }

  if (status != SALTS_OK)
    fprintf(stderr, "CHttp::Web deferred example failed: %d\n", status);
  return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
