#include <chttp_web/web.h>

#include <http_server/rate_limit.h>
#include <salts/error_codes.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  WEB_SECURITY_TIMEOUT_MS = 5000,
  WEB_SECURITY_CONNECTIONS = 8,
  WEB_SECURITY_COMMANDS = 32,
  WEB_SECURITY_HEADER_BYTES = 8192,
  WEB_SECURITY_BODY_BYTES = 4096,
  WEB_SECURITY_SEND_BYTES = 64 * 1024,
  WEB_SECURITY_BUFFER_CAPACITY_BYTES = 512 * 1024
};

static const unsigned char WEB_SECURITY_JWT_KEY[32] = {
    0x31, 0x42, 0x53, 0x64, 0x75, 0x86, 0x97, 0xa8,
    0xb9, 0xca, 0xdb, 0xec, 0xfd, 0x0e, 0x1f, 0x20,
    0x41, 0x52, 0x63, 0x74, 0x85, 0x96, 0xa7, 0xb8,
    0xc9, 0xda, 0xeb, 0xfc, 0x0d, 0x1e, 0x2f, 0x30};

typedef struct web_security_app {
  char *token;
} web_security_app;

static int web_security_reply(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  (void)user;
  (void)request;
  return chttp_server_reply(
      response, 200u, "text/plain; charset=utf-8", "ok", 2u);
}

static int web_security_override(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  int status;
  (void)user;
  (void)request;
  status = chttp_server_response_set_header(
      response, "Referrer-Policy", "strict-origin");
  return status == SALTS_OK
      ? chttp_server_reply(
            response, 200u, "text/plain; charset=utf-8",
            "override", 8u)
      : status;
}

static int web_security_token(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  const web_security_app *app = (const web_security_app *)user;
  (void)request;
  if (app == NULL || app->token == NULL) return SALTS_EINVAL;
  return chttp_server_reply(
      response, 200u, "text/plain; charset=utf-8",
      app->token, strlen(app->token));
}

