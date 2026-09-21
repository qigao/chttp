#include <chttp_web/web.h>
#include <http_client/http.h>

#include <cmeta/struct.h>
#include <salts/error_codes.h>
#include <vstr.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CHTTP_WEB_AUTH_APP_ASSET_ROOT
#define CHTTP_WEB_AUTH_APP_ASSET_ROOT "."
#endif

enum {
  AUTH_APP_TIMEOUT_MS = 5000,
  AUTH_APP_CONNECTIONS = 8,
  AUTH_APP_COMMANDS = 32,
  AUTH_APP_HEADERS = 24,
  AUTH_APP_HEADER_BYTES = 8192,
  AUTH_APP_BODY_BYTES = 16 * 1024,
  AUTH_APP_BUFFER_BYTES = 2 * 1024 * 1024,
  AUTH_APP_FORM_PAIRS = 8,
  AUTH_APP_FORM_BYTES = 2048,
  AUTH_APP_RETURN_BYTES = 256
};

typedef struct auth_app_identity {
  const char *username;
  const char *password;
  const char *subject;
  const char *role;
  const char *display_name;
} auth_app_identity;

static const auth_app_identity AUTH_APP_IDENTITIES[] = {
    {"alice", "secret", "alice", "admin", "<Admin & Alice>"}};

typedef struct auth_app_model {
  chttp_web_request_context request;
  chttp_web_principal principal;
  vstr return_to;
  vstr message;
} auth_app_model;

typedef struct auth_app {
  chttp_web_renderer renderer;
  cmeta_data_field_desc model_fields[4];
  cmeta_data_struct_shape model_shape;
  cmeta_data_desc model_desc;

  chttp_web_security_policy security;
  chttp_web_asset_mount assets;
  chttp_web_auth_policy dashboard_policy;
  chttp_web_auth_policy admin_policy;
  chttp_server_middleware dashboard_middleware;
  chttp_server_middleware admin_middleware;

  chttp_server server;
  uint16_t port;

  size_t dashboard_calls;
  size_t admin_terminal_calls;
  size_t forbidden_calls;
} auth_app;

static const cmeta_type_identity AUTH_APP_MODEL_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.authenticated.example.model");

static const cmeta_type_desc AUTH_APP_MODEL_TYPE = {
    "auth_app_model",
    sizeof(auth_app_model),
    _Alignof(auth_app_model),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &AUTH_APP_MODEL_ID};

static const cmeta_field_desc AUTH_APP_MODEL_LAYOUT_FIELDS[] = {
    {"request", "chttp_web_request_context",
     offsetof(auth_app_model, request),
     sizeof(((auth_app_model *)0)->request),
     _Alignof(chttp_web_request_context), NULL, NULL},
    {"principal", "chttp_web_principal",
     offsetof(auth_app_model, principal),
     sizeof(((auth_app_model *)0)->principal),
     _Alignof(chttp_web_principal), NULL, NULL},
    {"return_to", "vstr", offsetof(auth_app_model, return_to),
     sizeof(((auth_app_model *)0)->return_to), _Alignof(vstr), NULL, NULL},
    {"message", "vstr", offsetof(auth_app_model, message),
     sizeof(((auth_app_model *)0)->message), _Alignof(vstr), NULL, NULL}};

static const cmeta_struct_desc AUTH_APP_MODEL_LAYOUT = {
    "auth_app_model",
    sizeof(auth_app_model),
    _Alignof(auth_app_model),
    AUTH_APP_MODEL_LAYOUT_FIELDS,
    4u};

static cmeta_data_desc auth_app_model_desc(
    cmeta_data_field_desc fields[4],
    cmeta_data_struct_shape *shape) {
  fields[0] = (cmeta_data_field_desc){
      "chttp.web.authenticated.example.model.request",
      "request",
      offsetof(auth_app_model, request),
      chttp_web_request_context_data()};
  fields[1] = (cmeta_data_field_desc){
      "chttp.web.authenticated.example.model.principal",
      "principal",
      offsetof(auth_app_model, principal),
      chttp_web_principal_data()};
  fields[2] = (cmeta_data_field_desc){
      "chttp.web.authenticated.example.model.return_to",
      "return_to",
      offsetof(auth_app_model, return_to),
      chttp_web_vstr_cmeta_data()};
  fields[3] = (cmeta_data_field_desc){
      "chttp.web.authenticated.example.model.message",
      "message",
      offsetof(auth_app_model, message),
      chttp_web_vstr_cmeta_data()};
  *shape = (cmeta_data_struct_shape){
      &AUTH_APP_MODEL_LAYOUT, fields, 4u};
  return (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "chttp.web.authenticated.example.model.data",
      .display_name = "CHttp authenticated example model",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &AUTH_APP_MODEL_TYPE,
      .shape = shape};
}

static native_io_backend_kind auth_app_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config auth_app_network(size_t connections) {
  return (cnet_client_config){
      .backend = auth_app_backend(),
      .connection_capacity = connections,
      .command_capacity = AUTH_APP_COMMANDS,
      .request_capacity = AUTH_APP_COMMANDS,
      .completion_batch_capacity = connections,
      .event_capacity = AUTH_APP_COMMANDS,
      .max_send_bytes = 128u * 1024u,
      .receive_buffer_bytes = 4096u,
      .connect_timeout_ms = AUTH_APP_TIMEOUT_MS,
      .read_timeout_ms = AUTH_APP_TIMEOUT_MS,
      .write_timeout_ms = AUTH_APP_TIMEOUT_MS};
}

