#include <chttp_web/web.h>

#include <cmeta/struct.h>
#include <salts/error_codes.h>
#include <vstr.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  WEB_CRUD_TIMEOUT_MS = 5000,
  WEB_CRUD_CONNECTIONS = 8,
  WEB_CRUD_COMMANDS = 32,
  WEB_CRUD_HEADERS = 16,
  WEB_CRUD_HEADER_BYTES = 4096,
  WEB_CRUD_BODY_BYTES = 16 * 1024,
  WEB_CRUD_SEND_BYTES = 64 * 1024,
  WEB_CRUD_COMMAND_BUFFER_BYTES = 256 * 1024,
  WEB_CRUD_BUFFER_CAPACITY_BYTES = 1024 * 1024,
  WEB_CRUD_PATH_BYTES = 1024,
  WEB_CRUD_USER_CAPACITY = 8,
  WEB_CRUD_NAME_BYTES = 64,
  WEB_CRUD_ID_BYTES = 16,
  WEB_CRUD_FORM_PAIRS = 8,
  WEB_CRUD_FORM_BYTES = 2048
};

typedef struct web_crud_user_record {
  char id[WEB_CRUD_ID_BYTES];
  char name[WEB_CRUD_NAME_BYTES];
} web_crud_user_record;

typedef struct web_crud_user_view {
  vstr id;
  vstr name;
} web_crud_user_view;

typedef struct web_crud_model {
  chttp_web_request_context request;
  chttp_web_sequence_view users;
} web_crud_model;

typedef struct web_crud_app {
  chttp_web_renderer renderer;

  cmeta_data_field_desc user_fields[2];
  cmeta_data_struct_shape user_shape;
  cmeta_data_desc user_desc;

  cmeta_data_field_desc model_fields[2];
  cmeta_data_struct_shape model_shape;
  cmeta_data_desc model_desc;

  web_crud_user_record users[WEB_CRUD_USER_CAPACITY];
  size_t user_count;
  unsigned int next_id;
} web_crud_app;

static const cmeta_type_identity WEB_CRUD_USER_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.crud.example.user");

static const cmeta_type_desc WEB_CRUD_USER_TYPE = {
    "web_crud_user_view",
    sizeof(web_crud_user_view),
    _Alignof(web_crud_user_view),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &WEB_CRUD_USER_ID};

static const cmeta_field_desc WEB_CRUD_USER_LAYOUT_FIELDS[] = {
    {"id", "vstr", offsetof(web_crud_user_view, id),
     sizeof(((web_crud_user_view *)0)->id), _Alignof(vstr), NULL, NULL},
    {"name", "vstr", offsetof(web_crud_user_view, name),
     sizeof(((web_crud_user_view *)0)->name), _Alignof(vstr), NULL, NULL}};

static const cmeta_struct_desc WEB_CRUD_USER_LAYOUT = {
    "web_crud_user_view",
    sizeof(web_crud_user_view),
    _Alignof(web_crud_user_view),
    WEB_CRUD_USER_LAYOUT_FIELDS,
    2u};

static const cmeta_type_identity WEB_CRUD_MODEL_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.crud.example.model");

static const cmeta_type_desc WEB_CRUD_MODEL_TYPE = {
    "web_crud_model",
    sizeof(web_crud_model),
    _Alignof(web_crud_model),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &WEB_CRUD_MODEL_ID};

static const cmeta_field_desc WEB_CRUD_MODEL_LAYOUT_FIELDS[] = {
    {"request", "chttp_web_request_context",
     offsetof(web_crud_model, request),
     sizeof(((web_crud_model *)0)->request),
     _Alignof(chttp_web_request_context), NULL, NULL},
    {"users", "chttp_web_sequence_view",
     offsetof(web_crud_model, users),
     sizeof(((web_crud_model *)0)->users),
     _Alignof(chttp_web_sequence_view), NULL, NULL}};

static const cmeta_struct_desc WEB_CRUD_MODEL_LAYOUT = {
    "web_crud_model",
    sizeof(web_crud_model),
    _Alignof(web_crud_model),
    WEB_CRUD_MODEL_LAYOUT_FIELDS,
    2u};

