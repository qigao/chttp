#include <chttp_web/web.h>
#include <http_client/http.h>

#include "tinytest.h"

#include <salts/error_codes.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  AUTH_HTTP_TIMEOUT_MS = 5000,
  AUTH_HTTP_BODY_BYTES = 4096
};

static native_io_backend_kind auth_http_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config auth_http_network(size_t connections) {
  return (cnet_client_config){
      .backend = auth_http_backend(),
      .connection_capacity = connections,
      .command_capacity = 16u,
      .request_capacity = 16u,
      .completion_batch_capacity = 8u,
      .event_capacity = 32u,
      .max_send_bytes = 64u * 1024u,
      .receive_buffer_bytes = 4096u,
      .connect_timeout_ms = AUTH_HTTP_TIMEOUT_MS,
      .read_timeout_ms = AUTH_HTTP_TIMEOUT_MS,
      .write_timeout_ms = AUTH_HTTP_TIMEOUT_MS};
}

static chttp_server_config auth_http_server_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = auth_http_network(4u),
      .route_capacity = 8u,
      .middleware_capacity = 2u,
      .max_route_middleware_count = 2u,
      .max_route_param_count = 1u,
      .max_route_param_bytes = 128u,
      .max_target_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 4096u,
      .max_request_body_bytes = AUTH_HTTP_BODY_BYTES,
      .max_response_header_count = 16u,
      .max_response_header_bytes = 4096u,
      .max_response_body_bytes = AUTH_HTTP_BODY_BYTES,
      .session_capacity = 8u,
      .session_entry_capacity = 8u,
      .max_session_key_bytes = 64u,
      .max_session_value_bytes = 1024u,
      .session_idle_timeout_ms = 60000u,
      .session_cookie_name = "chttp_web_auth",
      .poll_slice_ms = 1u,
      .enable_http2 = 1,
      .h2_stream_capacity = 8u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_output_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u,
      .max_buffered_response_body_bytes = 4096u,
      .buffer_capacity_bytes = 2u * 1024u * 1024u};
}

