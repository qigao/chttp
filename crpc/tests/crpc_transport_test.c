#include "chttp_tls_test_material.h"
#include "tinytest.h"

#include <crpc/crpc.h>
#include <salts/clock.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CRPC_TRANSPORT_TEST_TIMEOUT_MS = 5000,
  CRPC_TRANSPORT_TEST_H2_BUFFER_BYTES = 64u * 1024u,
  CRPC_TRANSPORT_TEST_H2_STREAM_CAPACITY = 8,
  CRPC_TRANSPORT_TEST_H2_SETTINGS_CAPACITY = 16
};

typedef struct crpc_transport_test_probe {
  size_t h1_requests;
  size_t h2_requests;
} crpc_transport_test_probe;

typedef struct crpc_transport_test_completion {
  int called;
  int status;
  uint64_t request_id;
  uint64_t result;
} crpc_transport_test_completion;

static native_io_backend_kind crpc_transport_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config crpc_transport_test_network(size_t connection_capacity) {
  const cnet_client_config config = {.backend = crpc_transport_test_backend(),
                                     .connection_capacity = connection_capacity,
                                     .command_capacity = 32u,
                                     .request_capacity = 16u,
                                     .completion_batch_capacity = 8u,
                                     .event_capacity = 32u,
                                     .max_send_bytes = CRPC_TRANSPORT_TEST_H2_BUFFER_BYTES,
                                     .receive_buffer_bytes = 4096u,
                                     .connect_timeout_ms = CRPC_TRANSPORT_TEST_TIMEOUT_MS,
                                     .read_timeout_ms = CRPC_TRANSPORT_TEST_TIMEOUT_MS,
                                     .write_timeout_ms = CRPC_TRANSPORT_TEST_TIMEOUT_MS,
                                     .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
                                     .tls_handshake_timeout_ms = CRPC_TRANSPORT_TEST_TIMEOUT_MS};
  return config;
}

static chttp_server_config crpc_transport_test_server_config(void) {
  const chttp_server_config config = {.host = "127.0.0.1",
                                      .port = 0u,
                                      .backlog = 8u,
                                      .network = crpc_transport_test_network(4u),
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
                                      .poll_slice_ms = 2u,
                                      .enable_http2 = 1,
                                      .h2_stream_capacity = CRPC_TRANSPORT_TEST_H2_STREAM_CAPACITY,
                                      .h2_input_buffer_bytes = CRPC_TRANSPORT_TEST_H2_BUFFER_BYTES,
                                      .h2_output_buffer_bytes = CRPC_TRANSPORT_TEST_H2_BUFFER_BYTES,
                                      .h2_hpack_dynamic_table_bytes = 4096u,
                                      .h2_max_settings_count =
                                          CRPC_TRANSPORT_TEST_H2_SETTINGS_CAPACITY};
  return config;
}

static crpc_client_config crpc_transport_test_client_config(void) {
  const crpc_client_config config = {
      .http = {.network = crpc_transport_test_network(4u),
               .request_capacity = 4u,
               .max_start_line_bytes = 256u,
               .max_header_count = 16u,
               .max_header_bytes = 4096u,
               .max_request_body_bytes = 4096u,
               .max_response_body_bytes = 4096u,
               .max_informational_responses = 2u,
               .h2_input_buffer_bytes = CRPC_TRANSPORT_TEST_H2_BUFFER_BYTES,
               .h2_hpack_dynamic_table_bytes = 4096u,
               .h2_max_settings_count = CRPC_TRANSPORT_TEST_H2_SETTINGS_CAPACITY},
      .request_capacity = 2u,
      .max_method_bytes = 64u,
      .max_json_depth = 8u};
  return config;
}

static int crpc_transport_test_handler(void *user, const chttp_server_request_view *request,
                                       chttp_server_response *response) {
  static const char expected[] = "{\"jsonrpc\":\"2.0\",\"method\":\"math.double\",\"id\":9}";
  static const char body[] = "{\"jsonrpc\":\"2.0\",\"result\":14,\"id\":9}";
  crpc_transport_test_probe *probe = (crpc_transport_test_probe *)user;
  if (probe == NULL || request == NULL || response == NULL ||
      (request->http_major != 1u && request->http_major != 2u) || request->body == NULL ||
      request->body_size != sizeof(expected) - 1u ||
      memcmp(request->body, expected, sizeof(expected) - 1u) != 0)
    return SALTS_EPROTO;
  if (request->http_major == 2u) ++probe->h2_requests;
  else ++probe->h1_requests;
  return chttp_server_reply(response, 200u, "application/json", body, sizeof(body) - 1u);
}

