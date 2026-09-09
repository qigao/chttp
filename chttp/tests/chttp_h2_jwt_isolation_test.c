#include "tinytest.h"

#include <chttp/chttp.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { CHTTP_H2_JWT_ISOLATION_TIMEOUT_MS = 5000 };

typedef struct chttp_h2_jwt_isolation_probe {
  atomic_int protected_calls;
  atomic_int public_calls;
} chttp_h2_jwt_isolation_probe;

typedef struct chttp_h2_jwt_isolation_completion {
  size_t calls;
  int status;
  unsigned int response_status;
  char body[16];
  size_t body_size;
} chttp_h2_jwt_isolation_completion;

static native_io_backend_kind chttp_h2_jwt_isolation_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config chttp_h2_jwt_isolation_network(size_t connections) {
  const cnet_client_config config = {
      .backend = chttp_h2_jwt_isolation_backend(),
      .connection_capacity = connections,
      .command_capacity = 16u,
      .request_capacity = 16u,
      .completion_batch_capacity = 8u,
      .event_capacity = 32u,
      .max_send_bytes = 64u * 1024u,
      .receive_buffer_bytes = 4096u,
      .connect_timeout_ms = CHTTP_H2_JWT_ISOLATION_TIMEOUT_MS,
      .read_timeout_ms = CHTTP_H2_JWT_ISOLATION_TIMEOUT_MS,
      .write_timeout_ms = CHTTP_H2_JWT_ISOLATION_TIMEOUT_MS};
  return config;
}

static chttp_server_config chttp_h2_jwt_isolation_server_config(void) {
  const chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = chttp_h2_jwt_isolation_network(2u),
      .route_capacity = 4u,
      .middleware_capacity = 2u,
      .max_route_middleware_count = 2u,
      .max_route_param_count = 2u,
      .max_route_param_bytes = 64u,
      .max_target_bytes = 128u,
      .max_header_count = 16u,
      .max_header_bytes = 4096u,
      .max_request_body_bytes = 4096u,
      .max_response_header_count = 16u,
      .max_response_header_bytes = 4096u,
      .max_response_body_bytes = 4096u,
      .session_capacity = 2u,
      .session_entry_capacity = 2u,
      .max_session_key_bytes = 32u,
      .max_session_value_bytes = 64u,
      .session_idle_timeout_ms = 60000u,
      .session_cookie_name = "h2_jwt_sid",
      .poll_slice_ms = 2u,
      .enable_http2 = 1,
      .h2_stream_capacity = 4u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_output_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
  return config;
}