static cmeta_data_desc web_crud_user_desc(
    cmeta_data_field_desc fields[2],
    cmeta_data_struct_shape *shape) {
  fields[0] = (cmeta_data_field_desc){
      "chttp.web.crud.example.user.id",
      "id",
      offsetof(web_crud_user_view, id),
      chttp_web_vstr_cmeta_data()};
  fields[1] = (cmeta_data_field_desc){
      "chttp.web.crud.example.user.name",
      "name",
      offsetof(web_crud_user_view, name),
      chttp_web_vstr_cmeta_data()};
  *shape = (cmeta_data_struct_shape){
      &WEB_CRUD_USER_LAYOUT,
      fields,
      2u};
  return (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "chttp.web.crud.example.user.data",
      .display_name = "CHttp Web CRUD example user",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &WEB_CRUD_USER_TYPE,
      .shape = shape};
}

static cmeta_data_desc web_crud_model_desc(
    cmeta_data_field_desc fields[2],
    cmeta_data_struct_shape *shape) {
  fields[0] = (cmeta_data_field_desc){
      "chttp.web.crud.example.model.request",
      "request",
      offsetof(web_crud_model, request),
      chttp_web_request_context_data()};
  fields[1] = (cmeta_data_field_desc){
      "chttp.web.crud.example.model.users",
      "users",
      offsetof(web_crud_model, users),
      chttp_web_sequence_cmeta_data()};
  *shape = (cmeta_data_struct_shape){
      &WEB_CRUD_MODEL_LAYOUT,
      fields,
      2u};
  return (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "chttp.web.crud.example.model.data",
      .display_name = "CHttp Web CRUD example model",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &WEB_CRUD_MODEL_TYPE,
      .shape = shape};
}

static native_io_backend_kind web_crud_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_server_config web_crud_server_config(void) {
  chttp_server_config config = {0};
  config.host = "127.0.0.1";
  config.port = 0u;
  config.backlog = WEB_CRUD_CONNECTIONS;
  config.network.backend = web_crud_backend();
  config.network.connection_capacity = WEB_CRUD_CONNECTIONS;
  config.network.command_capacity = WEB_CRUD_COMMANDS;
  config.network.request_capacity = WEB_CRUD_COMMANDS;
  config.network.completion_batch_capacity = WEB_CRUD_CONNECTIONS;
  config.network.event_capacity = WEB_CRUD_COMMANDS;
  config.network.max_send_bytes = WEB_CRUD_SEND_BYTES;
  config.network.command_buffer_bytes = WEB_CRUD_COMMAND_BUFFER_BYTES;
  config.network.receive_buffer_bytes = WEB_CRUD_HEADER_BYTES;
  config.network.connect_timeout_ms = WEB_CRUD_TIMEOUT_MS;
  config.network.read_timeout_ms = WEB_CRUD_TIMEOUT_MS;
  config.network.write_timeout_ms = WEB_CRUD_TIMEOUT_MS;
  config.route_capacity = 4u;
  config.middleware_capacity = 2u;
  config.max_route_middleware_count = 1u;
  config.max_route_param_count = 1u;
  config.max_route_param_bytes = WEB_CRUD_PATH_BYTES;
  config.max_target_bytes = WEB_CRUD_PATH_BYTES;
  config.max_header_count = WEB_CRUD_HEADERS;
  config.max_header_bytes = WEB_CRUD_HEADER_BYTES;
  config.max_request_body_bytes = WEB_CRUD_BODY_BYTES;
  config.max_response_header_count = WEB_CRUD_HEADERS;
  config.max_response_header_bytes = WEB_CRUD_HEADER_BYTES;
  config.max_response_body_bytes = WEB_CRUD_BODY_BYTES;
  config.max_buffered_response_body_bytes = WEB_CRUD_BODY_BYTES;
  config.buffer_capacity_bytes = WEB_CRUD_BUFFER_CAPACITY_BYTES;
  config.session_capacity = WEB_CRUD_CONNECTIONS;
  config.session_entry_capacity = 4u;
  config.max_session_key_bytes = 64u;
  config.max_session_value_bytes = 1024u;
  config.session_idle_timeout_ms = 60000u;
  config.session_cookie_name = "chttp_web_crud";
  config.poll_slice_ms = 1u;
  return config;
}

