#include <chttp_web/web.h>

#include <cmeta/struct.h>
#include <salts/error_codes.h>
#include <salts_cmeta_data.h>
#include <vstr.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  WEB_CONTEXT_TIMEOUT_MS = 5000,
  WEB_CONTEXT_CONNECTIONS = 8,
  WEB_CONTEXT_COMMANDS = 32,
  WEB_CONTEXT_HEADERS = 8,
  WEB_CONTEXT_HEADER_BYTES = 4096,
  WEB_CONTEXT_BODY_BYTES = 64 * 1024,
  WEB_CONTEXT_SEND_BYTES = 128 * 1024,
  WEB_CONTEXT_COMMAND_BUFFER_BYTES = 512 * 1024,
  WEB_CONTEXT_BUFFER_CAPACITY_BYTES = 2 * 1024 * 1024,
  WEB_CONTEXT_PATH_BYTES = 1024
};

static const cmeta_data_buffer_shape WEB_VSTR_SHAPE = {
    CMETA_DATA_BUFFER_BORROWED};

static const cmeta_data_desc WEB_VSTR_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "chttp.web.context.example.vstr.data",
    .display_name = "CHttp Web context example borrowed vstr",
    .kind = CMETA_DATA_STRING,
    .storage_type = &salts_vstr_cmeta_type,
    .shape = &WEB_VSTR_SHAPE,
    .buffer_ops = &salts_vstr_cmeta_buffer_ops};

typedef struct web_context_model {
  vstr title;
  chttp_web_request_context request;
} web_context_model;

typedef struct web_context_app {
  chttp_web_renderer *renderer;
  const cmeta_data_desc *model_desc;
} web_context_app;

static const cmeta_type_identity WEB_CONTEXT_MODEL_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.context.example.model");

static const cmeta_type_desc WEB_CONTEXT_MODEL_TYPE = {
    "web_context_model",
    sizeof(web_context_model),
    _Alignof(web_context_model),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &WEB_CONTEXT_MODEL_ID};

static const cmeta_field_desc WEB_CONTEXT_MODEL_LAYOUT_FIELDS[] = {
    {"title", "vstr", offsetof(web_context_model, title),
     sizeof(((web_context_model *)0)->title), _Alignof(vstr), NULL, NULL},
    {"request", "chttp_web_request_context",
     offsetof(web_context_model, request),
     sizeof(((web_context_model *)0)->request),
     _Alignof(chttp_web_request_context), NULL, NULL}};

static const cmeta_struct_desc WEB_CONTEXT_MODEL_LAYOUT = {
    "web_context_model",
    sizeof(web_context_model),
    _Alignof(web_context_model),
    WEB_CONTEXT_MODEL_LAYOUT_FIELDS,
    2u};

static cmeta_data_desc web_context_model_desc(
    cmeta_data_field_desc fields[2], cmeta_data_struct_shape *shape) {
  fields[0] = (cmeta_data_field_desc){
      "chttp.web.context.example.model.title", "title",
      offsetof(web_context_model, title), &WEB_VSTR_DATA};
  fields[1] = (cmeta_data_field_desc){
      "chttp.web.context.example.model.request", "request",
      offsetof(web_context_model, request), chttp_web_request_context_data()};
  *shape = (cmeta_data_struct_shape){
      &WEB_CONTEXT_MODEL_LAYOUT, fields, 2u};
  return (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "chttp.web.context.example.model.data",
      .display_name = "CHttp Web context example model",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &WEB_CONTEXT_MODEL_TYPE,
      .shape = shape};
}