static chttp_client_config chttp_h2_jwt_isolation_client_config(void) {
  const chttp_client_config config = {
      .network = chttp_h2_jwt_isolation_network(1u),
      .request_capacity = 4u,
      .max_start_line_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 4096u,
      .max_request_body_bytes = 4096u,
      .max_response_body_bytes = 4096u,
      .max_informational_responses = 2u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
  return config;
}

static int chttp_h2_jwt_isolation_protected_handler(
    void *user, const chttp_server_request_view *request, chttp_server_response *response) {
  chttp_h2_jwt_isolation_probe *probe = (chttp_h2_jwt_isolation_probe *)user;
  if (probe == NULL || request == NULL || request->http_major != 2u ||
      request->jwt_claims == NULL || request->jwt_claims->subject == NULL ||
      strcmp(request->jwt_claims->subject, "alice") != 0)
    return SALTS_EPROTO;
  atomic_fetch_add_explicit(&probe->protected_calls, 1, memory_order_relaxed);
  return chttp_server_reply(response, 200u, "text/plain", "protected", 9u);
}

static int chttp_h2_jwt_isolation_public_handler(
    void *user, const chttp_server_request_view *request, chttp_server_response *response) {
  chttp_h2_jwt_isolation_probe *probe = (chttp_h2_jwt_isolation_probe *)user;
  if (probe == NULL || request == NULL || request->http_major != 2u ||
      request->jwt_claims != NULL)
    return SALTS_EPROTO;
  atomic_fetch_add_explicit(&probe->public_calls, 1, memory_order_relaxed);
  return chttp_server_reply(response, 200u, "text/plain", "public", 6u);
}

static void chttp_h2_jwt_isolation_complete(void *user, chttp_request request,
                                            const chttp_response_view *response,
                                            const chttp_error *error) {
  chttp_h2_jwt_isolation_completion *completion =
      (chttp_h2_jwt_isolation_completion *)user;
  size_t body_size;
  (void)request;
  if (completion == NULL) return;
  ++completion->calls;
  if (error != NULL) {
    completion->status = error->status;
    return;
  }
  if (response == NULL) {
    completion->status = SALTS_EPROTO;
    return;
  }
  completion->status = SALTS_OK;
  completion->response_status = response->status_code;
  body_size = response->body_size < sizeof(completion->body) - 1u
                  ? response->body_size
                  : sizeof(completion->body) - 1u;
  if (body_size != 0u && response->body != NULL)
    memcpy(completion->body, response->body, body_size);
  completion->body[body_size] = '\0';
  completion->body_size = body_size;
}

spec("CHTTP HTTP/2 JWT identity isolation") {
  it("isolates JWT claims across sibling and later public streams on one connection") {
    static const unsigned char key[] = "0123456789abcdef0123456789abcdef";
    const chttp_jwt_claims claims = {
        .subject = "alice", .expires_at = INT64_C(3000000000)};
    const chttp_jwt_bearer_validator_options validator_options = {
        .size = sizeof(validator_options), .key = key, .key_size = sizeof(key) - 1u};
    chttp_h2_jwt_isolation_probe probe;
    chttp_jwt_bearer_validator validator = {0};
    chttp_server server = {0};
    chttp_async_client client = {0};
    chttp_server_config server_config = chttp_h2_jwt_isolation_server_config();
    chttp_client_config client_config = chttp_h2_jwt_isolation_client_config();
    const chttp_server_route_options protected_route = {
        .method = CHTTP_METHOD_GET,
        .path = "/protected",
        .handler = chttp_h2_jwt_isolation_protected_handler,
        .user = &probe};
    chttp_h2_jwt_isolation_completion protected = {0};
    chttp_h2_jwt_isolation_completion sibling = {0};
    chttp_h2_jwt_isolation_completion later = {0};
    chttp_request protected_request = {0};
    chttp_request sibling_request = {0};
    chttp_request later_request = {0};
    chttp_request_options options;
    chttp_header authorization = {0};
    chttp_server_stats stats = {0};
    char authorization_storage[512];
    char *token = NULL;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t polls = 0u;

    atomic_init(&probe.protected_calls, 0);
    atomic_init(&probe.public_calls, 0);
    check_equal(chttp_jwt_hs256_token_create(&claims, key, sizeof(key) - 1u, &token), SALTS_OK);
    check_equal(chttp_jwt_bearer_header(token, authorization_storage, sizeof(authorization_storage),
                                        &authorization),
                SALTS_OK);
    check_equal(chttp_jwt_bearer_validator_init(&validator, &validator_options), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_route_with_jwt_bearer(&server, &protected_route, &validator), SALTS_OK);
    check_equal(chttp_server_get(&server, "/public", chttp_h2_jwt_isolation_public_handler, &probe),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port) > 0);
    check_true(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port) > 0);
    check_equal(chttp_async_client_init(&client, &client_config), SALTS_OK);

    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/protected",
                                      .method = CHTTP_METHOD_GET,
                                      .headers = &authorization,
                                      .header_count = 1u,
                                      .on_complete = chttp_h2_jwt_isolation_complete,
                                      .user = &protected,
                                      .protocol = CHTTP_HTTP_2};
    check_equal(chttp_async_client_submit(&client, &options, &protected_request), SALTS_OK);
    options.target = "/public";
    options.headers = NULL;
    options.header_count = 0u;
    options.user = &sibling;
    check_equal(chttp_async_client_submit(&client, &options, &sibling_request), SALTS_OK);

    while ((protected.calls == 0u || sibling.calls == 0u) && polls++ < 40u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);
    check_equal(protected.calls, (size_t)1u);
    check_equal(protected.status, SALTS_OK);
    check_equal(protected.response_status, 200u);
    check_equal(protected.body, "protected");
    check_equal(sibling.calls, (size_t)1u);
    check_equal(sibling.status, SALTS_OK);
    check_equal(sibling.response_status, 200u);
    check_equal(sibling.body, "public");

    options.user = &later;
    check_equal(chttp_async_client_submit(&client, &options, &later_request), SALTS_OK);
    polls = 0u;
    while (later.calls == 0u && polls++ < 40u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);
    check_equal(later.calls, (size_t)1u);
    check_equal(later.status, SALTS_OK);
    check_equal(later.response_status, 200u);
    check_equal(later.body, "public");

    check_equal(atomic_load_explicit(&probe.protected_calls, memory_order_relaxed), 1);
    check_equal(atomic_load_explicit(&probe.public_calls, memory_order_relaxed), 2);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)1u);
    check_equal(stats.requests, (uint64_t)3u);
    check_equal(stats.responses, (uint64_t)3u);

    check_equal(chttp_async_client_stop(&client, CHTTP_H2_JWT_ISOLATION_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_JWT_ISOLATION_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_jwt_bearer_validator_destroy(&validator), SALTS_OK);
    chttp_jwt_token_destroy(token);
  }
}