static int web_crud_content_type_form(
    const chttp_server_request_view *request) {
  static const char expected[] = "application/x-www-form-urlencoded";
  const char *content_type =
      chttp_server_request_header(request, "Content-Type");
  const size_t expected_size = sizeof(expected) - 1u;
  if (content_type == NULL ||
      strncmp(content_type, expected, expected_size) != 0)
    return 0;
  return content_type[expected_size] == '\0' ||
         content_type[expected_size] == ';';
}

static chttp_web_status web_crud_parse_form(
    const chttp_server_request_view *request,
    chttp_web_form_pair pairs[WEB_CRUD_FORM_PAIRS],
    char bytes[WEB_CRUD_FORM_BYTES],
    chttp_web_form *form,
    chttp_web_error *error) {
  chttp_web_form_parse_options options =
      (chttp_web_form_parse_options)CHTTP_WEB_FORM_PARSE_OPTIONS_INIT;
  options.max_input_bytes = WEB_CRUD_FORM_BYTES;
  options.max_pairs = WEB_CRUD_FORM_PAIRS;
  options.max_decoded_bytes = WEB_CRUD_FORM_BYTES;
  options.pair_storage = pairs;
  options.pair_capacity = WEB_CRUD_FORM_PAIRS;
  options.byte_storage = bytes;
  options.byte_capacity = WEB_CRUD_FORM_BYTES;
  return chttp_web_form_parse(
      request->body, request->body_size, &options, form, error);
}

static int web_crud_reply_text(
    chttp_server_response *response,
    unsigned int status_code,
    const char *text) {
  return chttp_server_reply(
      response,
      status_code,
      "text/plain; charset=utf-8",
      text,
      strlen(text));
}

static int web_crud_parse_and_validate(
    const chttp_server_request_view *request,
    chttp_web_form_pair pairs[WEB_CRUD_FORM_PAIRS],
    char bytes[WEB_CRUD_FORM_BYTES],
    chttp_web_form *form) {
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;

  if (!web_crud_content_type_form(request))
    return 415;

  status = web_crud_parse_form(request, pairs, bytes, form, &error);
  if (status != CHTTP_WEB_OK)
    return 400;

  status = chttp_web_csrf_validate(request, form, &error);
  if (status == CHTTP_WEB_CSRF)
    return 403;
  if (status != CHTTP_WEB_OK) {
    fprintf(
        stderr,
        "CRUD CSRF helper failed: status=%d native=%d message=%s\n",
        (int)status,
        error.native_status,
        error.message);
    return -1;
  }
  return 0;
}

static int web_crud_render(
    web_crud_app *app,
    const chttp_server_request_view *request,
    chttp_server_response *response,
    int fragment) {
  web_crud_user_view rows[WEB_CRUD_USER_CAPACITY];
  chttp_web_named_value params[1];
  chttp_web_request_context_options options =
      (chttp_web_request_context_options)
          CHTTP_WEB_REQUEST_CONTEXT_OPTIONS_INIT;
  web_crud_model model = {0};
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  const char *csrf = NULL;
  const char *template_name;
  chttp_web_status status;
  size_t i;

  status = chttp_web_csrf_ensure(request->session, &csrf, &error);
  if (status != CHTTP_WEB_OK || csrf == NULL) {
    fprintf(
        stderr,
        "CRUD CSRF ensure failed: status=%d native=%d message=%s\n",
        (int)status,
        error.native_status,
        error.message);
    return SALTS_EIO;
  }

  options.param_storage = params;
  options.param_capacity = 1u;
  status = chttp_web_request_context_init(
      &model.request, request, &options, &error);
  if (status != CHTTP_WEB_OK) {
    fprintf(
        stderr,
        "CRUD context failed: status=%d native=%d message=%s\n",
        (int)status,
        error.native_status,
        error.message);
    return SALTS_EIO;
  }

  for (i = 0u; i < app->user_count; ++i) {
    rows[i].id = vstr_from_cstr(app->users[i].id);
    rows[i].name = vstr_from_cstr(app->users[i].name);
  }
  model.users = (chttp_web_sequence_view){
      .data = rows,
      .count = app->user_count,
      .stride = sizeof(rows[0]),
      .element = &app->user_desc};

  template_name =
      fragment || model.request.htmx ? "users.html" : "page.html";
  status = chttp_web_render_response(
      &app->renderer,
      response,
      template_name,
      &app->model_desc,
      &model,
      200u,
      NULL,
      &error);
  if (status != CHTTP_WEB_OK) {
    fprintf(
        stderr,
        "CRUD render failed: status=%d native=%d message=%s\n",
        (int)status,
        error.native_status,
        error.message);
    return SALTS_EIO;
  }
  return SALTS_OK;
}