static int web_context_prepare(
    const chttp_server_request_view *request,
    web_context_model *model,
    chttp_web_named_value param_storage[1],
    chttp_web_named_value header_storage[1],
    chttp_web_named_value session_storage[1],
    chttp_web_error *error) {
  static const char *const header_names[] = {"X-Request-ID"};
  static const char *const session_keys[] = {"theme"};
  chttp_web_request_context_options options =
      (chttp_web_request_context_options)
          CHTTP_WEB_REQUEST_CONTEXT_OPTIONS_INIT;
  const char *theme;

  if (request->session != NULL) {
    theme = chttp_server_request_header(request, "X-Theme");
    if (theme == NULL) theme = "light";
    if (chttp_session_set(request->session, "theme", theme) != SALTS_OK)
      return SALTS_EIO;
  }

  options.header_names = header_names;
  options.header_name_count = 1u;
  options.session_keys = session_keys;
  options.session_key_count = 1u;
  options.param_storage = param_storage;
  options.param_capacity = 1u;
  options.header_storage = header_storage;
  options.header_capacity = 1u;
  options.session_storage = session_storage;
  options.session_capacity = 1u;

  *model = (web_context_model){
      .title = vstr_from_cstr("<Admin & Users>")};

  return chttp_web_request_context_init(
             &model->request, request, &options, error) == CHTTP_WEB_OK
             ? SALTS_OK
             : SALTS_EIO;
}

static int web_context_users(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_context_app *app = (web_context_app *)user;
  chttp_web_named_value params[1];
  chttp_web_named_value headers[1];
  chttp_web_named_value session[1];
  web_context_model model;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  const char *template_name;

  if (web_context_prepare(
          request, &model, params, headers, session, &error) != SALTS_OK) {
    fprintf(stderr, "context failed: status=%d native=%d message=%s\n",
            (int)error.status, error.native_status, error.message);
    return SALTS_EIO;
  }

  if (model.request.htmx) {
    status = chttp_web_hx_trigger(response, "users:rendered", &error);
    if (status == CHTTP_WEB_OK)
      status = chttp_web_hx_retarget(response, "#users", &error);
    if (status != CHTTP_WEB_OK) {
      fprintf(stderr, "HX headers failed: status=%d native=%d message=%s\n",
              (int)error.status, error.native_status, error.message);
      return SALTS_EIO;
    }
    template_name = "fragment.html";
  } else {
    template_name = "page.html";
  }

  status = chttp_web_render_response(
      app->renderer, response, template_name, app->model_desc, &model,
      200u, NULL, &error);
  if (status != CHTTP_WEB_OK) {
    fprintf(stderr, "render failed: status=%d native=%d message=%s\n",
            (int)error.status, error.native_status, error.message);
    return SALTS_EIO;
  }
  return SALTS_OK;
}

static int web_context_redirect(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  (void)user;
  (void)request;

  status = chttp_web_redirect(response, 303u, "/users/redirected", &error);
  if (status != CHTTP_WEB_OK) {
    fprintf(stderr, "redirect failed: status=%d native=%d message=%s\n",
            (int)error.status, error.native_status, error.message);
    return SALTS_EIO;
  }
  return SALTS_OK;
}

static int web_context_error(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_context_app *app = (web_context_app *)user;
  chttp_web_named_value headers[1];
  chttp_web_named_value session[1];
  web_context_model model;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_request_context_options options =
      (chttp_web_request_context_options)
          CHTTP_WEB_REQUEST_CONTEXT_OPTIONS_INIT;
  chttp_web_status status;

  options.header_storage = headers;
  options.header_capacity = 1u;
  options.session_storage = session;
  options.session_capacity = 1u;
  model = (web_context_model){
      .title = vstr_from_cstr("<Not Found & Gone>")};

  status = chttp_web_request_context_init(
      &model.request, request, &options, &error);
  if (status == CHTTP_WEB_OK)
    status = chttp_web_render_error(
        app->renderer, response, 404u, "error.html",
        app->model_desc, &model, &error);
  if (status != CHTTP_WEB_OK) {
    fprintf(stderr, "error render failed: status=%d native=%d message=%s\n",
            (int)error.status, error.native_status, error.message);
    return SALTS_EIO;
  }
  return SALTS_OK;
}

