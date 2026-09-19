#include <chttp_web/web.h>

#include <cmeta/struct.h>
#include <salts/error_codes.h>
#include <vstr.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  WEB_EXAMPLE_TIMEOUT_MS = 5000,
  WEB_EXAMPLE_CONNECTIONS = 8,
  WEB_EXAMPLE_COMMANDS = 32,
  WEB_EXAMPLE_HEADERS = 16,
  WEB_EXAMPLE_HEADER_BYTES = 4096,
  WEB_EXAMPLE_BODY_BYTES = 64 * 1024,
  WEB_EXAMPLE_SEND_BYTES = 128 * 1024,
  WEB_EXAMPLE_COMMAND_BUFFER_BYTES = 512 * 1024,
  WEB_EXAMPLE_BUFFER_CAPACITY_BYTES = 2 * 1024 * 1024,
  WEB_EXAMPLE_PATH_BYTES = 1024
};

typedef struct web_example_model {
  vstr title;
} web_example_model;

typedef struct web_example_context {
  chttp_web_renderer *renderer;
  const cmeta_data_desc *model_desc;
  const web_example_model *model;
} web_example_context;

static const cmeta_type_identity WEB_EXAMPLE_MODEL_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.example.model");

static const cmeta_type_desc WEB_EXAMPLE_MODEL_TYPE = {
    "web_example_model", sizeof(web_example_model), _Alignof(web_example_model),
    CMETA_T_OBJECT, NULL, NULL, &WEB_EXAMPLE_MODEL_ID};

static const cmeta_field_desc WEB_EXAMPLE_MODEL_LAYOUT_FIELDS[] = {
    {"title", "vstr", offsetof(web_example_model, title),
     sizeof(((web_example_model *)0)->title), _Alignof(vstr), NULL, NULL}};

static const cmeta_struct_desc WEB_EXAMPLE_MODEL_LAYOUT = {
    "web_example_model", sizeof(web_example_model), _Alignof(web_example_model),
    WEB_EXAMPLE_MODEL_LAYOUT_FIELDS, 1u};

static cmeta_data_desc web_example_model_desc(
    cmeta_data_field_desc fields[1], cmeta_data_struct_shape *shape) {
  fields[0] = (cmeta_data_field_desc){
      "chttp.web.example.model.title", "title",
      offsetof(web_example_model, title), jinja_cmeta_vstr_data()};
  *shape = (cmeta_data_struct_shape){
      &WEB_EXAMPLE_MODEL_LAYOUT, fields, 1u};
  return (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "chttp.web.example.model.data",
      .display_name = "CHttp Web example model",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &WEB_EXAMPLE_MODEL_TYPE,
      .shape = shape};
}

static int web_example_page(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_example_context *context = (web_example_context *)user;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  (void)request;

  status = chttp_web_render_response(
      context->renderer, response, "page.html",
      context->model_desc, context->model, 200u, NULL, &error);
  if (status != CHTTP_WEB_OK) {
    fprintf(stderr, "render failed: status=%d native=%d message=%s\n",
            (int)status, error.native_status, error.message);
    return SALTS_EIO;
  }
  return SALTS_OK;
}