static int web_crud_after_mutation(
    web_crud_app *app,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;

  if (!chttp_web_request_is_htmx(request)) {
    status = chttp_web_redirect(response, 303u, "/users", &error);
    return status == CHTTP_WEB_OK ? SALTS_OK : SALTS_EIO;
  }

  status = chttp_web_hx_trigger(response, "users:changed", &error);
  if (status == CHTTP_WEB_OK)
    status = chttp_web_hx_retarget(response, "#users", &error);
  if (status != CHTTP_WEB_OK)
    return SALTS_EIO;

  return web_crud_render(app, request, response, 1);
}

static int web_crud_index(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  return web_crud_render(
      (web_crud_app *)user, request, response, 0);
}

static int web_crud_create(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_crud_app *app = (web_crud_app *)user;
  chttp_web_form_pair pairs[WEB_CRUD_FORM_PAIRS];
  char bytes[WEB_CRUD_FORM_BYTES];
  chttp_web_form form = {0};
  const chttp_web_form_pair *name;
  web_crud_user_record *record;
  int status;

  status = web_crud_parse_and_validate(
      request, pairs, bytes, &form);
  if (status > 0)
    return web_crud_reply_text(response, (unsigned int)status, "request rejected");
  if (status < 0)
    return SALTS_EIO;

  name = chttp_web_form_get(&form, "name", 0u);
  if (name == NULL ||
      name->value.size == 0u ||
      name->value.size >= WEB_CRUD_NAME_BYTES ||
      chttp_web_form_count(&form, "name") != 1u)
    return web_crud_reply_text(response, 400u, "invalid name");

  if (app->user_count == WEB_CRUD_USER_CAPACITY)
    return web_crud_reply_text(response, 409u, "user capacity reached");

  record = &app->users[app->user_count];
  if (snprintf(
          record->id,
          sizeof(record->id),
          "%u",
          app->next_id) <= 0)
    return SALTS_EIO;
  memcpy(record->name, name->value.data, name->value.size);
  record->name[name->value.size] = '\0';
  ++app->user_count;
  ++app->next_id;

  return web_crud_after_mutation(app, request, response);
}

static int web_crud_delete(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  web_crud_app *app = (web_crud_app *)user;
  chttp_web_form_pair pairs[WEB_CRUD_FORM_PAIRS];
  char bytes[WEB_CRUD_FORM_BYTES];
  chttp_web_form form = {0};
  const char *id;
  size_t i;
  int status;

  status = web_crud_parse_and_validate(
      request, pairs, bytes, &form);
  if (status > 0)
    return web_crud_reply_text(response, (unsigned int)status, "request rejected");
  if (status < 0)
    return SALTS_EIO;

  id = chttp_server_request_param(request, "id");
  if (id == NULL)
    return web_crud_reply_text(response, 400u, "missing id");

  for (i = 0u; i < app->user_count; ++i) {
    if (strcmp(app->users[i].id, id) == 0) {
      if (i + 1u < app->user_count)
        memmove(
            &app->users[i],
            &app->users[i + 1u],
            (app->user_count - i - 1u) * sizeof(app->users[0]));
      --app->user_count;
      return web_crud_after_mutation(app, request, response);
    }
  }

  return web_crud_reply_text(response, 404u, "user not found");
}

static int web_crud_seed(
    web_crud_app *app,
    const char *name) {
  web_crud_user_record *record;
  const size_t name_size = strlen(name);
  if (app->user_count == WEB_CRUD_USER_CAPACITY ||
      name_size == 0u ||
      name_size >= WEB_CRUD_NAME_BYTES)
    return SALTS_ENOBUFS;
  record = &app->users[app->user_count];
  if (snprintf(
          record->id,
          sizeof(record->id),
          "%u",
          app->next_id) <= 0)
    return SALTS_EIO;
  memcpy(record->name, name, name_size + 1u);
  ++app->user_count;
  ++app->next_id;
  return SALTS_OK;
}

