#include <chttp_web/web.h>

#include <salts/error_codes.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  WEB_SESSION_TIMEOUT_MS = 5000,
  WEB_SESSION_CONNECTIONS = 8,
  WEB_SESSION_COMMANDS = 32,
  WEB_SESSION_HEADERS = 12,
  WEB_SESSION_HEADER_BYTES = 4096,
  WEB_SESSION_BODY_BYTES = 16 * 1024,
  WEB_SESSION_SEND_BYTES = 64 * 1024,
  WEB_SESSION_COMMAND_BUFFER_BYTES = 256 * 1024,
  WEB_SESSION_BUFFER_CAPACITY_BYTES = 1024 * 1024,
  WEB_SESSION_PATH_BYTES = 1024
};

static int web_session_reply_empty(
    chttp_server_response *response, unsigned int status_code) {
  return chttp_server_reply(response, status_code, NULL, NULL, 0u);
}

static int web_session_reply_csrf_failure(
    chttp_server_response *response, chttp_web_status status,
    const chttp_web_error *error) {
  static const char forbidden[] = "csrf rejected";
  if (status == CHTTP_WEB_CSRF)
    return chttp_server_reply(
        response, 403u, "text/plain; charset=utf-8",
        forbidden, sizeof(forbidden) - 1u);
  fprintf(stderr, "CSRF helper failed: status=%d native=%d message=%s\n",
          (int)status, error ? error->native_status : 0,
          error ? error->message : "");
  return SALTS_EIO;
}