int main(void) {
  static const char layout[] =
      "<!doctype html><html><body>{% block body %}{% endblock %}</body></html>";
  static const char page[] =
      "{% extends \"layout.html\" %}"
      "{% block body %}<main><h1>{{ title }}</h1></main>{% endblock %}";
  static const chttp_web_template templates[] = {
      {"layout.html", layout, sizeof(layout) - 1u},
      {"page.html", page, sizeof(page) - 1u}};

  chttp_web_renderer renderer = {0};
  chttp_web_renderer_config renderer_config =
      (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
  chttp_web_error web_error = CHTTP_WEB_ERROR_INIT;
  cmeta_data_field_desc fields[1];
  cmeta_data_struct_shape shape;
  cmeta_data_desc desc = web_example_model_desc(fields, &shape);
  web_example_model model = {
      .title = vstr_from_cstr("<script>alert(1)</script> CHttp::Web")};
  web_example_context context = {&renderer, &desc, &model};
  chttp_server server = {0};
  chttp_server_config config = {0};
  uint16_t port = 0u;
  int status;

  status = chttp_web_renderer_init(
      &renderer, templates, 2u, &renderer_config, &web_error);
  if (status != CHTTP_WEB_OK) {
    fprintf(stderr, "renderer init failed: %s\n", web_error.message);
    return EXIT_FAILURE;
  }

  config.host = "127.0.0.1";
  config.port = 0u;
  config.backlog = WEB_EXAMPLE_CONNECTIONS;
#if defined(_WIN32)
  config.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  config.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
  config.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
  config.network.connection_capacity = WEB_EXAMPLE_CONNECTIONS;
  config.network.command_capacity = WEB_EXAMPLE_COMMANDS;
  config.network.request_capacity = WEB_EXAMPLE_COMMANDS;
  config.network.completion_batch_capacity = WEB_EXAMPLE_CONNECTIONS;
  config.network.event_capacity = WEB_EXAMPLE_COMMANDS;
  config.network.max_send_bytes = WEB_EXAMPLE_SEND_BYTES;
  config.network.command_buffer_bytes = WEB_EXAMPLE_COMMAND_BUFFER_BYTES;
  config.network.receive_buffer_bytes = WEB_EXAMPLE_HEADER_BYTES;
  config.network.connect_timeout_ms = WEB_EXAMPLE_TIMEOUT_MS;
  config.network.read_timeout_ms = WEB_EXAMPLE_TIMEOUT_MS;
  config.network.write_timeout_ms = WEB_EXAMPLE_TIMEOUT_MS;
  config.route_capacity = 2u;
  config.middleware_capacity = 1u;
  config.max_route_middleware_count = 1u;
  config.max_route_param_count = 1u;
  config.max_route_param_bytes = WEB_EXAMPLE_PATH_BYTES;
  config.max_target_bytes = WEB_EXAMPLE_PATH_BYTES;
  config.max_header_count = WEB_EXAMPLE_HEADERS;
  config.max_header_bytes = WEB_EXAMPLE_HEADER_BYTES;
  config.max_request_body_bytes = WEB_EXAMPLE_BODY_BYTES;
  config.max_response_header_count = WEB_EXAMPLE_HEADERS;
  config.max_response_header_bytes = WEB_EXAMPLE_HEADER_BYTES;
  config.max_response_body_bytes = WEB_EXAMPLE_BODY_BYTES;
  config.max_buffered_response_body_bytes = WEB_EXAMPLE_BODY_BYTES;
  config.buffer_capacity_bytes = WEB_EXAMPLE_BUFFER_CAPACITY_BYTES;
  config.poll_slice_ms = 1u;

  status = chttp_server_init(&server, &config);
  if (status == SALTS_OK)
    status = chttp_server_get(&server, "/", web_example_page, &context);
  if (status == SALTS_OK) status = chttp_server_start(&server);
  if (status == SALTS_OK) status = chttp_server_port(&server, &port);

  if (status == SALTS_OK) {
    printf("CHttp::Web example: http://127.0.0.1:%u/\n", (unsigned)port);
    fflush(stdout);
    (void)getchar();
  }

  if (server.impl) {
    const int stopped = chttp_server_stop(&server, WEB_EXAMPLE_TIMEOUT_MS);
    if (status == SALTS_OK) status = stopped;
    if (stopped == SALTS_OK) {
      const int destroyed = chttp_server_destroy(&server);
      if (status == SALTS_OK) status = destroyed;
    }
  }
  chttp_web_renderer_destroy(&renderer);

  if (status != SALTS_OK)
    fprintf(stderr, "CHttp::Web example failed: %d\n", status);
  return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