static chttp_client_config auth_http_client_config(void) {
  return (chttp_client_config){
      .network = auth_http_network(4u),
      .request_capacity = 4u,
      .max_start_line_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 4096u,
      .max_request_body_bytes = AUTH_HTTP_BODY_BYTES,
      .max_response_body_bytes = AUTH_HTTP_BODY_BYTES,
      .max_informational_responses = 2u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
}

static chttp_web_status auth_http_parse_form(
    const chttp_server_request_view *request,
    chttp_web_form_pair pairs[8],
    char bytes[2048],
    chttp_web_form *form,
    chttp_web_error *error) {
  chttp_web_form_parse_options options =
      (chttp_web_form_parse_options)CHTTP_WEB_FORM_PARSE_OPTIONS_INIT;
  options.max_input_bytes = 2048u;
  options.max_pairs = 8u;
  options.max_decoded_bytes = 2048u;
  options.pair_storage = pairs;
  options.pair_capacity = 8u;
  options.byte_storage = bytes;
  options.byte_capacity = 2048u;
  return chttp_web_form_parse(
      request->body, request->body_size, &options, form, error);
}

static int auth_http_csrf(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  const char *token = NULL;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  (void)user;
  if (request == NULL || request->session == NULL) return SALTS_EPROTO;
  if (chttp_web_csrf_ensure(
          request->session, &token, &error) != CHTTP_WEB_OK ||
      token == NULL)
    return SALTS_EIO;
  return chttp_server_reply(
      response, 200u, "text/plain",
      token, CHTTP_WEB_CSRF_TOKEN_BYTES);
}

static int auth_http_login(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_form_pair pairs[8];
  char bytes[2048];
  chttp_web_form form = {0};
  const chttp_web_form_pair *username;
  const chttp_web_form_pair *password;
  chttp_web_principal_input principal =
      (chttp_web_principal_input)CHTTP_WEB_PRINCIPAL_INPUT_INIT;
  const char *rotated = NULL;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  (void)user;

  if (request == NULL || request->method != CHTTP_METHOD_POST)
    return SALTS_EPROTO;
  status = auth_http_parse_form(
      request, pairs, bytes, &form, &error);
  if (status != CHTTP_WEB_OK)
    return chttp_server_reply(response, 400u, NULL, NULL, 0u);

  username = chttp_web_form_get(&form, "username", 0u);
  password = chttp_web_form_get(&form, "password", 0u);
  if (username == NULL || password == NULL ||
      username->value.size != 5u ||
      memcmp(username->value.data, "alice", 5u) != 0 ||
      password->value.size != 6u ||
      memcmp(password->value.data, "secret", 6u) != 0)
    return chttp_server_reply(response, 401u, NULL, NULL, 0u);

  principal.subject = "alice";
  principal.role = "admin";
  principal.display_name = "<Admin & Alice>";

  status = chttp_web_principal_sign_in(
      request, &form, &principal, &rotated, &error);
  if (status == CHTTP_WEB_CSRF)
    return chttp_server_reply(response, 403u, NULL, NULL, 0u);
  if (status != CHTTP_WEB_OK)
    return chttp_server_reply(response, 500u, NULL, NULL, 0u);
  if (rotated == NULL) return SALTS_EPROTO;

  return chttp_server_reply(
      response, 200u, "text/plain",
      rotated, CHTTP_WEB_CSRF_TOKEN_BYTES);
}

static int auth_http_whoami(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_principal principal = CHTTP_WEB_PRINCIPAL_INIT;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  char body[128];
  int written;
  (void)user;

  if (chttp_web_principal_get(
          request, &principal, &error) != CHTTP_WEB_OK)
    return SALTS_EIO;
  if (!principal.authenticated)
    return chttp_server_reply(
        response, 200u, "text/plain", "anonymous", 9u);

  written = snprintf(
      body, sizeof(body), "%.*s:%.*s",
      (int)principal.subject.size, principal.subject.data,
      (int)principal.role.size,
      principal.role.data != NULL ? principal.role.data : "");
  if (written < 0 || (size_t)written >= sizeof(body))
    return SALTS_EMSGSIZE;
  return chttp_server_reply(
      response, 200u, "text/plain", body, (size_t)written);
}

static int auth_http_logout(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_form_pair pairs[8];
  char bytes[2048];
  chttp_web_form form = {0};
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  (void)user;

  status = auth_http_parse_form(
      request, pairs, bytes, &form, &error);
  if (status != CHTTP_WEB_OK)
    return chttp_server_reply(response, 400u, NULL, NULL, 0u);

  status = chttp_web_principal_sign_out(
      request, &form, &error);
  if (status == CHTTP_WEB_CSRF)
    return chttp_server_reply(response, 403u, NULL, NULL, 0u);
  return status == CHTTP_WEB_OK
      ? chttp_server_reply(response, 204u, NULL, NULL, 0u)
      : SALTS_EIO;
}

static int auth_http_copy_cookie(
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

static int auth_http_get(
    chttp_client *client,
    const char *uri,
    const char *target,
    const char *cookie,
    int htmx,
    chttp_protocol protocol,
    chttp_response *response) {
  chttp_header headers[2];
  size_t count = 0u;
  chttp_options options;
  chttp_error error = {0};
  if (cookie != NULL)
    headers[count++] = (chttp_header){"Cookie", cookie};
  if (htmx)
    headers[count++] = (chttp_header){"HX-Request", "true"};
  options = (chttp_options){
      .connection_uri = uri,
      .authority = "127.0.0.1",
      .target = target,
      .headers = count != 0u ? headers : NULL,
      .header_count = count,
      .timeout_ms = AUTH_HTTP_TIMEOUT_MS,
      .protocol = protocol};
  return chttp_get(client, &options, response, &error);
}

static int auth_http_post(
    chttp_client *client,
    const char *uri,
    const char *target,
    const char *cookie,
    const char *body,
    chttp_protocol protocol,
    chttp_response *response) {
  chttp_header headers[2];
  size_t count = 0u;
  chttp_options options;
  chttp_error error = {0};
  headers[count++] = (chttp_header){
      "Content-Type", "application/x-www-form-urlencoded"};
  if (cookie != NULL)
    headers[count++] = (chttp_header){"Cookie", cookie};
  options = (chttp_options){
      .connection_uri = uri,
      .authority = "127.0.0.1",
      .target = target,
      .headers = headers,
      .header_count = count,
      .body = body,
      .body_size = strlen(body),
      .timeout_ms = AUTH_HTTP_TIMEOUT_MS,
      .protocol = protocol};
  return chttp_post(client, &options, response, &error);
}

spec("CHttp::Web browser principal lifecycle") {
  it("rotates Session and CSRF on login then invalidates authenticated state on logout") {
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = auth_http_server_config();
    chttp_client_config client_config = auth_http_client_config();
    chttp_response csrf = {0};
    chttp_response login = {0};
    chttp_response stale = {0};
    chttp_response current = {0};
    chttp_response htmx = {0};
    chttp_response bad_logout = {0};
    chttp_response still_current = {0};
    chttp_response logout = {0};
    chttp_response stale_after_logout = {0};
    char uri[64];
    char cookie1[256];
    char cookie2[256];
    char token1[CHTTP_WEB_CSRF_TOKEN_BYTES + 1u];
    char token2[CHTTP_WEB_CSRF_TOKEN_BYTES + 1u];
    char login_body[512];
    char logout_body[256];
    uint16_t port = 0u;

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_get(&server, "/csrf", auth_http_csrf, NULL), SALTS_OK);
    check_equal(chttp_server_post(&server, "/login", auth_http_login, NULL), SALTS_OK);
    check_equal(chttp_server_get(&server, "/whoami", auth_http_whoami, NULL), SALTS_OK);
    check_equal(chttp_server_post(&server, "/logout", auth_http_logout, NULL), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(
        uri, sizeof(uri), "tcp://127.0.0.1:%u",
        (unsigned int)port) > 0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    check_equal(auth_http_get(
        &client, uri, "/csrf", NULL, 0,
        CHTTP_HTTP_1_1, &csrf), SALTS_OK);
    check_equal(csrf.status_code, 200u);
    check_equal(csrf.body_size, (size_t)CHTTP_WEB_CSRF_TOKEN_BYTES);
    memcpy(token1, csrf.body, CHTTP_WEB_CSRF_TOKEN_BYTES);
    token1[CHTTP_WEB_CSRF_TOKEN_BYTES] = '\0';
    check_equal(auth_http_copy_cookie(
        &csrf, cookie1, sizeof(cookie1)), SALTS_OK);

    check_greater(snprintf(
        login_body, sizeof(login_body),
        "_csrf=%s&username=alice&password=secret", token1), 0);
    check_equal(auth_http_post(
        &client, uri, "/login", cookie1, login_body,
        CHTTP_HTTP_1_1, &login), SALTS_OK);
    check_equal(login.status_code, 200u);
    check_equal(login.body_size, (size_t)CHTTP_WEB_CSRF_TOKEN_BYTES);
    memcpy(token2, login.body, CHTTP_WEB_CSRF_TOKEN_BYTES);
    token2[CHTTP_WEB_CSRF_TOKEN_BYTES] = '\0';
    check_true(strcmp(token1, token2) != 0);
    check_equal(auth_http_copy_cookie(
        &login, cookie2, sizeof(cookie2)), SALTS_OK);
    check_true(strcmp(cookie1, cookie2) != 0);

    check_equal(auth_http_get(
        &client, uri, "/whoami", cookie1, 0,
        CHTTP_HTTP_1_1, &stale), SALTS_OK);
    check_equal(stale.status_code, 200u);
    check_equal(stale.body, "anonymous", 9u);

    check_equal(auth_http_get(
        &client, uri, "/whoami", cookie2, 0,
        CHTTP_HTTP_1_1, &current), SALTS_OK);
    check_equal(current.body, "alice:admin", 11u);

    check_equal(auth_http_get(
        &client, uri, "/whoami", cookie2, 1,
        CHTTP_HTTP_2, &htmx), SALTS_OK);
    check_equal(htmx.body, "alice:admin", 11u);

    check_greater(snprintf(
        logout_body, sizeof(logout_body), "_csrf=%s", token1), 0);
    check_equal(auth_http_post(
        &client, uri, "/logout", cookie2, logout_body,
        CHTTP_HTTP_1_1, &bad_logout), SALTS_OK);
    check_equal(bad_logout.status_code, 403u);

    check_equal(auth_http_get(
        &client, uri, "/whoami", cookie2, 0,
        CHTTP_HTTP_1_1, &still_current), SALTS_OK);
    check_equal(still_current.body, "alice:admin", 11u);

    check_greater(snprintf(
        logout_body, sizeof(logout_body), "_csrf=%s", token2), 0);
    check_equal(auth_http_post(
        &client, uri, "/logout", cookie2, logout_body,
        CHTTP_HTTP_1_1, &logout), SALTS_OK);
    check_equal(logout.status_code, 204u);
    check_not_null(chttp_response_header(&logout, "Set-Cookie"));
    check_not_null(strstr(
        chttp_response_header(&logout, "Set-Cookie"), "Max-Age=0"));

    check_equal(auth_http_get(
        &client, uri, "/whoami", cookie2, 0,
        CHTTP_HTTP_1_1, &stale_after_logout), SALTS_OK);
    check_equal(stale_after_logout.body, "anonymous", 9u);

    chttp_response_destroy(&stale_after_logout);
    chttp_response_destroy(&logout);
    chttp_response_destroy(&still_current);
    chttp_response_destroy(&bad_logout);
    chttp_response_destroy(&htmx);
    chttp_response_destroy(&current);
    chttp_response_destroy(&stale);
    chttp_response_destroy(&login);
    chttp_response_destroy(&csrf);

    check_equal(
        chttp_client_destroy(&client, AUTH_HTTP_TIMEOUT_MS), SALTS_OK);
    check_equal(
        chttp_server_stop(&server, AUTH_HTTP_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("fails closed when principal Session capacity is exhausted") {
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = auth_http_server_config();
    chttp_client_config client_config = auth_http_client_config();
    chttp_response csrf = {0};
    chttp_response failed = {0};
    chttp_response stale = {0};
    char uri[64];
    char cookie[256];
    char token[CHTTP_WEB_CSRF_TOKEN_BYTES + 1u];
    char login_body[512];
    uint16_t port = 0u;
    const char *expired;

    server_config.session_entry_capacity = 2u;
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_get(&server, "/csrf", auth_http_csrf, NULL), SALTS_OK);
    check_equal(chttp_server_post(&server, "/login", auth_http_login, NULL), SALTS_OK);
    check_equal(chttp_server_get(&server, "/whoami", auth_http_whoami, NULL), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(
        uri, sizeof(uri), "tcp://127.0.0.1:%u",
        (unsigned int)port) > 0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    check_equal(auth_http_get(
        &client, uri, "/csrf", NULL, 0,
        CHTTP_HTTP_1_1, &csrf), SALTS_OK);
    check_equal(csrf.status_code, 200u);
    memcpy(token, csrf.body, CHTTP_WEB_CSRF_TOKEN_BYTES);
    token[CHTTP_WEB_CSRF_TOKEN_BYTES] = '\0';
    check_equal(auth_http_copy_cookie(
        &csrf, cookie, sizeof(cookie)), SALTS_OK);

    check_greater(snprintf(
        login_body, sizeof(login_body),
        "_csrf=%s&username=alice&password=secret", token), 0);
    check_equal(auth_http_post(
        &client, uri, "/login", cookie, login_body,
        CHTTP_HTTP_1_1, &failed), SALTS_OK);
    check_equal(failed.status_code, 500u);
    expired = chttp_response_header(&failed, "Set-Cookie");
    check_not_null(expired);
    if (expired != NULL) check_not_null(strstr(expired, "Max-Age=0"));

    check_equal(auth_http_get(
        &client, uri, "/whoami", cookie, 0,
        CHTTP_HTTP_1_1, &stale), SALTS_OK);
    check_equal(stale.body, "anonymous", 9u);

    chttp_response_destroy(&stale);
    chttp_response_destroy(&failed);
    chttp_response_destroy(&csrf);
    check_equal(
        chttp_client_destroy(&client, AUTH_HTTP_TIMEOUT_MS), SALTS_OK);
    check_equal(
        chttp_server_stop(&server, AUTH_HTTP_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

}
