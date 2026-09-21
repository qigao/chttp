#include <chttp_web/web.h>
#include <http_client/http.h>

#include "tinytest.h"

#include <salts/error_codes.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  UPLOAD_HTTP_TIMEOUT_MS = 5000,
  UPLOAD_HTTP_BODY_BYTES = 4096,
  UPLOAD_HTTP_STAGE_BYTES = 512
};

typedef struct upload_http_app {
  chttp_web_upload_request upload;
  unsigned char staged[UPLOAD_HTTP_STAGE_BYTES];
  size_t staged_size;
  unsigned char committed[UPLOAD_HTTP_STAGE_BYTES];
  size_t committed_size;
  size_t begin_calls;
  size_t commit_calls;
  size_t abort_calls;
  chttp_web_status abort_status;
  int abort_native_status;
} upload_http_app;

static native_io_backend_kind upload_http_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config upload_http_network(size_t connections) {
  return (cnet_client_config){
      .backend = upload_http_backend(),
      .connection_capacity = connections,
      .command_capacity = 16u,
      .request_capacity = 16u,
      .completion_batch_capacity = 8u,
      .event_capacity = 32u,
      .max_send_bytes = 64u * 1024u,
      .receive_buffer_bytes = 4096u,
      .connect_timeout_ms = UPLOAD_HTTP_TIMEOUT_MS,
      .read_timeout_ms = UPLOAD_HTTP_TIMEOUT_MS,
      .write_timeout_ms = UPLOAD_HTTP_TIMEOUT_MS};
}

static chttp_server_config upload_http_server_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = upload_http_network(4u),
      .route_capacity = 4u,
      .middleware_capacity = 1u,
      .max_route_middleware_count = 1u,
      .max_route_param_count = 1u,
      .max_route_param_bytes = 128u,
      .max_target_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 4096u,
      .max_request_body_bytes = UPLOAD_HTTP_BODY_BYTES,
      .max_response_header_count = 16u,
      .max_response_header_bytes = 4096u,
      .max_response_body_bytes = 4096u,
      .session_capacity = 8u,
      .session_entry_capacity = 4u,
      .max_session_key_bytes = 64u,
      .max_session_value_bytes = 1024u,
      .session_idle_timeout_ms = 60000u,
      .session_cookie_name = "chttp_web_upload",
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