int main(void) {
  static const char layout[] =
      "<!doctype html><html><head><meta charset=\"utf-8\">"
      "<title>CHttp::Web CRUD</title></head><body>"
      "{% block body %}{% endblock %}</body></html>";
  static const char page[] =
      "{% extends \"layout.html\" %}"
      "{% block body %}<main><h1>Users</h1>"
      "{% include \"users.html\" %}</main>{% endblock %}";
  static const char users[] =
      "<section id=\"users\">"
      "<ul>"
      "{% for user in users %}"
      "<li>{{ user.name }} "
      "<form method=\"post\" action=\"/users/{{ user.id }}/delete\" "
      "hx-post=\"/users/{{ user.id }}/delete\" "
      "hx-target=\"#users\" hx-swap=\"outerHTML\" "
      "style=\"display:inline\">"
      "<input type=\"hidden\" name=\"_csrf\" "
      "value=\"{{ request.csrf_token }}\">"
      "<button type=\"submit\">Delete</button></form></li>"
      "{% endfor %}"
      "</ul>"
      "<form method=\"post\" action=\"/users\" "
      "hx-post=\"/users\" hx-target=\"#users\" "
      "hx-swap=\"outerHTML\">"
      "<input type=\"hidden\" name=\"_csrf\" "
      "value=\"{{ request.csrf_token }}\">"
      "<label>Name <input name=\"name\" maxlength=\"63\" required></label>"
      "<button type=\"submit\">Add</button></form>"
      "</section>";
  static const chttp_web_template templates[] = {
      {"layout.html", layout, sizeof(layout) - 1u},
      {"page.html", page, sizeof(page) - 1u},
      {"users.html", users, sizeof(users) - 1u}};

  chttp_web_renderer_config renderer_config =
      (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
  chttp_web_security_policy security =
      chttp_web_security_reference_policy();
  chttp_web_error web_error = CHTTP_WEB_ERROR_INIT;
  web_crud_app app = {0};
  chttp_server server = {0};
  chttp_server_config config = web_crud_server_config();
  uint16_t port = 0u;
  int status;

  app.next_id = 1u;
  app.user_desc =
      web_crud_user_desc(app.user_fields, &app.user_shape);
  app.model_desc =
      web_crud_model_desc(app.model_fields, &app.model_shape);

  status = web_crud_seed(&app, "Ada");
  if (status == SALTS_OK)
    status = web_crud_seed(&app, "Linus");
  if (status != SALTS_OK)
    return EXIT_FAILURE;

  status = chttp_web_renderer_init(
      &app.renderer, templates, 3u, &renderer_config, &web_error);
  if (status != CHTTP_WEB_OK) {
    fprintf(stderr, "CRUD renderer init failed: %s\n", web_error.message);
    return EXIT_FAILURE;
  }

  status = chttp_server_init(&server, &config);
  if (status == SALTS_OK)
    status = chttp_web_security_use(&server, &security);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/users", web_crud_index, &app);
  if (status == SALTS_OK)
    status = chttp_server_post(
        &server, "/users", web_crud_create, &app);
  if (status == SALTS_OK)
    status = chttp_server_post(
        &server, "/users/:id/delete", web_crud_delete, &app);
  if (status == SALTS_OK)
    status = chttp_server_start(&server);
  if (status == SALTS_OK)
    status = chttp_server_port(&server, &port);

  if (status == SALTS_OK) {
    printf(
        "CHttp::Web CRUD example: http://127.0.0.1:%u/users\n",
        (unsigned int)port);
    printf(
        "The HTML works with ordinary forms and includes HTMX-compatible "
        "hx-* attributes for progressive enhancement.\n");
    fflush(stdout);
    (void)getchar();
  }

  if (server.impl) {
    const int stopped =
        chttp_server_stop(&server, WEB_CRUD_TIMEOUT_MS);
    if (status == SALTS_OK)
      status = stopped;
    if (stopped == SALTS_OK) {
      const int destroyed = chttp_server_destroy(&server);
      if (status == SALTS_OK)
        status = destroyed;
    }
  }
  chttp_web_renderer_destroy(&app.renderer);

  if (status != SALTS_OK)
    fprintf(stderr, "CHttp::Web CRUD example failed: %d\n", status);
  return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