static chttp_server_config auth_app_server_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = AUTH_APP_CONNECTIONS,
      .network = auth_app_network(AUTH_APP_CONNECTIONS),
      .route_capacity = 8u,
      .middleware_capacity = 2u,
      .max_route_middleware_count = 1u,
      .max_route_param_count = 1u,
      .max_route_param_bytes = 128u,
      .max_target_bytes = 1024u,
      .max_header_count = AUTH_APP_HEADERS,
      .max_header_bytes = AUTH_APP_HEADER_BYTES,
      .max_request_body_bytes = AUTH_APP_BODY_BYTES,
      .max_response_header_count = AUTH_APP_HEADERS,
      .max_response_header_bytes = AUTH_APP_HEADER_BYTES,
      .max_response_body_bytes = AUTH_APP_BODY_BYTES,
      .max_buffered_response_body_bytes = AUTH_APP_BODY_BYTES,
      .buffer_capacity_bytes = AUTH_APP_BUFFER_BYTES,
      .session_capacity = AUTH_APP_CONNECTIONS,
      .session_entry_capacity = 8u,
      .max_session_key_bytes = 64u,
      .max_session_value_bytes = 1024u,
      .session_idle_timeout_ms = 60000u,
      .session_cookie_name = "chttp_auth_app",
      .session_cookie_secure = 0,
      .poll_slice_ms = 1u,
      .enable_http2 = 1,
      .h2_stream_capacity = 8u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_output_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
}