static void crpc_transport_test_complete(void *user, crpc_request request,
                                         const crpc_response_view *response,
                                         const crpc_error *error) {
  crpc_transport_test_completion *completion = (crpc_transport_test_completion *)user;
  cserde_token token = {0};
  (void)request;
  if (completion == NULL) return;
  completion->called = 1;
  if (error != NULL) {
    completion->status = error->status;
    return;
  }
  if (response == NULL || response->kind != CRPC_RESPONSE_RESULT ||
      cserde_reader_next(response->value.result, &token) != CSERDE_OK ||
      token.kind != CSERDE_UINT) {
    completion->status = SALTS_EPROTO;
    return;
  }
  completion->status = SALTS_OK;
  completion->request_id = response->request_id;
  completion->result = token.value.uint;
}

static int crpc_transport_test_poll_until(crpc_async_client *client,
                                          crpc_transport_test_completion *completion) {
  const uint64_t deadline = salts_monotonic_ms() + CRPC_TRANSPORT_TEST_TIMEOUT_MS;
  while (!completion->called) {
    size_t count = 0u;
    const int status = crpc_async_client_poll(client, 5u, &count);
    if (status != SALTS_OK) return status;
    if (salts_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
  }
  return completion->status;
}

static int crpc_transport_test_clients(const char *uri, const chttp_tls_profile *tls,
                                       chttp_protocol protocol) {
  crpc_client client = {0};
  crpc_async_client async_client = {0};
  crpc_client_config config = crpc_transport_test_client_config();
  crpc_options options = {.connection_uri = uri,
                          .authority = "localhost",
                          .target = "/rpc",
                          .method = {.service = "math", .name = "double"},
                          .request_id = UINT64_C(9),
                          .deadline_ms = CRPC_TRANSPORT_TEST_TIMEOUT_MS,
                          .tls = tls,
                          .protocol = protocol};
  crpc_response response = {0};
  crpc_error error = {0};
  crpc_request request = {0};
  crpc_transport_test_completion completion = {0};
  cserde_token token = {0};
  int client_initialized = 0;
  int async_initialized = 0;
  int status;

  status = crpc_client_init(&client, &config);
  if (status != SALTS_OK) goto done;
  client_initialized = 1;
  status = crpc_request_reply(&client, &options, &response, &error);
  if (status != SALTS_OK) goto done;
  if (response.kind != CRPC_RESPONSE_RESULT || response.request_id != UINT64_C(9) ||
      cserde_reader_next(response.value.result, &token) != CSERDE_OK || token.kind != CSERDE_UINT ||
      token.value.uint != UINT64_C(14)) {
    status = SALTS_EPROTO;
    goto done;
  }
  crpc_response_destroy(&response);
  status = crpc_client_destroy(&client, CRPC_TRANSPORT_TEST_TIMEOUT_MS);
  client_initialized = 0;
  if (status != SALTS_OK) goto done;

  status = crpc_async_client_init(&async_client, &config);
  if (status != SALTS_OK) goto done;
  async_initialized = 1;
  status = crpc_async_client_submit(&async_client, &options, crpc_transport_test_complete,
                                    &completion, &request);
  if (status != SALTS_OK) goto done;
  status = crpc_transport_test_poll_until(&async_client, &completion);
  if (status != SALTS_OK || completion.request_id != UINT64_C(9) ||
      completion.result != UINT64_C(14)) {
    if (status == SALTS_OK) status = SALTS_EPROTO;
    goto done;
  }

done:
  crpc_response_destroy(&response);
  if (async_initialized) {
    const int stop_status = crpc_async_client_stop(&async_client, CRPC_TRANSPORT_TEST_TIMEOUT_MS);
    const int destroy_status = crpc_async_client_destroy(&async_client);
    if (status == SALTS_OK && stop_status != SALTS_OK) status = stop_status;
    if (status == SALTS_OK && destroy_status != SALTS_OK) status = destroy_status;
  }
  if (client_initialized) {
    const int destroy_status = crpc_client_destroy(&client, CRPC_TRANSPORT_TEST_TIMEOUT_MS);
    if (status == SALTS_OK && destroy_status != SALTS_OK) status = destroy_status;
  }
  return status;
}

spec("CRPC CHTTP transport parity") {
  it("supports synchronous and asynchronous JSON-RPC over cleartext HTTP/2") {
    chttp_server server = {0};
    chttp_server_config config = crpc_transport_test_server_config();
    crpc_transport_test_probe probe = {0};
    char uri[64];
    uint16_t port = 0u;

    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(chttp_server_post(&server, "/rpc", crpc_transport_test_handler, &probe), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(crpc_transport_test_clients(uri, NULL, CHTTP_HTTP_2), SALTS_OK);
    check_equal(probe.h1_requests, (size_t)0u);
    check_equal(probe.h2_requests, (size_t)2u);
    check_equal(chttp_server_stop(&server, CRPC_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("supports synchronous and asynchronous JSON-RPC over TLS HTTP/1.1 and HTTP/2") {
    static const char *server_alpn[] = {"h2", "http/1.1"};
    static const char *h2[] = {"h2"};
    static const char *h1[] = {"http/1.1"};
    chttp_server server = {0};
    chttp_tls_profile h2_profile = {0};
    chttp_tls_profile h1_profile = {0};
    chttp_server_config config = crpc_transport_test_server_config();
    crpc_transport_test_probe probe = {0};
    cnet_tls_server_config server_tls;
    cnet_tls_client_config h2_tls;
    cnet_tls_client_config h1_tls;
    char *cert_path = tt_make_temp_file("crpc-h2-cert", ".pem");
    char *key_path = tt_make_temp_file("crpc-h2-key", ".pem");
    char uri[64];
    uint16_t port = 0u;

    check_not_null(cert_path);
    check_not_null(key_path);
    check_equal(tt_write_file(cert_path, CHTTP_TLS_TEST_CERTIFICATE,
                              sizeof(CHTTP_TLS_TEST_CERTIFICATE) - 1u),
                0);
    check_equal(tt_write_file(key_path, CHTTP_TLS_TEST_KEY, sizeof(CHTTP_TLS_TEST_KEY) - 1u), 0);
    server_tls = (cnet_tls_server_config){.size = sizeof(server_tls),
                                          .cert_file = cert_path,
                                          .key_file = key_path,
                                          .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
                                          .alpn_protocols = server_alpn,
                                          .alpn_protocol_count = 2u};
    h2_tls = (cnet_tls_client_config){.size = sizeof(h2_tls),
                                      .ca_file = cert_path,
                                      .server_name = "localhost",
                                      .alpn_protocols = h2,
                                      .alpn_protocol_count = 1u};
    h1_tls = h2_tls;
    h1_tls.alpn_protocols = h1;
    config.tls = &server_tls;

    check_equal(chttp_tls_profile_init(&h2_profile, &h2_tls), SALTS_OK);
    check_equal(chttp_tls_profile_init(&h1_profile, &h1_tls), SALTS_OK);
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(chttp_server_post(&server, "/rpc", crpc_transport_test_handler, &probe), SALTS_OK);
    check_equal(tt_remove_file(cert_path), 0);
    check_equal(tt_remove_file(key_path), 0);
    free(cert_path);
    free(key_path);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(crpc_transport_test_clients(uri, &h2_profile, CHTTP_HTTP_2), SALTS_OK);
    check_equal(crpc_transport_test_clients(uri, &h1_profile, CHTTP_HTTP_1_1), SALTS_OK);
    check_equal(probe.h1_requests, (size_t)2u);
    check_equal(probe.h2_requests, (size_t)2u);
    check_equal(chttp_tls_profile_destroy(&h1_profile), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&h2_profile), SALTS_OK);
    check_equal(chttp_server_stop(&server, CRPC_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("rejects a TLS ALPN profile that contradicts the selected RPC protocol") {
    static const char *h2[] = {"h2"};
    crpc_async_client client = {0};
    chttp_tls_profile profile = {0};
    crpc_client_config config = crpc_transport_test_client_config();
    cnet_tls_client_config tls = {
        .size = sizeof(tls), .alpn_protocols = h2, .alpn_protocol_count = 1u};
    crpc_transport_test_completion completion = {0};
    crpc_request request = {7u, 9u};
    crpc_options options = {.connection_uri = "tls://127.0.0.1:443",
                            .authority = "localhost",
                            .target = "/rpc",
                            .method = {.name = "ping"},
                            .request_id = UINT64_C(1),
                            .tls = &profile,
                            .protocol = CHTTP_HTTP_1_1};

    check_equal(chttp_tls_profile_init(&profile, &tls), SALTS_OK);
    check_equal(crpc_async_client_init(&client, &config), SALTS_OK);
    check_equal(crpc_async_client_submit(&client, &options, crpc_transport_test_complete,
                                         &completion, &request),
                SALTS_EPROTONOSUPPORT);
    check_equal(request.slot, 0u);
    check_equal(request.generation, 0u);
    check_equal(completion.called, 0);
    check_equal(crpc_async_client_stop(&client, CRPC_TRANSPORT_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
  }
}