static int web_session_content_type_form(
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

static chttp_web_status web_session_parse_form(
    const chttp_server_request_view *request,
    chttp_web_form_pair pairs[8], char bytes[2048],
    chttp_web_form *form, chttp_web_error *error) {
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

static int web_session_safe(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  (void)user;
  status = chttp_web_csrf_validate(request, NULL, &error);
  if (status != CHTTP_WEB_OK)
    return web_session_reply_csrf_failure(response, status, &error);
  return web_session_reply_empty(response, 204u);
}

static int web_session_token(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_request_context context;
  chttp_web_request_context_options options =
      (chttp_web_request_context_options)
          CHTTP_WEB_REQUEST_CONTEXT_OPTIONS_INIT;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  const char *token = NULL;
  chttp_web_status status;
  (void)user;

  status = chttp_web_csrf_ensure(request->session, &token, &error);
  if (status != CHTTP_WEB_OK || token == NULL) {
    fprintf(stderr, "CSRF ensure failed: status=%d native=%d message=%s\n",
            (int)error.status, error.native_status, error.message);
    return SALTS_EIO;
  }

  status = chttp_web_request_context_init(
      &context, request, &options, &error);
  if (status != CHTTP_WEB_OK || !context.csrf_available ||
      context.csrf_token.size != CHTTP_WEB_CSRF_TOKEN_BYTES ||
      memcmp(context.csrf_token.data, token,
             CHTTP_WEB_CSRF_TOKEN_BYTES) != 0) {
    fprintf(stderr, "CSRF context exposure failed: status=%d message=%s\n",
            (int)error.status, error.message);
    return SALTS_EIO;
  }

  return chttp_server_reply(
      response, 200u, "text/plain; charset=utf-8",
      token, CHTTP_WEB_CSRF_TOKEN_BYTES);
}

static int web_session_mutate(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_form_pair pairs[8];
  char bytes[2048];
  chttp_web_form form = {0};
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_status status;
  (void)user;

  if (!web_session_content_type_form(request))
    return web_session_reply_empty(response, 415u);
  status = web_session_parse_form(
      request, pairs, bytes, &form, &error);
  if (status != CHTTP_WEB_OK)
    return web_session_reply_empty(response, 400u);
  status = chttp_web_csrf_validate(request, &form, &error);
  if (status != CHTTP_WEB_OK)
    return web_session_reply_csrf_failure(response, status, &error);
  return web_session_reply_empty(response, 204u);
}

static int web_session_rotate(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  const char *token = NULL;
  chttp_web_status status;
  (void)user;

  status = chttp_web_csrf_rotate(request->session, &token, &error);
  if (status != CHTTP_WEB_OK || token == NULL) {
    fprintf(stderr, "CSRF rotate failed: status=%d native=%d message=%s\n",
            (int)error.status, error.native_status, error.message);
    return SALTS_EIO;
  }
  return chttp_server_reply(
      response, 200u, "text/plain; charset=utf-8",
      token, CHTTP_WEB_CSRF_TOKEN_BYTES);
}

static int web_session_invalidate(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  (void)user;
  if (request->session == NULL ||
      chttp_session_invalidate(request->session) != SALTS_OK)
    return SALTS_EIO;
  return web_session_reply_empty(response, 204u);
}

static int web_session_flash_post(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_form_pair pairs[8];
  char bytes[2048];
  chttp_web_form form = {0};
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  chttp_web_flash_config config =
      (chttp_web_flash_config)CHTTP_WEB_FLASH_CONFIG_INIT;
  chttp_web_status status;
  (void)user;

  config.max_messages = 2u;
  if (!web_session_content_type_form(request))
    return web_session_reply_empty(response, 415u);
  status = web_session_parse_form(
      request, pairs, bytes, &form, &error);
  if (status != CHTTP_WEB_OK)
    return web_session_reply_empty(response, 400u);
  status = chttp_web_csrf_validate(request, &form, &error);
  if (status != CHTTP_WEB_OK)
    return web_session_reply_csrf_failure(response, status, &error);

  status = chttp_web_flash_push(
      request->session, &config, "info", "saved", &error);
  if (status == CHTTP_WEB_OK)
    status = chttp_web_flash_push(
        request->session, &config, "warning", "check", &error);
  if (status != CHTTP_WEB_OK) {
    fprintf(stderr, "flash push failed: status=%d native=%d message=%s\n",
            (int)error.status, error.native_status, error.message);
    return SALTS_EIO;
  }

  status = chttp_web_flash_push(
      request->session, &config, "extra", "must-fail", &error);
  if (status != CHTTP_WEB_CAPACITY) {
    fprintf(stderr, "flash count bound did not fail closed: status=%d\n",
            (int)status);
    return SALTS_EIO;
  }
  return web_session_reply_empty(response, 204u);
}

static int web_session_flash_get(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  chttp_web_flash_message messages[2];
  char bytes[512];
  char output[512];
  chttp_web_flash_buffer buffer =
      (chttp_web_flash_buffer)CHTTP_WEB_FLASH_BUFFER_INIT;
  chttp_web_flash_config config =
      (chttp_web_flash_config)CHTTP_WEB_FLASH_CONFIG_INIT;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  size_t count = 0u;
  size_t output_size = 0u;
  size_t i;
  chttp_web_status status;
  (void)user;

  config.max_messages = 2u;
  buffer.message_storage = messages;
  buffer.message_capacity = 2u;
  buffer.byte_storage = bytes;
  buffer.byte_capacity = sizeof(bytes);

  status = chttp_web_flash_consume(
      request->session, &config, &buffer, &count, &error);
  if (status != CHTTP_WEB_OK) {
    fprintf(stderr, "flash consume failed: status=%d native=%d message=%s\n",
            (int)error.status, error.native_status, error.message);
    return SALTS_EIO;
  }

  for (i = 0u; i < count; ++i) {
    const size_t required =
        messages[i].level.size + 1u + messages[i].text.size + 1u;
    if (required > sizeof(output) - output_size)
      return SALTS_ENOBUFS;
    memcpy(output + output_size,
           messages[i].level.data, messages[i].level.size);
    output_size += messages[i].level.size;
    output[output_size++] = ':';
    memcpy(output + output_size,
           messages[i].text.data, messages[i].text.size);
    output_size += messages[i].text.size;
    output[output_size++] = '\n';
  }

  return chttp_server_reply(
      response, 200u, "text/plain; charset=utf-8",
      output, output_size);
}

int main(void) {
  chttp_server server = {0};
  chttp_server_config config = {0};
  uint16_t port = 0u;
  int status;

  config.host = "127.0.0.1";
  config.port = 0u;
  config.backlog = WEB_SESSION_CONNECTIONS;
#if defined(_WIN32)
  config.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  config.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
  config.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
  config.network.connection_capacity = WEB_SESSION_CONNECTIONS;
  config.network.command_capacity = WEB_SESSION_COMMANDS;
  config.network.request_capacity = WEB_SESSION_COMMANDS;
  config.network.completion_batch_capacity = WEB_SESSION_CONNECTIONS;
  config.network.event_capacity = WEB_SESSION_COMMANDS;
  config.network.max_send_bytes = WEB_SESSION_SEND_BYTES;
  config.network.command_buffer_bytes = WEB_SESSION_COMMAND_BUFFER_BYTES;
  config.network.receive_buffer_bytes = WEB_SESSION_HEADER_BYTES;
  config.network.connect_timeout_ms = WEB_SESSION_TIMEOUT_MS;
  config.network.read_timeout_ms = WEB_SESSION_TIMEOUT_MS;
  config.network.write_timeout_ms = WEB_SESSION_TIMEOUT_MS;
  config.route_capacity = 8u;
  config.middleware_capacity = 1u;
  config.max_route_middleware_count = 1u;
  config.max_route_param_count = 1u;
  config.max_route_param_bytes = WEB_SESSION_PATH_BYTES;
  config.max_target_bytes = WEB_SESSION_PATH_BYTES;
  config.max_header_count = WEB_SESSION_HEADERS;
  config.max_header_bytes = WEB_SESSION_HEADER_BYTES;
  config.max_request_body_bytes = WEB_SESSION_BODY_BYTES;
  config.max_response_header_count = WEB_SESSION_HEADERS;
  config.max_response_header_bytes = WEB_SESSION_HEADER_BYTES;
  config.max_response_body_bytes = WEB_SESSION_BODY_BYTES;
  config.max_buffered_response_body_bytes = WEB_SESSION_BODY_BYTES;
  config.buffer_capacity_bytes = WEB_SESSION_BUFFER_CAPACITY_BYTES;
  config.session_capacity = 8u;
  config.session_entry_capacity = 4u;
  config.max_session_key_bytes = 64u;
  config.max_session_value_bytes = 1024u;
  config.session_idle_timeout_ms = 60000u;
  config.session_cookie_name = "chttp_web_session";
  config.poll_slice_ms = 1u;

  status = chttp_server_init(&server, &config);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/safe", web_session_safe, NULL);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/csrf", web_session_token, NULL);
  if (status == SALTS_OK)
    status = chttp_server_post(
        &server, "/mutate", web_session_mutate, NULL);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/rotate", web_session_rotate, NULL);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/invalidate", web_session_invalidate, NULL);
  if (status == SALTS_OK)
    status = chttp_server_post(
        &server, "/flash", web_session_flash_post, NULL);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/flash", web_session_flash_get, NULL);
  if (status == SALTS_OK) status = chttp_server_start(&server);
  if (status == SALTS_OK) status = chttp_server_port(&server, &port);

  if (status == SALTS_OK) {
    printf("CHttp::Web session example: http://127.0.0.1:%u/\n",
           (unsigned)port);
    fflush(stdout);
    (void)getchar();
  }

  if (server.impl) {
    const int stopped =
        chttp_server_stop(&server, WEB_SESSION_TIMEOUT_MS);
    if (status == SALTS_OK) status = stopped;
    if (stopped == SALTS_OK) {
      const int destroyed = chttp_server_destroy(&server);
      if (status == SALTS_OK) status = destroyed;
    }
  }

  if (status != SALTS_OK)
    fprintf(stderr, "CHttp::Web session example failed: %d\n", status);
  return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