static int web_context_hx_headers(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  (void)user;
  (void)request;

  status = chttp_web_hx_redirect(response, "/users/42", &error);
  if (status == CHTTP_WEB_OK)
    status = chttp_web_hx_trigger(response, "users:refresh", &error);
  if (status == CHTTP_WEB_OK)
    status = chttp_web_hx_retarget(response, "#users", &error);
  if (status != CHTTP_WEB_OK) {
    fprintf(stderr, "HX helper failed: status=%d native=%d message=%s\n",
            (int)error.status, error.native_status, error.message);
    return SALTS_EIO;
  }
  return chttp_server_reply(response, 204u, NULL, NULL, 0u);
}

static int web_context_hx_bounded(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  static const char *const fill_names[] = {
      "X-Fill-0", "X-Fill-1", "X-Fill-2",
      "X-Fill-3", "X-Fill-4", "X-Fill-5"};
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  size_t i;
  (void)user;
  (void)request;

  for (i = 0u; i < 6u; ++i)
    if (chttp_server_response_set_header(
            response, fill_names[i], "1") != SALTS_OK)
      return SALTS_EIO;

  status = chttp_web_hx_redirect(response, "/users/42", &error);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;
  status = chttp_web_hx_trigger(response, "users:refresh", &error);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;

  status = chttp_web_hx_retarget(response, "#users", &error);
  if (status != CHTTP_WEB_SERVER || error.native_status != SALTS_ENOBUFS) {
    fprintf(stderr,
            "expected bounded HX failure: status=%d native=%d message=%s\n",
            (int)status, error.native_status, error.message);
    return SALTS_EIO;
  }
  return chttp_server_reply(response, 204u, NULL, NULL, 0u);
}