static chttp_client_config auth_app_client_config(void) {
  return (chttp_client_config){
      .network = auth_app_network(4u),
      .request_capacity = 4u,
      .max_start_line_bytes = 1024u,
      .max_header_count = AUTH_APP_HEADERS,
      .max_header_bytes = AUTH_APP_HEADER_BYTES,
      .max_request_body_bytes = AUTH_APP_BODY_BYTES,
      .max_response_body_bytes = AUTH_APP_BODY_BYTES,
      .max_informational_responses = 2u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
}

static int auth_app_content_type_form(
    const chttp_server_request_view *request) {
  static const char expected[] = "application/x-www-form-urlencoded";
  const char *value = chttp_server_request_header(request, "Content-Type");
  const size_t expected_size = sizeof(expected) - 1u;
  return value != NULL &&
         strncmp(value, expected, expected_size) == 0 &&
         (value[expected_size] == '\0' || value[expected_size] == ';');
}

static chttp_web_status auth_app_parse_form(
    const void *data,
    size_t data_size,
    chttp_web_form_pair pairs[AUTH_APP_FORM_PAIRS],
    char bytes[AUTH_APP_FORM_BYTES],
    chttp_web_form *form,
    chttp_web_error *error) {
  chttp_web_form_parse_options options =
      (chttp_web_form_parse_options)CHTTP_WEB_FORM_PARSE_OPTIONS_INIT;
  options.max_input_bytes = AUTH_APP_FORM_BYTES;
  options.max_pairs = AUTH_APP_FORM_PAIRS;
  options.max_decoded_bytes = AUTH_APP_FORM_BYTES;
  options.pair_storage = pairs;
  options.pair_capacity = AUTH_APP_FORM_PAIRS;
  options.byte_storage = bytes;
  options.byte_capacity = AUTH_APP_FORM_BYTES;
  return chttp_web_form_parse(
      data, data_size, &options, form, error);
}

static int auth_app_copy_view(
    char *output,
    size_t capacity,
    chttp_web_string_view value) {
  if (output == NULL || capacity == 0u ||
      value.size >= capacity ||
      (value.size != 0u && value.data == NULL))
    return SALTS_EMSGSIZE;
  if (value.size != 0u) memcpy(output, value.data, value.size);
  output[value.size] = '\0';
  return SALTS_OK;
}

static int auth_app_validate_return_view(
    chttp_web_string_view value,
    char output[AUTH_APP_RETURN_BYTES + 1u]) {
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  if (value.size == 0u ||
      value.size > AUTH_APP_RETURN_BYTES ||
      auth_app_copy_view(output, AUTH_APP_RETURN_BYTES + 1u, value) != SALTS_OK)
    return SALTS_EINVAL;
  return chttp_web_local_target_validate(
             output, value.size, AUTH_APP_RETURN_BYTES, &error) ==
             CHTTP_WEB_OK
             ? SALTS_OK
             : SALTS_EPERM;
}

static int auth_app_return_from_form(
    const chttp_web_form *form,
    char output[AUTH_APP_RETURN_BYTES + 1u]) {
  const chttp_web_form_pair *pair;
  if (form == NULL || output == NULL) return SALTS_EINVAL;
  if (chttp_web_form_count(form, "return_to") == 0u) {
    memcpy(output, "/dashboard", sizeof("/dashboard"));
    return SALTS_OK;
  }
  if (chttp_web_form_count(form, "return_to") != 1u)
    return SALTS_EINVAL;
  pair = chttp_web_form_get(form, "return_to", 0u);
  return pair != NULL
             ? auth_app_validate_return_view(pair->value, output)
             : SALTS_EINVAL;
}

static int auth_app_return_from_query(
    const chttp_server_request_view *request,
    char output[AUTH_APP_RETURN_BYTES + 1u]) {
  chttp_web_form_pair pairs[AUTH_APP_FORM_PAIRS];
  char bytes[AUTH_APP_FORM_BYTES];
  chttp_web_form form = {0};
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  const char *query;
  chttp_web_status status;

  if (request == NULL || request->target == NULL || output == NULL)
    return SALTS_EINVAL;
  query = strchr(request->target, '?');
  if (query == NULL) {
    memcpy(output, "/dashboard", sizeof("/dashboard"));
    return SALTS_OK;
  }
  ++query;
  if (*query == '\0') return SALTS_EINVAL;
  status = auth_app_parse_form(
      query, strlen(query), pairs, bytes, &form, &error);
  if (status != CHTTP_WEB_OK || form.pair_count != 1u)
    return SALTS_EINVAL;
  return auth_app_return_from_form(&form, output);
}

static const auth_app_identity *auth_app_verify_credentials(
    const chttp_web_form *form) {
  const chttp_web_form_pair *username;
  const chttp_web_form_pair *password;
  size_t index;

  if (form == NULL ||
      chttp_web_form_count(form, "username") != 1u ||
      chttp_web_form_count(form, "password") != 1u)
    return NULL;
  username = chttp_web_form_get(form, "username", 0u);
  password = chttp_web_form_get(form, "password", 0u);
  if (username == NULL || password == NULL) return NULL;

  for (index = 0u;
       index < sizeof(AUTH_APP_IDENTITIES) / sizeof(AUTH_APP_IDENTITIES[0]);
       ++index) {
    const auth_app_identity *identity = &AUTH_APP_IDENTITIES[index];
    const size_t username_size = strlen(identity->username);
    const size_t password_size = strlen(identity->password);
    if (username->value.size == username_size &&
        password->value.size == password_size &&
        memcmp(username->value.data, identity->username, username_size) == 0 &&
        memcmp(password->value.data, identity->password, password_size) == 0)
      return identity;
  }
  return NULL;
}

static int auth_app_render(
    auth_app *app,
    const chttp_server_request_view *request,
    chttp_server_response *response,
    const char *template_name,
    unsigned int status_code,
    const char *return_to,
    const char *message) {
  chttp_web_request_context_options options =
      (chttp_web_request_context_options)
          CHTTP_WEB_REQUEST_CONTEXT_OPTIONS_INIT;
  auth_app_model model = {0};
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  const char *csrf = NULL;
  chttp_web_status status;

  if (app == NULL || request == NULL || response == NULL ||
      request->session == NULL || template_name == NULL)
    return SALTS_EINVAL;

  status = chttp_web_csrf_ensure(
      request->session, &csrf, &error);
  if (status != CHTTP_WEB_OK || csrf == NULL) return SALTS_EIO;
  status = chttp_web_request_context_init(
      &model.request, request, &options, &error);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;
  status = chttp_web_principal_get(
      request, &model.principal, &error);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;

  model.return_to = vstr_from_cstr(return_to != NULL ? return_to : "");
  model.message = vstr_from_cstr(message != NULL ? message : "");
  status = chttp_web_render_response(
      &app->renderer,
      response,
      template_name,
      &app->model_desc,
      &model,
      status_code,
      NULL,
      &error);
  if (status == CHTTP_WEB_OK) return SALTS_OK;
  return error.native_status != 0 ? error.native_status : SALTS_EIO;
}

static int auth_app_login_get(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  auth_app *app = (auth_app *)user;
  char return_to[AUTH_APP_RETURN_BYTES + 1u];
  if (app == NULL || request == NULL)
    return SALTS_EINVAL;
  if (auth_app_return_from_query(request, return_to) != SALTS_OK)
    return chttp_server_reply(
        response, 400u, "text/plain; charset=utf-8",
        "invalid return target", sizeof("invalid return target") - 1u);
  return auth_app_render(
      app, request, response,
      chttp_web_request_is_htmx(request)
          ? "login-fragment.html"
          : "login.html",
      200u, return_to, "");
}

static int auth_app_login_post(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  auth_app *app = (auth_app *)user;
  chttp_web_form_pair pairs[AUTH_APP_FORM_PAIRS];
  char bytes[AUTH_APP_FORM_BYTES];
  chttp_web_form form = {0};
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  const auth_app_identity *identity;
  chttp_web_principal_input principal =
      (chttp_web_principal_input)CHTTP_WEB_PRINCIPAL_INPUT_INIT;
  const char *rotated = NULL;
  char return_to[AUTH_APP_RETURN_BYTES + 1u];
  chttp_web_status status;

  if (app == NULL || request == NULL) return SALTS_EINVAL;
  if (!auth_app_content_type_form(request))
    return chttp_server_reply(response, 415u, NULL, NULL, 0u);

  status = auth_app_parse_form(
      request->body, request->body_size,
      pairs, bytes, &form, &error);
  if (status != CHTTP_WEB_OK)
    return chttp_server_reply(response, 400u, NULL, NULL, 0u);
  if (auth_app_return_from_form(&form, return_to) != SALTS_OK)
    return chttp_server_reply(
        response, 400u, "text/plain; charset=utf-8",
        "invalid return target", sizeof("invalid return target") - 1u);

  status = chttp_web_csrf_validate(request, &form, &error);
  if (status == CHTTP_WEB_CSRF)
    return chttp_server_reply(response, 403u, NULL, NULL, 0u);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;

  identity = auth_app_verify_credentials(&form);
  if (identity == NULL)
    return auth_app_render(
        app, request, response,
        chttp_web_request_is_htmx(request)
            ? "login-fragment.html"
            : "login.html",
        401u, return_to, "invalid credentials");

  principal.subject = identity->subject;
  principal.role = identity->role;
  principal.display_name = identity->display_name;
  status = chttp_web_principal_sign_in(
      request, &form, &principal, &rotated, &error);
  if (status != CHTTP_WEB_OK || rotated == NULL)
    return SALTS_EIO;

  if (chttp_web_request_is_htmx(request)) {
    status = chttp_web_hx_redirect(response, return_to, &error);
    if (status != CHTTP_WEB_OK)
      return error.native_status != 0 ? error.native_status : SALTS_EIO;
    return chttp_server_reply(response, 200u, NULL, NULL, 0u);
  }
  status = chttp_web_redirect(
      response, 303u, return_to, &error);
  return status == CHTTP_WEB_OK
             ? SALTS_OK
             : (error.native_status != 0 ? error.native_status : SALTS_EIO);
}

static int auth_app_role_equals(
    const chttp_web_principal *principal,
    const char *role) {
  const size_t role_size = strlen(role);
  return principal != NULL &&
         principal->role.data != NULL &&
         principal->role.size == role_size &&
         memcmp(principal->role.data, role, role_size) == 0;
}

static int auth_app_authorize_admin(
    void *user,
    const chttp_web_principal *principal,
    const chttp_server_request_view *request) {
  (void)user;
  (void)request;
  return auth_app_role_equals(principal, "admin")
             ? SALTS_OK
             : SALTS_EPERM;
}

static int auth_app_authorize_superadmin(
    void *user,
    const chttp_web_principal *principal,
    const chttp_server_request_view *request) {
  (void)user;
  (void)request;
  return auth_app_role_equals(principal, "superadmin")
             ? SALTS_OK
             : SALTS_EPERM;
}

static int auth_app_dashboard(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  auth_app *app = (auth_app *)user;
  if (app == NULL) return SALTS_EINVAL;
  ++app->dashboard_calls;
  return auth_app_render(
      app, request, response,
      chttp_web_request_is_htmx(request)
          ? "dashboard-fragment.html"
          : "dashboard.html",
      200u, "", "");
}

static int auth_app_admin_terminal(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  auth_app *app = (auth_app *)user;
  (void)request;
  if (app == NULL) return SALTS_EINVAL;
  ++app->admin_terminal_calls;
  return chttp_server_reply(
      response, 200u, "text/plain", "unexpected", 10u);
}

static int auth_app_forbidden(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  auth_app *app = (auth_app *)user;
  if (app == NULL) return SALTS_EINVAL;
  ++app->forbidden_calls;
  return auth_app_render(
      app, request, response,
      chttp_web_request_is_htmx(request)
          ? "forbidden-fragment.html"
          : "forbidden.html",
      403u, "", "authorization denied");
}

static int auth_app_logout_post(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_form_pair pairs[AUTH_APP_FORM_PAIRS];
  char bytes[AUTH_APP_FORM_BYTES];
  chttp_web_form form = {0};
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;

  (void)user;
  if (request == NULL) return SALTS_EINVAL;
  if (!auth_app_content_type_form(request))
    return chttp_server_reply(response, 415u, NULL, NULL, 0u);
  status = auth_app_parse_form(
      request->body, request->body_size,
      pairs, bytes, &form, &error);
  if (status != CHTTP_WEB_OK)
    return chttp_server_reply(response, 400u, NULL, NULL, 0u);

  status = chttp_web_principal_sign_out(request, &form, &error);
  if (status == CHTTP_WEB_CSRF)
    return chttp_server_reply(response, 403u, NULL, NULL, 0u);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;

  if (chttp_web_request_is_htmx(request)) {
    status = chttp_web_hx_redirect(response, "/login", &error);
    if (status != CHTTP_WEB_OK)
      return error.native_status != 0 ? error.native_status : SALTS_EIO;
    return chttp_server_reply(response, 200u, NULL, NULL, 0u);
  }
  status = chttp_web_redirect(response, 303u, "/login", &error);
  return status == CHTTP_WEB_OK
             ? SALTS_OK
             : (error.native_status != 0 ? error.native_status : SALTS_EIO);
}

static int auth_app_root(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  (void)user;
  (void)request;
  status = chttp_web_redirect(response, 303u, "/dashboard", &error);
  return status == CHTTP_WEB_OK
             ? SALTS_OK
             : (error.native_status != 0 ? error.native_status : SALTS_EIO);
}

static int auth_app_start(auth_app *app, const char *asset_root) {
  static const char login[] =
      "<!doctype html><html><head><meta charset=\"utf-8\">"
      "<title>CHttp Login</title><link rel=\"stylesheet\" href=\"/assets/app.css\">"
      "</head><body><main id=\"app\"><h1>Sign in</h1><p class=\"error\">{{ message }}</p>"
      "<form method=\"post\" action=\"/login\" hx-post=\"/login\" hx-target=\"#app\">"
      "<input type=\"hidden\" name=\"_csrf\" value=\"{{ request.csrf_token }}\">"
      "<input type=\"hidden\" name=\"return_to\" value=\"{{ return_to }}\">"
      "<label>User <input name=\"username\" autocomplete=\"username\"></label>"
      "<label>Password <input type=\"password\" name=\"password\" autocomplete=\"current-password\"></label>"
      "<button type=\"submit\">Sign in</button></form></main></body></html>";
  static const char login_fragment[] =
      "<main id=\"app\"><h1>Sign in</h1><p class=\"error\">{{ message }}</p>"
      "<form method=\"post\" action=\"/login\" hx-post=\"/login\" hx-target=\"#app\">"
      "<input type=\"hidden\" name=\"_csrf\" value=\"{{ request.csrf_token }}\">"
      "<input type=\"hidden\" name=\"return_to\" value=\"{{ return_to }}\">"
      "<label>User <input name=\"username\"></label>"
      "<label>Password <input type=\"password\" name=\"password\"></label>"
      "<button type=\"submit\">Sign in</button></form></main>";
  static const char dashboard[] =
      "<!doctype html><html><head><meta charset=\"utf-8\">"
      "<title>Dashboard</title><link rel=\"stylesheet\" href=\"/assets/app.css\">"
      "</head><body><main id=\"app\"><h1>Dashboard</h1>"
      "<p id=\"subject\">{{ principal.subject }}</p>"
      "<p id=\"display\">{{ principal.display_name }}</p>"
      "<form method=\"post\" action=\"/logout\" hx-post=\"/logout\">"
      "<input type=\"hidden\" name=\"_csrf\" value=\"{{ request.csrf_token }}\">"
      "<button type=\"submit\">Sign out</button></form></main></body></html>";
  static const char dashboard_fragment[] =
      "<main id=\"app\"><h1>Dashboard</h1>"
      "<p id=\"subject\">{{ principal.subject }}</p>"
      "<p id=\"display\">{{ principal.display_name }}</p>"
      "<form method=\"post\" action=\"/logout\" hx-post=\"/logout\">"
      "<input type=\"hidden\" name=\"_csrf\" value=\"{{ request.csrf_token }}\">"
      "<button type=\"submit\">Sign out</button></form></main>";
  static const char forbidden[] =
      "<!doctype html><html><head><meta charset=\"utf-8\"><title>Forbidden</title>"
      "</head><body><main id=\"app\"><h1>Forbidden</h1>"
      "<p>{{ message }}</p><p>{{ principal.display_name }}</p></main></body></html>";
  static const char forbidden_fragment[] =
      "<main id=\"app\"><h1>Forbidden</h1>"
      "<p>{{ message }}</p><p>{{ principal.display_name }}</p></main>";
  static const chttp_web_template templates[] = {
      {"login.html", login, sizeof(login) - 1u},
      {"login-fragment.html", login_fragment, sizeof(login_fragment) - 1u},
      {"dashboard.html", dashboard, sizeof(dashboard) - 1u},
      {"dashboard-fragment.html", dashboard_fragment,
       sizeof(dashboard_fragment) - 1u},
      {"forbidden.html", forbidden, sizeof(forbidden) - 1u},
      {"forbidden-fragment.html", forbidden_fragment,
       sizeof(forbidden_fragment) - 1u}};
  chttp_web_renderer_config renderer_config =
      (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_server_config server_config = auth_app_server_config();
  chttp_server_route_options dashboard_route = {0};
  chttp_server_route_options admin_route = {0};
  chttp_server_route_options logout_route = {0};
  int status;

  if (app == NULL || asset_root == NULL || asset_root[0] == '\0')
    return SALTS_EINVAL;
  memset(app, 0, sizeof(*app));
  app->model_desc =
      auth_app_model_desc(app->model_fields, &app->model_shape);

  if (chttp_web_renderer_init(
          &app->renderer, templates,
          sizeof(templates) / sizeof(templates[0]),
          &renderer_config, &error) != CHTTP_WEB_OK)
    return SALTS_EIO;

  app->security = chttp_web_security_authenticated_policy();
  app->assets =
      (chttp_web_asset_mount)CHTTP_WEB_ASSET_MOUNT_INIT;
  app->assets.url_prefix = "/assets";
  app->assets.filesystem_root = asset_root;
  app->assets.max_relative_path_bytes = 256u;
  app->assets.cache_control = "public, max-age=60";

  app->dashboard_policy =
      (chttp_web_auth_policy)CHTTP_WEB_AUTH_POLICY_INIT;
  app->dashboard_policy.login_path = "/login";
  app->dashboard_policy.include_return_target = true;
  app->dashboard_policy.max_return_target_bytes = AUTH_APP_RETURN_BYTES;
  app->dashboard_policy.authorize = auth_app_authorize_admin;
  app->dashboard_policy.authorize_user = app;

  app->admin_policy =
      (chttp_web_auth_policy)CHTTP_WEB_AUTH_POLICY_INIT;
  app->admin_policy.login_path = "/login";
  app->admin_policy.include_return_target = true;
  app->admin_policy.max_return_target_bytes = AUTH_APP_RETURN_BYTES;
  app->admin_policy.authorize = auth_app_authorize_superadmin;
  app->admin_policy.authorize_user = app;
  app->admin_policy.forbidden = auth_app_forbidden;
  app->admin_policy.forbidden_user = app;

  app->dashboard_middleware = (chttp_server_middleware){
      chttp_web_auth_middleware, &app->dashboard_policy};
  app->admin_middleware = (chttp_server_middleware){
      chttp_web_auth_middleware, &app->admin_policy};

  dashboard_route = (chttp_server_route_options){
      .method = CHTTP_METHOD_GET,
      .path = "/dashboard",
      .middleware = &app->dashboard_middleware,
      .middleware_count = 1u,
      .handler = auth_app_dashboard,
      .user = app};
  admin_route = (chttp_server_route_options){
      .method = CHTTP_METHOD_GET,
      .path = "/admin-only",
      .middleware = &app->admin_middleware,
      .middleware_count = 1u,
      .handler = auth_app_admin_terminal,
      .user = app};
  logout_route = (chttp_server_route_options){
      .method = CHTTP_METHOD_POST,
      .path = "/logout",
      .middleware = &app->dashboard_middleware,
      .middleware_count = 1u,
      .handler = auth_app_logout_post,
      .user = app};

  status = chttp_server_init(&app->server, &server_config);
  if (status == SALTS_OK)
    status = chttp_web_security_use(&app->server, &app->security);
  if (status == SALTS_OK)
    status = chttp_web_assets_use(&app->server, &app->assets);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &app->server, "/", auth_app_root, app);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &app->server, "/login", auth_app_login_get, app);
  if (status == SALTS_OK)
    status = chttp_server_post(
        &app->server, "/login", auth_app_login_post, app);
  if (status == SALTS_OK)
    status = chttp_server_route_with(
        &app->server, &dashboard_route);
  if (status == SALTS_OK)
    status = chttp_server_route_with(
        &app->server, &admin_route);
  if (status == SALTS_OK)
    status = chttp_server_route_with(
        &app->server, &logout_route);
  if (status == SALTS_OK)
    status = chttp_server_start(&app->server);
  if (status == SALTS_OK)
    status = chttp_server_port(&app->server, &app->port);

  if (status != SALTS_OK) {
    (void)chttp_server_destroy(&app->server);
    chttp_web_renderer_destroy(&app->renderer);
  }
  return status;
}