static chttp_client_config upload_http_client_config(void) {
  return (chttp_client_config){
      .network = upload_http_network(4u),
      .request_capacity = 4u,
      .max_start_line_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 4096u,
      .max_request_body_bytes = UPLOAD_HTTP_BODY_BYTES,
      .max_response_body_bytes = 4096u,
      .max_informational_responses = 2u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
}

static int upload_http_stage_begin(
    void *user, const chttp_server_request_view *request) {
  upload_http_app *app = (upload_http_app *)user;
  if (app == NULL || request == NULL ||
      request->session != NULL ||
      request->body_sink_user != NULL ||
      strcmp(request->path, "/upload") != 0)
    return SALTS_EPROTO;
  ++app->begin_calls;
  app->staged_size = 0u;
  return SALTS_OK;
}

static int upload_http_part_begin(
    void *user, const chttp_web_multipart_part *part) {
  upload_http_app *app = (upload_http_app *)user;
  (void)app;
  if (part == NULL || part->name.data == NULL)
    return SALTS_EINVAL;
  return SALTS_OK;
}

static int upload_http_part_data(
    void *user, const void *data, size_t size) {
  upload_http_app *app = (upload_http_app *)user;
  if (app == NULL || (size != 0u && data == NULL) ||
      size > sizeof(app->staged) - app->staged_size)
    return SALTS_ENOBUFS;
  if (size != 0u)
    memcpy(app->staged + app->staged_size, data, size);
  app->staged_size += size;
  return SALTS_OK;
}

static int upload_http_part_end(void *user) {
  return user != NULL ? SALTS_OK : SALTS_EINVAL;
}

static int upload_http_commit(void *user) {
  upload_http_app *app = (upload_http_app *)user;
  if (app == NULL || app->staged_size > sizeof(app->committed))
    return SALTS_EINVAL;
  memcpy(app->committed, app->staged, app->staged_size);
  app->committed_size = app->staged_size;
  ++app->commit_calls;
  return SALTS_OK;
}

static void upload_http_abort(
    void *user, chttp_web_status status, int native_status) {
  upload_http_app *app = (upload_http_app *)user;
  if (app == NULL) return;
  ++app->abort_calls;
  app->abort_status = status;
  app->abort_native_status = native_status;
  app->staged_size = 0u;
}

static chttp_web_upload_callbacks upload_http_callbacks(void) {
  return (chttp_web_upload_callbacks){
      .size = sizeof(chttp_web_upload_callbacks),
      .begin = upload_http_stage_begin,
      .part_begin = upload_http_part_begin,
      .part_data = upload_http_part_data,
      .part_end = upload_http_part_end,
      .commit = upload_http_commit,
      .abort = upload_http_abort};
}

static chttp_web_multipart_limits upload_http_limits(void) {
  chttp_web_multipart_limits limits =
      (chttp_web_multipart_limits)CHTTP_WEB_MULTIPART_LIMITS_INIT;
  limits.max_parts = 8u;
  limits.max_header_count = 8u;
  limits.max_header_bytes = 1024u;
  limits.max_name_bytes = 64u;
  limits.max_filename_bytes = 128u;
  limits.max_content_type_bytes = 64u;
  limits.max_field_bytes = 128u;
  limits.max_total_bytes = UPLOAD_HTTP_BODY_BYTES;
  return limits;
}

static int upload_http_csrf(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  const char *token = NULL;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  (void)user;
  if (request == NULL || request->session == NULL)
    return SALTS_EPROTO;
  status = chttp_web_csrf_ensure(
      request->session, &token, &error);
  if (status != CHTTP_WEB_OK || token == NULL)
    return SALTS_EIO;
  return chttp_server_reply(
      response, 200u, "text/plain",
      token, CHTTP_WEB_CSRF_TOKEN_BYTES);
}

static int upload_http_open(
    void *user,
    const chttp_server_request_view *request,
    chttp_body_sink *out_sink) {
  upload_http_app *app = (upload_http_app *)user;
  chttp_web_multipart_limits limits = upload_http_limits();
  chttp_web_upload_callbacks callbacks = upload_http_callbacks();
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  if (app == NULL) return SALTS_EINVAL;
  status = chttp_web_upload_open(
      &app->upload, request, &limits, &callbacks,
      app, out_sink, &error);
  if (status == CHTTP_WEB_OK) return SALTS_OK;
  return error.native_status != 0
      ? error.native_status
      : SALTS_EPROTO;
}

static void upload_http_close(
    void *user, chttp_body_sink *sink, int status) {
  upload_http_app *app = (upload_http_app *)user;
  if (app == NULL || sink == NULL ||
      sink->user != &app->upload)
    return;
  chttp_web_upload_close(&app->upload, status);
}

static int upload_http_handler(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  upload_http_app *app = (upload_http_app *)user;
  chttp_web_validation validation = CHTTP_WEB_VALIDATION_INIT;
  chttp_web_validation_error validation_errors[1];
  char validation_bytes[64];
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  const char *reject;
  chttp_web_status status;
  unsigned int http_status;
  int reply_status;

  if (app == NULL || request == NULL ||
      request->body_sink_user != &app->upload)
    return SALTS_EPROTO;

  status = chttp_web_validation_init(
      &validation,
      validation_errors,
      1u,
      validation_bytes,
      sizeof(validation_bytes),
      &error);
  if (status != CHTTP_WEB_OK) return SALTS_EIO;

  reject = chttp_server_request_header(request, "X-Reject");
  if (reject != NULL && strcmp(reject, "1") == 0) {
    status = chttp_web_validation_add_global(
        &validation, "application rejected upload", &error);
    if (status != CHTTP_WEB_OK) return SALTS_EIO;
  }

  status = chttp_web_upload_finalize(
      &app->upload, request, &validation, &error);
  if (status == CHTTP_WEB_OK)
    http_status = 200u;
  else if (status == CHTTP_WEB_CSRF)
    http_status = 403u;
  else if (status == CHTTP_WEB_VALIDATION)
    http_status = 422u;
  else
    http_status = 500u;

  reply_status = chttp_server_reply(
      response, http_status, "text/plain",
      status == CHTTP_WEB_OK ? "ok" : "rejected",
      status == CHTTP_WEB_OK ? 2u : 8u);

  if (chttp_web_upload_reset(&app->upload, &error) != CHTTP_WEB_OK)
    return SALTS_EIO;
  return reply_status;
}

static int upload_http_copy_cookie(
    const chttp_response *response,
    char *cookie,
    size_t capacity) {
  const char *set_cookie = chttp_response_header(response, "Set-Cookie");
  const char *end;
  size_t size;
  if (set_cookie == NULL || cookie == NULL || capacity == 0u)
    return SALTS_EINVAL;
  end = strchr(set_cookie, ';');
  size = end != NULL
      ? (size_t)(end - set_cookie)
      : strlen(set_cookie);
  if (size == 0u || size >= capacity)
    return SALTS_EMSGSIZE;
  memcpy(cookie, set_cookie, size);
  cookie[size] = '\0';
  return SALTS_OK;
}

static int upload_http_get_csrf(
    chttp_client *client,
    const char *uri,
    chttp_protocol protocol,
    char token[CHTTP_WEB_CSRF_TOKEN_BYTES + 1u],
    char *cookie,
    size_t cookie_capacity) {
  chttp_options options = {
      .connection_uri = uri,
      .authority = "127.0.0.1",
      .target = "/csrf",
      .timeout_ms = UPLOAD_HTTP_TIMEOUT_MS,
      .protocol = protocol};
  chttp_response response = {0};
  chttp_error error = {0};
  int status = chttp_get(client, &options, &response, &error);
  if (status != SALTS_OK) return status;
  if (response.status_code != 200u ||
      response.body == NULL ||
      response.body_size != CHTTP_WEB_CSRF_TOKEN_BYTES) {
    chttp_response_destroy(&response);
    return SALTS_EPROTO;
  }
  memcpy(token, response.body, CHTTP_WEB_CSRF_TOKEN_BYTES);
  token[CHTTP_WEB_CSRF_TOKEN_BYTES] = '\0';
  status = upload_http_copy_cookie(
      &response, cookie, cookie_capacity);
  chttp_response_destroy(&response);
  return status;
}

static int upload_http_post(
    chttp_client *client,
    const char *uri,
    chttp_protocol protocol,
    const char *cookie,
    const char *token,
    int reject,
    unsigned int *out_status) {
  static const char prefix[] =
      "--AaB03x\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"a.txt\"\r\n"
      "Content-Type: text/plain\r\n\r\n"
      "payload\r\n"
      "--AaB03x\r\n"
      "Content-Disposition: form-data; name=\"_csrf\"\r\n\r\n";
  static const char suffix[] = "\r\n--AaB03x--\r\n";
  char body[1024];
  chttp_header headers[3];
  size_t header_count = 0u;
  chttp_options options;
  chttp_response response = {0};
  chttp_error error = {0};
  int body_size;
  int status;

  body_size = snprintf(
      body, sizeof(body), "%s%s%s", prefix, token, suffix);
  if (body_size <= 0 || (size_t)body_size >= sizeof(body))
    return SALTS_EMSGSIZE;

  headers[header_count++] = (chttp_header){
      "Content-Type", "multipart/form-data; boundary=AaB03x"};
  headers[header_count++] = (chttp_header){
      "Cookie", cookie};
  if (reject)
    headers[header_count++] = (chttp_header){
        "X-Reject", "1"};

  options = (chttp_options){
      .connection_uri = uri,
      .authority = "127.0.0.1",
      .target = "/upload",
      .headers = headers,
      .header_count = header_count,
      .body = body,
      .body_size = (size_t)body_size,
      .timeout_ms = UPLOAD_HTTP_TIMEOUT_MS,
      .protocol = protocol};

  status = chttp_post(client, &options, &response, &error);
  if (status == SALTS_OK && out_status != NULL)
    *out_status = response.status_code;
  chttp_response_destroy(&response);
  return status;
}

spec("CHttp::Web upload route integration") {
  it("commits only after complete H1/H2 body plus session CSRF and validation") {
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = upload_http_server_config();
    chttp_client_config client_config = upload_http_client_config();
    upload_http_app app = {0};
    chttp_server_route_options upload_route = {
        .method = CHTTP_METHOD_POST,
        .path = "/upload",
        .handler = upload_http_handler,
        .user = &app,
        .body_open = upload_http_open,
        .body_close = upload_http_close};
    char uri[64];
    char h1_token[CHTTP_WEB_CSRF_TOKEN_BYTES + 1u];
    char h2_token[CHTTP_WEB_CSRF_TOKEN_BYTES + 1u];
    char h1_cookie[256];
    char h2_cookie[256];
    char bad_token[CHTTP_WEB_CSRF_TOKEN_BYTES + 1u];
    unsigned int status_code = 0u;
    uint16_t port = 0u;

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/csrf", upload_http_csrf, &app),
        SALTS_OK);
    check_equal(
        chttp_server_route_with(&server, &upload_route),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(
        snprintf(
            uri, sizeof(uri), "tcp://127.0.0.1:%u",
            (unsigned int)port) > 0);

    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    check_equal(
        upload_http_get_csrf(
            &client, uri, CHTTP_HTTP_1_1,
            h1_token, h1_cookie, sizeof(h1_cookie)),
        SALTS_OK);
    check_equal(
        upload_http_post(
            &client, uri, CHTTP_HTTP_1_1,
            h1_cookie, h1_token, 0, &status_code),
        SALTS_OK);
    check_equal(status_code, 200u);
    check_equal(app.commit_calls, (size_t)1u);
    check_equal(app.abort_calls, (size_t)0u);
    check_equal(app.committed_size, (size_t)7u);
    check_equal(app.committed, "payload", 7u);

    check_equal(
        upload_http_get_csrf(
            &client, uri, CHTTP_HTTP_2,
            h2_token, h2_cookie, sizeof(h2_cookie)),
        SALTS_OK);
    check_equal(
        upload_http_post(
            &client, uri, CHTTP_HTTP_2,
            h2_cookie, h2_token, 0, &status_code),
        SALTS_OK);
    check_equal(status_code, 200u);
    check_equal(app.commit_calls, (size_t)2u);
    check_equal(app.abort_calls, (size_t)0u);

    memset(bad_token, '0', CHTTP_WEB_CSRF_TOKEN_BYTES);
    bad_token[CHTTP_WEB_CSRF_TOKEN_BYTES] = '\0';
    if (strcmp(bad_token, h1_token) == 0)
      bad_token[0] = '1';

    check_equal(
        upload_http_post(
            &client, uri, CHTTP_HTTP_1_1,
            h1_cookie, bad_token, 0, &status_code),
        SALTS_OK);
    check_equal(status_code, 403u);
    check_equal(app.commit_calls, (size_t)2u);
    check_equal(app.abort_calls, (size_t)1u);
    check_equal(app.abort_status, CHTTP_WEB_CSRF);

    check_equal(
        upload_http_post(
            &client, uri, CHTTP_HTTP_2,
            h2_cookie, h2_token, 1, &status_code),
        SALTS_OK);
    check_equal(status_code, 422u);
    check_equal(app.commit_calls, (size_t)2u);
    check_equal(app.abort_calls, (size_t)2u);
    check_equal(app.abort_status, CHTTP_WEB_VALIDATION);

    check_equal(
        chttp_client_destroy(&client, UPLOAD_HTTP_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_server_stop(&server, UPLOAD_HTTP_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }
}