int main(void) {
  static const char *const origins[] = {"https://app.example"};
  const chttp_server_cors_options cors = {
      .origins = origins,
      .origin_count = 1u,
      .methods = "GET, OPTIONS",
      .allowed_headers = "Authorization",
      .exposed_headers = "X-Test",
      .max_age_seconds = 600u,
      .allow_credentials = 0};
  chttp_web_security_policy security =
      chttp_web_security_authenticated_policy();
  chttp_rate_limit_config rate_config = {
      .scope = CHTTP_RATE_LIMIT_GLOBAL,
      .group_capacity = 1u,
      .burst = 100u,
      .refill_tokens = 100u,
      .refill_period_ms = 60000u,
      .key_bytes = 64u};
  chttp_rate_limiter limiter = {0};
  chttp_jwt_bearer_validator validator = {0};
  chttp_jwt_bearer_validator_options validator_options = {
      .size = sizeof(chttp_jwt_bearer_validator_options),
      .key = WEB_SECURITY_JWT_KEY,
      .key_size = sizeof(WEB_SECURITY_JWT_KEY)};
  chttp_jwt_claims claims = {
      .subject = "security-test"};
  web_security_app app = {0};
  chttp_server server = {0};
  chttp_server_config config = {0};
  chttp_server_route_options protected_route = {0};
  uint16_t port = 0u;
  int status;

  security.strict_transport_security =
      "max-age=31536000; includeSubDomains";

  status = chttp_jwt_hs256_token_create(
      &claims, WEB_SECURITY_JWT_KEY,
      sizeof(WEB_SECURITY_JWT_KEY), &app.token);
  if (status != SALTS_OK) return EXIT_FAILURE;
  status = chttp_rate_limiter_init(&limiter, &rate_config);
  if (status == SALTS_OK)
    status = chttp_jwt_bearer_validator_init(
        &validator, &validator_options);
  if (status != SALTS_OK) {
    chttp_rate_limiter_destroy(&limiter);
    chttp_jwt_token_destroy(app.token);
    return EXIT_FAILURE;
  }

  config.host = "127.0.0.1";
  config.port = 0u;
  config.backlog = WEB_SECURITY_CONNECTIONS;
#if defined(_WIN32)
  config.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  config.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
  config.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
  config.network.connection_capacity = WEB_SECURITY_CONNECTIONS;
  config.network.command_capacity = WEB_SECURITY_COMMANDS;
  config.network.request_capacity = WEB_SECURITY_COMMANDS;
  config.network.completion_batch_capacity = WEB_SECURITY_CONNECTIONS;
  config.network.event_capacity = WEB_SECURITY_COMMANDS;
  config.network.max_send_bytes = WEB_SECURITY_SEND_BYTES;
  config.network.command_buffer_bytes = WEB_SECURITY_HEADER_BYTES;
  config.network.receive_buffer_bytes = WEB_SECURITY_HEADER_BYTES;
  config.network.connect_timeout_ms = WEB_SECURITY_TIMEOUT_MS;
  config.network.read_timeout_ms = WEB_SECURITY_TIMEOUT_MS;
  config.network.write_timeout_ms = WEB_SECURITY_TIMEOUT_MS;
  config.route_capacity = 8u;
  config.middleware_capacity = 4u;
  config.max_route_middleware_count = 2u;
  config.max_route_param_count = 1u;
  config.max_route_param_bytes = 64u;
  config.max_target_bytes = 256u;
  config.max_header_count = 32u;
  config.max_header_bytes = WEB_SECURITY_HEADER_BYTES;
  config.max_request_body_bytes = WEB_SECURITY_BODY_BYTES;
  config.max_response_header_count = 32u;
  config.max_response_header_bytes = WEB_SECURITY_HEADER_BYTES;
  config.max_response_body_bytes = WEB_SECURITY_BODY_BYTES;
  config.max_buffered_response_body_bytes = WEB_SECURITY_BODY_BYTES;
  config.buffer_capacity_bytes = WEB_SECURITY_BUFFER_CAPACITY_BYTES;
  config.poll_slice_ms = 1u;

  status = chttp_server_init(&server, &config);
  if (status == SALTS_OK)
    status = chttp_server_set_admission(
        &server, chttp_rate_limiter_admit, &limiter);
  if (status == SALTS_OK)
    status = chttp_web_security_use(&server, &security);
  if (status == SALTS_OK)
    status = chttp_server_use_cors(&server, &cors);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/token", web_security_token, &app);
  if (status == SALTS_OK)
    status = chttp_server_get(
        &server, "/override", web_security_override, NULL);

  protected_route = (chttp_server_route_options){
      .method = CHTTP_METHOD_GET,
      .path = "/secure",
      .handler = web_security_reply};
  if (status == SALTS_OK)
    status = chttp_server_route_with_jwt_bearer(
        &server, &protected_route, &validator);
  if (status == SALTS_OK)
    status = chttp_server_options(
        &server, "/secure", web_security_reply, NULL);
  if (status == SALTS_OK) status = chttp_server_start(&server);
  if (status == SALTS_OK) status = chttp_server_port(&server, &port);

  if (status == SALTS_OK) {
    printf("CHttp::Web security example: http://127.0.0.1:%u/\n",
           (unsigned)port);
    fflush(stdout);
    (void)getchar();
  }

  if (server.impl) {
    const int stopped =
        chttp_server_stop(&server, WEB_SECURITY_TIMEOUT_MS);
    if (status == SALTS_OK) status = stopped;
    if (stopped == SALTS_OK) {
      const int destroyed = chttp_server_destroy(&server);
      if (status == SALTS_OK) status = destroyed;
    }
  }
  chttp_jwt_bearer_validator_destroy(&validator);
  chttp_rate_limiter_destroy(&limiter);
  chttp_jwt_token_destroy(app.token);

  if (status != SALTS_OK)
    fprintf(stderr, "CHttp::Web security example failed: %d\n", status);
  return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