static int auth_app_stop(auth_app *app) {
  int stop_status;
  int destroy_status;
  if (app == NULL) return SALTS_EINVAL;
  stop_status = chttp_server_stop(&app->server, AUTH_APP_TIMEOUT_MS);
  destroy_status = chttp_server_destroy(&app->server);
  chttp_web_renderer_destroy(&app->renderer);
  return stop_status != SALTS_OK ? stop_status : destroy_status;
}

static const char *auth_app_find_bytes(
    const void *data,
    size_t data_size,
    const char *needle) {
  const unsigned char *bytes = (const unsigned char *)data;
  const size_t needle_size = strlen(needle);
  size_t offset;
  if (data == NULL || needle == NULL || needle_size == 0u ||
      data_size < needle_size)
    return NULL;
  for (offset = 0u; offset <= data_size - needle_size; ++offset)
    if (memcmp(bytes + offset, needle, needle_size) == 0)
      return (const char *)(bytes + offset);
  return NULL;
}

static int auth_app_body_contains(
    const chttp_response *response,
    const char *needle) {
  return response != NULL &&
         auth_app_find_bytes(
             response->body, response->body_size, needle) != NULL;
}

static int auth_app_extract_csrf(
    const chttp_response *response,
    char token[CHTTP_WEB_CSRF_TOKEN_BYTES + 1u]) {
  static const char marker[] = "name=\"_csrf\" value=\"";
  const char *found;
  const char *value;
  size_t remaining;

  if (response == NULL || token == NULL) return SALTS_EINVAL;
  found = auth_app_find_bytes(
      response->body, response->body_size, marker);
  if (found == NULL) return SALTS_EPROTO;
  value = found + sizeof(marker) - 1u;
  remaining =
      response->body_size - (size_t)(value - (const char *)response->body);
  if (remaining <= CHTTP_WEB_CSRF_TOKEN_BYTES ||
      value[CHTTP_WEB_CSRF_TOKEN_BYTES] != '"')
    return SALTS_EPROTO;
  memcpy(token, value, CHTTP_WEB_CSRF_TOKEN_BYTES);
  token[CHTTP_WEB_CSRF_TOKEN_BYTES] = '\0';
  return SALTS_OK;
}

