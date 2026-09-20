#include <chttp_web/web.h>
#include "tinytest.h"

#include <http_client/http.h>
#include <salts/thread.h>
#include <vstr.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
  WEB_LIFECYCLE_TIMEOUT_MS = 5000,
  WEB_LIFECYCLE_CYCLES = 3
};

typedef struct web_lifecycle_model {
  vstr value;
} web_lifecycle_model;

static const cmeta_type_identity WEB_LIFECYCLE_MODEL_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.lifecycle.test.model");

static const cmeta_type_desc WEB_LIFECYCLE_MODEL_TYPE = {
    "web_lifecycle_model",
    sizeof(web_lifecycle_model),
    _Alignof(web_lifecycle_model),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &WEB_LIFECYCLE_MODEL_ID};

static const cmeta_field_desc WEB_LIFECYCLE_MODEL_LAYOUT_FIELDS[] = {
    {"value",
     "vstr",
     offsetof(web_lifecycle_model, value),
     sizeof(((web_lifecycle_model *)0)->value),
     _Alignof(vstr),
     NULL,
     NULL}};

static const cmeta_struct_desc WEB_LIFECYCLE_MODEL_LAYOUT = {
    "web_lifecycle_model",
    sizeof(web_lifecycle_model),
    _Alignof(web_lifecycle_model),
    WEB_LIFECYCLE_MODEL_LAYOUT_FIELDS,
    1u};

typedef struct web_lifecycle_app {
  chttp_web_renderer *renderer;
  cmeta_data_field_desc fields[1];
  cmeta_data_struct_shape shape;
  cmeta_data_desc desc;
} web_lifecycle_app;

static native_io_backend_kind web_lifecycle_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config web_lifecycle_network(size_t connections) {
  const cnet_client_config config = {
      .backend = web_lifecycle_backend(),
      .connection_capacity = connections,
      .command_capacity = 16u,
      .request_capacity = 8u,
      .completion_batch_capacity = 8u,
      .event_capacity = 16u,
      .max_send_bytes = 8192u,
      .receive_buffer_bytes = 2048u,
      .connect_timeout_ms = WEB_LIFECYCLE_TIMEOUT_MS,
      .read_timeout_ms = WEB_LIFECYCLE_TIMEOUT_MS,
      .write_timeout_ms = WEB_LIFECYCLE_TIMEOUT_MS};
  return config;
}

static chttp_server_config web_lifecycle_server_config(void) {
  const chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = web_lifecycle_network(4u),
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

static chttp_client_config web_lifecycle_client_config(void) {
  const chttp_client_config config = {
      .network = web_lifecycle_network(2u),
      .request_capacity = 2u,
      .max_start_line_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 1024u,
      .max_request_body_bytes = 512u,
      .max_response_body_bytes = 1024u,
      .max_informational_responses = 2u};
  return config;
}

static void web_lifecycle_app_init(
    web_lifecycle_app *app, chttp_web_renderer *renderer) {
  app->renderer = renderer;
  app->fields[0] = (cmeta_data_field_desc){
      "chttp.web.lifecycle.test.model.value",
      "value",
      offsetof(web_lifecycle_model, value),
      chttp_web_vstr_cmeta_data()};
  app->shape = (cmeta_data_struct_shape){
      &WEB_LIFECYCLE_MODEL_LAYOUT,
      app->fields,
      1u};
  app->desc = (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "chttp.web.lifecycle.test.model.data",
      .display_name = "CHttp Web lifecycle test model",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &WEB_LIFECYCLE_MODEL_TYPE,
      .shape = &app->shape};
}

static int web_lifecycle_handler(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_lifecycle_app *app = (web_lifecycle_app *)user;
  const char *value = chttp_server_request_param(request, "value");
  web_lifecycle_model model;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;

  if (app == NULL || app->renderer == NULL || value == NULL)
    return SALTS_EINVAL;

  model.value = vstr_from_cstr(value);
  status = chttp_web_render_response(
      app->renderer,
      response,
      "page.html",
      &app->desc,
      &model,
      200u,
      NULL,
      &error);
  return status == CHTTP_WEB_OK
             ? SALTS_OK
             : (error.native_status != SALTS_OK
                    ? error.native_status
                    : SALTS_EIO);
}

static int web_lifecycle_call(
    chttp_client *client,
    uint16_t port,
    const char *target,
    chttp_response *response) {
  char uri[64];
  chttp_options options = {0};
  chttp_error error = {0};
  const int written =
      snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port);

  if (written <= 0 || (size_t)written >= sizeof(uri)) return SALTS_EMSGSIZE;

  options.connection_uri = uri;
  options.authority = "127.0.0.1";
  options.target = target;
  options.timeout_ms = WEB_LIFECYCLE_TIMEOUT_MS;
  return chttp_get(client, &options, response, &error);
}

spec("CHttp::Web lifecycle qualification") {
  it("reuses one renderer across isolated requests and repeated server restarts") {
    static const char page[] = "<p>{{ value }}</p>";
    static const chttp_web_template templates[] = {
        {"page.html", page, sizeof(page) - 1u}};
    chttp_web_renderer renderer = {0};
    chttp_web_renderer_config renderer_config =
        (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    web_lifecycle_app app;

    check_equal(
        chttp_web_renderer_init(
            &renderer, templates, 1u, &renderer_config, &error),
        CHTTP_WEB_OK);
    web_lifecycle_app_init(&app, &renderer);
    check_true(cmeta_data_desc_valid(&app.desc));

    for (size_t cycle = 0u; cycle < WEB_LIFECYCLE_CYCLES; ++cycle) {
      chttp_server server = {0};
      chttp_client client = {0};
      chttp_server_config server_config = web_lifecycle_server_config();
      chttp_client_config client_config = web_lifecycle_client_config();
      uint16_t port = 0u;

      check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
      check_equal(
          chttp_server_get(
              &server, "/hello/:value", web_lifecycle_handler, &app),
          SALTS_OK);
      check_equal(chttp_server_start(&server), SALTS_OK);
      check_equal(chttp_server_port(&server, &port), SALTS_OK);
      check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

      for (size_t request = 0u; request < 2u; ++request) {
        char target[64];
        char expected[64];
        chttp_response response = {0};
        const char *label = request == 0u ? "first" : "second";
        int target_size = snprintf(
            target, sizeof(target), "/hello/%s-%zu", label, cycle);
        int expected_size = snprintf(
            expected, sizeof(expected), "<p>%s-%zu</p>", label, cycle);

        check_true(
            target_size > 0 && (size_t)target_size < sizeof(target));
        check_true(
            expected_size > 0 && (size_t)expected_size < sizeof(expected));
        check_equal(
            web_lifecycle_call(&client, port, target, &response),
            SALTS_OK);
        check_equal(response.status_code, 200u);
        check_equal(response.body_size, (size_t)expected_size);
        check_equal(
            response.body, expected, (size_t)expected_size);
        chttp_response_destroy(&response);
      }

      check_equal(
          chttp_client_destroy(&client, WEB_LIFECYCLE_TIMEOUT_MS),
          SALTS_OK);
      check_equal(
          chttp_server_stop(&server, WEB_LIFECYCLE_TIMEOUT_MS),
          SALTS_OK);
      check_equal(chttp_server_destroy(&server), SALTS_OK);
    }

    chttp_web_renderer_destroy(&renderer);
    chttp_web_renderer_destroy(&renderer);
  }
}