int main(void) {
  static const char layout[] =
      "<!doctype html><html><body>{% block body %}{% endblock %}</body></html>";
  static const char page[] =
      "{% extends \"layout.html\" %}"
      "{% block body %}<main id=\"users\"><h1>{{ title }}</h1>"
      "<p class=\"kind\">full</p>"
      "<p>{{ request.method }} {{ request.path }}</p>"
      "<p class=\"id\">{{ request.params[0].value }}</p>"
      "<p class=\"rid\">{{ request.headers[0].value }}</p>"
      "<p class=\"theme\">{{ request.session[0].value }}</p>"
      "</main>{% endblock %}";
  static const char fragment[] =
      "<section id=\"users\"><h2>{{ title }}</h2>"
      "<p class=\"kind\">fragment</p>"
      "<p>{{ request.method }} {{ request.path }}</p>"
      "<p class=\"id\">{{ request.params[0].value }}</p>"
      "<p class=\"rid\">{{ request.headers[0].value }}</p>"
      "<p class=\"theme\">{{ request.session[0].value }}</p>"
      "</section>";
  static const char error_page[] =
      "<!doctype html><html><body><h1>404</h1>"
      "<p>{{ title }}</p><p>{{ request.path }}</p></body></html>";
  static const chttp_web_template templates[] = {
      {"layout.html", layout, sizeof(layout) - 1u},
      {"page.html", page, sizeof(page) - 1u},
      {"fragment.html", fragment, sizeof(fragment) - 1u},
      {"error.html", error_page, sizeof(error_page) - 1u}};

  chttp_web_renderer renderer = {0};
  chttp_web_renderer_config renderer_config =
      (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
  chttp_web_error web_error = CHTTP_WEB_ERROR_INIT;
  cmeta_data_field_desc fields[2];
  cmeta_data_struct_shape shape;
  cmeta_data_desc desc = web_context_model_desc(fields, &shape);
  web_context_app app = {&renderer, &desc};
  chttp_server server = {0};
  chttp_server_config config = {0};
  uint16_t port = 0u;
  int status;

  status = chttp_web_renderer_init(
      &renderer, templates, 4u, &renderer_config, &web_error);
  if (status != CHTTP_WEB_OK) {
    fprintf(stderr, "renderer init failed: %s\n", web_error.message);
    return EXIT_FAILURE;
  }

  config.host = "127.0.0.1";
  config.port = 0u;
  config.backlog = WEB_CONTEXT_CONNECTIONS;
#if defined(_WIN32)
  config.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  config.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
  config.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
  config.network.connection_capacity = WEB_CONTEXT_CONNECTIONS;
  config.network.command_capacity = WEB_CONTEXT_COMMANDS;
  config.network.request_capacity = WEB_CONTEXT_COMMANDS;
  config.network.completion_batch_capacity = WEB_CONTEXT_CONNECTIONS;
  config.network.event_capacity = WEB_CONTEXT_COMMANDS;
  config.network.max_send_bytes = WEB_CONTEXT_SEND_BYTES;
  config.network.command_buffer_bytes = WEB_CONTEXT_COMMAND_BUFFER_BYTES;
  config.network.receive_buffer_bytes = WEB_CONTEXT_HEADER_BYTES;
  config.network.connect_timeout_ms = WEB_CONTEXT_TIMEOUT_MS;
  config.network.read_timeout_ms = WEB_CONTEXT_TIMEOUT_MS;
  config.network.write_timeout_ms = WEB_CONTEXT_TIMEOUT_MS;
  config.route_capacity = 8u;
  config.middleware_capacity = 1u;
  config.max_route_middleware_count = 1u;
  config.max_route_param_count = 1u;
  config.max_route_param_bytes = WEB_CONTEXT_PATH_BYTES;
  config.max_target_bytes = WEB_CONTEXT_PATH_BYTES;
  config.max_header_count = WEB_CONTEXT_HEADERS;
  config.max_header_bytes = WEB_CONTEXT_HEADER_BYTES;
  config.max_request_body_bytes = WEB_CONTEXT_BODY_BYTES;
  config.max_response_header_count = WEB_CONTEXT_HEADERS;
  config.max_response_header_bytes = WEB_CONTEXT_HEADER_BYTES;
  config.max_response_body_bytes = WEB_CONTEXT_BODY_BYTES;
  config.max_buffered_response_body_bytes = WEB_CONTEXT_BODY_BYTES;
  config.buffer_capacity_bytes = WEB_CONTEXT_BUFFER_CAPACITY_BYTES;
  config.session_capacity = 8u;
  config.session_entry_capacity = 4u;
  config.max_session_key_bytes = 64u;
  config.max_session_value_bytes = 64u;
  config.session_idle_timeout_ms = 60000u;
  config.session_cookie_name = "chttp_web_session";
  config.poll_slice_ms = 1u;

  status = chttp_server_init(&server, &config);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/users/:id", web_context_users, &app);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/redirect", web_context_redirect, &app);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/error", web_context_error, &app);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/hx-headers", web_context_hx_headers, &app);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/hx-bounded", web_context_hx_bounded, &app);
  if (status == SALTS_OK) status = chttp_server_start(&server);
  if (status == SALTS_OK) status = chttp_server_port(&server, &port);

  if (status == SALTS_OK) {
    printf("CHttp::Web context example: http://127.0.0.1:%u/\n",
           (unsigned)port);
    fflush(stdout);
    (void)getchar();
  }

  if (server.impl) {
    const int stopped =
        chttp_server_stop(&server, WEB_CONTEXT_TIMEOUT_MS);
    if (status == SALTS_OK) status = stopped;
    if (stopped == SALTS_OK) {
      const int destroyed = chttp_server_destroy(&server);
      if (status == SALTS_OK) status = destroyed;
    }
  }
  chttp_web_renderer_destroy(&renderer);

  if (status != SALTS_OK)
    fprintf(stderr, "CHttp::Web context example failed: %d\n", status);
  return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