static int auth_app_copy_cookie(
    const chttp_response *response,
    char *cookie,
    size_t capacity) {
  const char *set_cookie = chttp_response_header(response, "Set-Cookie");
  const char *end;
  size_t size;
  if (set_cookie == NULL || cookie == NULL || capacity == 0u)
    return SALTS_EINVAL;
  end = strchr(set_cookie, ';');
  size = end != NULL ? (size_t)(end - set_cookie) : strlen(set_cookie);
  if (size == 0u || size >= capacity) return SALTS_EMSGSIZE;
  memcpy(cookie, set_cookie, size);
  cookie[size] = '\0';
  return SALTS_OK;
}

static int auth_app_call(
    chttp_client *client,
    const char *uri,
    chttp_method method,
    const char *target,
    const char *cookie,
    bool htmx,
    chttp_protocol protocol,
    const char *body,
    chttp_response *response) {
  chttp_header headers[3];
  size_t header_count = 0u;
  chttp_options options = {0};
  chttp_error error = {0};

  if (cookie != NULL)
    headers[header_count++] = (chttp_header){"Cookie", cookie};
  if (htmx)
    headers[header_count++] = (chttp_header){"HX-Request", "true"};
  if (method == CHTTP_METHOD_POST)
    headers[header_count++] =
        (chttp_header){"Content-Type", "application/x-www-form-urlencoded"};

  options.connection_uri = uri;
  options.authority = "127.0.0.1";
  options.target = target;
  options.headers = header_count != 0u ? headers : NULL;
  options.header_count = header_count;
  options.body = body;
  options.body_size = body != NULL ? strlen(body) : 0u;
  options.timeout_ms = AUTH_APP_TIMEOUT_MS;
  options.protocol = protocol;

  if (method == CHTTP_METHOD_GET)
    return chttp_get(client, &options, response, &error);
  if (method == CHTTP_METHOD_POST)
    return chttp_post(client, &options, response, &error);
  return SALTS_EINVAL;
}

static int auth_app_header_equals(
    const chttp_response *response,
    const char *name,
    const char *expected) {
  const char *value = chttp_response_header(response, name);
  return value != NULL && strcmp(value, expected) == 0;
}

static int auth_app_self_test(auth_app *app) {
  chttp_client client = {0};
  chttp_client_config client_config = auth_app_client_config();
  chttp_response response = {0};
  char uri[64];
  char prelogin_cookie[256];
  char current_cookie[256];
  char csrf1[CHTTP_WEB_CSRF_TOKEN_BYTES + 1u];
  char csrf2[CHTTP_WEB_CSRF_TOKEN_BYTES + 1u];
  char body[768];
  int status;
  int result = SALTS_EPROTO;

#define AUTH_APP_CHECK(expr)                                      \
  do {                                                            \
    if (!(expr)) {                                                \
      fprintf(stderr, "authenticated app self-test failed: %s\n", #expr); \
      goto cleanup;                                               \
    }                                                             \
  } while (0)

  AUTH_APP_CHECK(app != NULL && app->port != 0u);
  AUTH_APP_CHECK(snprintf(
                     uri, sizeof(uri), "tcp://127.0.0.1:%u",
                     (unsigned int)app->port) > 0);
  AUTH_APP_CHECK(chttp_client_init(&client, &client_config) == SALTS_OK);

  status = auth_app_call(
      &client, uri, CHTTP_METHOD_GET, "/login",
      NULL, false, CHTTP_HTTP_1_1, NULL, &response);
  AUTH_APP_CHECK(status == SALTS_OK && response.status_code == 200u);
  AUTH_APP_CHECK(auth_app_body_contains(&response, "<!doctype html>"));
  AUTH_APP_CHECK(auth_app_extract_csrf(&response, csrf1) == SALTS_OK);
  AUTH_APP_CHECK(
      auth_app_copy_cookie(
          &response, prelogin_cookie, sizeof(prelogin_cookie)) == SALTS_OK);
  chttp_response_destroy(&response);
  response = (chttp_response){0};

  status = auth_app_call(
      &client, uri, CHTTP_METHOD_GET, "/dashboard",
      prelogin_cookie, false, CHTTP_HTTP_1_1, NULL, &response);
  AUTH_APP_CHECK(status == SALTS_OK && response.status_code == 303u);
  AUTH_APP_CHECK(auth_app_header_equals(
      &response, "Location", "/login?return_to=%2Fdashboard"));
  AUTH_APP_CHECK(app->dashboard_calls == 0u);
  chttp_response_destroy(&response);
  response = (chttp_response){0};

  status = auth_app_call(
      &client, uri, CHTTP_METHOD_GET, "/dashboard",
      prelogin_cookie, true, CHTTP_HTTP_2, NULL, &response);
  AUTH_APP_CHECK(status == SALTS_OK && response.status_code == 200u);
  AUTH_APP_CHECK(auth_app_header_equals(
      &response, "HX-Redirect", "/login?return_to=%2Fdashboard"));
  AUTH_APP_CHECK(app->dashboard_calls == 0u);
  chttp_response_destroy(&response);
  response = (chttp_response){0};

  AUTH_APP_CHECK(snprintf(
                     body, sizeof(body),
                     "_csrf=%s&username=alice&password=secret&return_to=//evil.example",
                     csrf1) > 0);
  status = auth_app_call(
      &client, uri, CHTTP_METHOD_POST, "/login",
      prelogin_cookie, false, CHTTP_HTTP_1_1, body, &response);
  AUTH_APP_CHECK(status == SALTS_OK && response.status_code == 400u);
  chttp_response_destroy(&response);
  response = (chttp_response){0};

  AUTH_APP_CHECK(snprintf(
                     body, sizeof(body),
                     "_csrf=%s&username=alice&password=secret&return_to=/dashboard",
                     csrf1) > 0);
  status = auth_app_call(
      &client, uri, CHTTP_METHOD_POST, "/login",
      prelogin_cookie, false, CHTTP_HTTP_1_1, body, &response);
  AUTH_APP_CHECK(status == SALTS_OK && response.status_code == 303u);
  AUTH_APP_CHECK(auth_app_header_equals(&response, "Location", "/dashboard"));
  AUTH_APP_CHECK(
      auth_app_copy_cookie(
          &response, current_cookie, sizeof(current_cookie)) == SALTS_OK);
  AUTH_APP_CHECK(strcmp(prelogin_cookie, current_cookie) != 0);
  chttp_response_destroy(&response);
  response = (chttp_response){0};

  status = auth_app_call(
      &client, uri, CHTTP_METHOD_GET, "/dashboard",
      prelogin_cookie, false, CHTTP_HTTP_1_1, NULL, &response);
  AUTH_APP_CHECK(status == SALTS_OK && response.status_code == 303u);
  AUTH_APP_CHECK(app->dashboard_calls == 0u);
  chttp_response_destroy(&response);
  response = (chttp_response){0};

  status = auth_app_call(
      &client, uri, CHTTP_METHOD_GET, "/dashboard",
      current_cookie, false, CHTTP_HTTP_1_1, NULL, &response);
  AUTH_APP_CHECK(status == SALTS_OK && response.status_code == 200u);
  AUTH_APP_CHECK(app->dashboard_calls == 1u);
  AUTH_APP_CHECK(auth_app_body_contains(
      &response, "&lt;Admin &amp; Alice&gt;"));
  AUTH_APP_CHECK(auth_app_body_contains(&response, "/assets/app.css"));
  AUTH_APP_CHECK(auth_app_extract_csrf(&response, csrf2) == SALTS_OK);
  AUTH_APP_CHECK(strcmp(csrf1, csrf2) != 0);
  chttp_response_destroy(&response);
  response = (chttp_response){0};

  status = auth_app_call(
      &client, uri, CHTTP_METHOD_GET, "/admin-only",
      current_cookie, false, CHTTP_HTTP_1_1, NULL, &response);
  AUTH_APP_CHECK(status == SALTS_OK && response.status_code == 403u);
  AUTH_APP_CHECK(app->forbidden_calls == 1u);
  AUTH_APP_CHECK(app->admin_terminal_calls == 0u);
  AUTH_APP_CHECK(auth_app_body_contains(
      &response, "&lt;Admin &amp; Alice&gt;"));
  chttp_response_destroy(&response);
  response = (chttp_response){0};

  status = auth_app_call(
      &client, uri, CHTTP_METHOD_GET, "/assets/app.css",
      NULL, false, CHTTP_HTTP_2, NULL, &response);
  AUTH_APP_CHECK(status == SALTS_OK && response.status_code == 200u);
  AUTH_APP_CHECK(auth_app_body_contains(&response, "font-family"));
  AUTH_APP_CHECK(auth_app_header_equals(
      &response, "Cache-Control", "public, max-age=60"));
  chttp_response_destroy(&response);
  response = (chttp_response){0};

  status = auth_app_call(
      &client, uri, CHTTP_METHOD_GET, "/dashboard",
      current_cookie, true, CHTTP_HTTP_2, NULL, &response);
  AUTH_APP_CHECK(status == SALTS_OK && response.status_code == 200u);
  AUTH_APP_CHECK(app->dashboard_calls == 2u);
  AUTH_APP_CHECK(!auth_app_body_contains(&response, "<!doctype html>"));
  AUTH_APP_CHECK(auth_app_body_contains(&response, "<main id=\"app\">"));
  chttp_response_destroy(&response);
  response = (chttp_response){0};

  AUTH_APP_CHECK(snprintf(
                     body, sizeof(body), "_csrf=%s", csrf2) > 0);
  status = auth_app_call(
      &client, uri, CHTTP_METHOD_POST, "/logout",
      current_cookie, false, CHTTP_HTTP_1_1, body, &response);
  AUTH_APP_CHECK(status == SALTS_OK && response.status_code == 303u);
  AUTH_APP_CHECK(auth_app_header_equals(&response, "Location", "/login"));
  AUTH_APP_CHECK(
      chttp_response_header(&response, "Set-Cookie") != NULL &&
      strstr(chttp_response_header(&response, "Set-Cookie"), "Max-Age=0") != NULL);
  chttp_response_destroy(&response);
  response = (chttp_response){0};

  status = auth_app_call(
      &client, uri, CHTTP_METHOD_GET, "/dashboard",
      current_cookie, false, CHTTP_HTTP_1_1, NULL, &response);
  AUTH_APP_CHECK(status == SALTS_OK && response.status_code == 303u);
  AUTH_APP_CHECK(app->dashboard_calls == 2u);
  chttp_response_destroy(&response);
  response = (chttp_response){0};

  result = SALTS_OK;

cleanup:
  chttp_response_destroy(&response);
  if (client.impl != NULL)
    (void)chttp_client_destroy(&client, AUTH_APP_TIMEOUT_MS);
#undef AUTH_APP_CHECK
  return result;
}

int main(int argc, char **argv) {
  auth_app app;
  const char *asset_root = CHTTP_WEB_AUTH_APP_ASSET_ROOT;
  bool self_test = false;
  int status;
  int index;

  for (index = 1; index < argc; ++index) {
    if (strcmp(argv[index], "--self-test") == 0) {
      self_test = true;
    } else if (strcmp(argv[index], "--asset-root") == 0 &&
               index + 1 < argc) {
      asset_root = argv[++index];
    } else {
      fprintf(stderr, "usage: %s [--self-test] [--asset-root DIR]\n", argv[0]);
      return EXIT_FAILURE;
    }
  }

  status = auth_app_start(&app, asset_root);
  if (status != SALTS_OK) {
    fprintf(stderr, "authenticated app start failed: %d\n", status);
    return EXIT_FAILURE;
  }

  if (self_test) {
    status = auth_app_self_test(&app);
    if (status == SALTS_OK)
      puts("authenticated application self-test passed");
  } else {
    printf(
        "CHttp authenticated app: http://127.0.0.1:%u/\n"
        "demo credentials: alice / secret\n"
        "press Enter to stop\n",
        (unsigned int)app.port);
    (void)getchar();
  }

  {
    const int stop_status = auth_app_stop(&app);
    if (status == SALTS_OK) status = stop_status;
  }
  return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
