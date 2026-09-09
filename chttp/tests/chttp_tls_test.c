#include "chttp_tls_test_material.h"
#include "tinytest.h"

#include <chttp/chttp.h>
#include <salts/clock.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CHTTP_TLS_TEST_TIMEOUT_MS = 5000 };

typedef struct chttp_tls_test_probe {
  size_t middleware_calls;
  cnet_stream_peer peer;
  char peer_certificate_sha256[CNET_TLS_PEER_CERTIFICATE_SHA256_CAPACITY];
} chttp_tls_test_probe;

typedef struct chttp_tls_async_probe {
  int called;
  int status;
  unsigned int status_code;
  unsigned char body[8];
  size_t body_size;
} chttp_tls_async_probe;

static native_io_backend_kind chttp_tls_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config chttp_tls_test_network(size_t connection_capacity) {
  const cnet_client_config config = {.backend = chttp_tls_test_backend(),
                                     .connection_capacity = connection_capacity,
                                     .command_capacity = 32u,
                                     .request_capacity = 16u,
                                     .completion_batch_capacity = 8u,
                                     .event_capacity = 32u,
                                     .max_send_bytes = 4096u,
                                     .receive_buffer_bytes = 1024u,
                                     .connect_timeout_ms = CHTTP_TLS_TEST_TIMEOUT_MS,
                                     .read_timeout_ms = CHTTP_TLS_TEST_TIMEOUT_MS,
                                     .write_timeout_ms = CHTTP_TLS_TEST_TIMEOUT_MS,
                                     .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
                                     .tls_handshake_timeout_ms = CHTTP_TLS_TEST_TIMEOUT_MS};
  return config;
}

static chttp_client_config chttp_tls_test_client_config(void) {
  const chttp_client_config config = {.network = chttp_tls_test_network(4u),
                                      .request_capacity = 4u,
                                      .max_start_line_bytes = 256u,
                                      .max_header_count = 16u,
                                      .max_header_bytes = 1024u,
                                      .max_request_body_bytes = 512u,
                                      .max_response_body_bytes = 512u,
                                      .max_informational_responses = 2u};
  return config;
}

static chttp_server_config chttp_tls_test_server_config(void) {
  const chttp_server_config config = {.host = "127.0.0.1",
                                      .port = 0u,
                                      .backlog = 8u,
                                      .network = chttp_tls_test_network(4u),
                                      .route_capacity = 4u,
                                      .middleware_capacity = 2u,
                                      .max_route_middleware_count = 2u,
                                      .max_route_param_count = 2u,
                                      .max_route_param_bytes = 64u,
                                      .max_target_bytes = 128u,
                                      .max_header_count = 16u,
                                      .max_header_bytes = 1024u,
                                      .max_request_body_bytes = 512u,
                                      .max_response_header_count = 16u,
                                      .max_response_header_bytes = 1024u,
                                      .max_response_body_bytes = 512u,
                                      .session_capacity = 4u,
                                      .session_entry_capacity = 2u,
                                      .max_session_key_bytes = 16u,
                                      .max_session_value_bytes = 16u,
                                      .session_idle_timeout_ms = 60000u,
                                      .session_cookie_name = "secure_sid",
                                      .session_cookie_secure = 1,
                                      .poll_slice_ms = 2u};
  return config;
}

static int chttp_tls_test_middleware(void *user, const chttp_server_request_view *request,
                                     chttp_server_response *response, chttp_server_next *next) {
  chttp_tls_test_probe *probe = (chttp_tls_test_probe *)user;
  int status;
  if (request->peer != NULL) probe->peer = *request->peer;
  if (request->peer_certificate_sha256 != NULL)
    memcpy(probe->peer_certificate_sha256, request->peer_certificate_sha256,
           CNET_TLS_PEER_CERTIFICATE_SHA256_CAPACITY);
  ++probe->middleware_calls;
  status = chttp_server_response_set_header(response, "X-TLS-Middleware", "yes");
  return status == SALTS_OK ? chttp_server_next_call(next) : status;
}

static int chttp_tls_test_session(void *user, const chttp_server_request_view *request,
                                  chttp_server_response *response) {
  const char *previous = chttp_session_get(request->session, "visits");
  char current[2] = {'1', '\0'};
  int status;
  (void)user;
  if (previous != NULL) current[0] = (char)(previous[0] + 1);
  status = chttp_session_set(request->session, "visits", current);
  return status == SALTS_OK ? chttp_server_reply(response, 200u, "text/plain", current, 1u)
                            : status;
}

static int chttp_tls_test_cookie(const chttp_response *response, char *output, size_t capacity) {
  const char *set_cookie = chttp_response_header(response, "Set-Cookie");
  const char *end;
  size_t size;
  if (set_cookie == NULL || output == NULL || capacity == 0u) return SALTS_EINVAL;
  end = strchr(set_cookie, ';');
  if (end == NULL) return SALTS_EPROTO;
  size = (size_t)(end - set_cookie);
  if (size >= capacity) return SALTS_EMSGSIZE;
  memcpy(output, set_cookie, size);
  output[size] = '\0';
  return SALTS_OK;
}

static int chttp_tls_test_get(chttp_client *client, const char *uri,
                              const chttp_tls_profile *profile, const chttp_header *headers,
                              size_t header_count, chttp_response *response) {
  const chttp_options options = {.connection_uri = uri,
                                 .authority = "localhost",
                                 .target = "/session",
                                 .headers = headers,
                                 .header_count = header_count,
                                 .timeout_ms = CHTTP_TLS_TEST_TIMEOUT_MS,
                                 .tls = profile};
  chttp_error error = {0};
  return chttp_get(client, &options, response, &error);
}

static void chttp_tls_test_noop_complete(void *user, chttp_request request,
                                         const chttp_response_view *response,
                                         const chttp_error *error) {
  (void)user;
  (void)request;
  (void)response;
  (void)error;
}

static void chttp_tls_test_async_complete(void *user, chttp_request request,
                                          const chttp_response_view *response,
                                          const chttp_error *error) {
  chttp_tls_async_probe *probe = (chttp_tls_async_probe *)user;
  (void)request;
  probe->called = 1;
  if (error != NULL) {
    probe->status = error->status;
    return;
  }
  if (response == NULL || response->body_size > sizeof(probe->body)) {
    probe->status = response == NULL ? SALTS_EPROTO : SALTS_EMSGSIZE;
    return;
  }
  probe->status = SALTS_OK;
  probe->status_code = response->status_code;
  probe->body_size = response->body_size;
  if (response->body_size != 0u) memcpy(probe->body, response->body, response->body_size);
}

static int chttp_tls_test_poll_until(chttp_async_client *client, chttp_tls_async_probe *probe) {
  const uint64_t deadline = salts_monotonic_ms() + CHTTP_TLS_TEST_TIMEOUT_MS;
  while (probe->called == 0) {
    size_t completions = 0u;
    const int status = chttp_async_client_poll(client, 5u, &completions);
    if (status != SALTS_OK) return status;
    if (salts_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
  }
  return SALTS_OK;
}

spec("CHTTP HTTPS adapter") {
  it("rejects unsupported or incomplete server TLS policy before listen") {
    static const char *h2[] = {"h2"};
    static const char *h1[] = {"http/1.1"};
    chttp_server server = {0};
    chttp_server_config config = chttp_tls_test_server_config();
    cnet_tls_server_config tls = {.size = sizeof(tls),
                                  .cert_file = "missing-cert.pem",
                                  .key_file = "missing-key.pem",
                                  .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
                                  .alpn_protocols = h2,
                                  .alpn_protocol_count = 1u};

    config.tls = &tls;
    check_equal(chttp_server_init(&server, &config), SALTS_ENOTSUP);
    check_null(server.impl);

    tls.alpn_protocols = h1;
    config.network.tls_io_buffer_bytes = 0u;
    config.network.tls_handshake_timeout_ms = 0u;
    check_equal(chttp_server_init(&server, &config), SALTS_EINVAL);
    check_null(server.impl);

    config.network = chttp_tls_test_network(4u);
    tls.cert_file = NULL;
    check_equal(chttp_server_init(&server, &config), SALTS_EINVAL);
    check_null(server.impl);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("validates TLS profile lifecycle and explicit HTTP ALPN") {
    static const char *h2[] = {"h2"};
    static const char *mixed[] = {"h2", "http/1.1"};
    chttp_tls_profile profile = {0};
    cnet_tls_client_config config = {.size = sizeof(config)};

    check_equal(chttp_tls_profile_init(NULL, &config), SALTS_EINVAL);
    check_equal(chttp_tls_profile_init(&profile, NULL), SALTS_EINVAL);
    config.alpn_protocols = h2;
    config.alpn_protocol_count = 1u;
    check_equal(chttp_tls_profile_init(&profile, &config), SALTS_OK);
    check_not_null(profile.impl);
    check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
    config.alpn_protocols = mixed;
    config.alpn_protocol_count = 2u;
    check_equal(chttp_tls_profile_init(&profile, &config), SALTS_ENOTSUP);
    check_null(profile.impl);
    config.alpn_protocols = NULL;
    config.alpn_protocol_count = 0u;
    check_equal(chttp_tls_profile_init(&profile, &config), SALTS_OK);
    check_not_null(profile.impl);
    check_equal(chttp_tls_profile_init(&profile, &config), SALTS_EALREADY);
    check_equal(chttp_tls_profile_destroy(NULL), SALTS_EINVAL);
    check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
    check_null(profile.impl);
    check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
  }

  it("rejects an HTTP/2 TLS profile for a default HTTP/1.1 request") {
    static const char *h2[] = {"h2"};
    chttp_async_client client = {0};
    chttp_tls_profile profile = {0};
    cnet_tls_client_config tls = {
        .size = sizeof(tls), .alpn_protocols = h2, .alpn_protocol_count = 1u};
    chttp_client_config config = chttp_tls_test_client_config();
    chttp_request request = {9u, 9u};
    chttp_request_options options = {.connection_uri = "tls://127.0.0.1:443",
                                     .authority = "localhost",
                                     .target = "/",
                                     .method = CHTTP_METHOD_GET,
                                     .on_complete = chttp_tls_test_noop_complete,
                                     .tls = &profile};

    check_equal(chttp_tls_profile_init(&profile, &tls), SALTS_OK);
    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_async_client_submit(&client, &options, &request), SALTS_EPROTONOSUPPORT);
    check_equal(request.slot, 0u);
    check_equal(request.generation, 0u);
    check_equal(chttp_async_client_stop(&client, CHTTP_TLS_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
  }

  it("rejects a TLS profile on a plaintext request") {
    chttp_async_client client = {0};
    chttp_tls_profile profile = {0};
    cnet_tls_client_config tls = {.size = sizeof(tls)};
    chttp_client_config config = chttp_tls_test_client_config();
    chttp_request request = {9u, 9u};
    chttp_request_options options = {.connection_uri = "tcp://127.0.0.1:443",
                                     .authority = "localhost",
                                     .target = "/",
                                     .method = CHTTP_METHOD_GET,
                                     .on_complete = chttp_tls_test_noop_complete,
                                     .tls = &profile};

    check_equal(chttp_tls_profile_init(&profile, &tls), SALTS_OK);
    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_async_client_submit(&client, &options, &request), SALTS_EINVAL);
    check_equal(request.slot, 0u);
    check_equal(request.generation, 0u);
    check_equal(chttp_async_client_stop(&client, CHTTP_TLS_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
  }

  it("serves HTTPS middleware and sessions while isolating pool profiles") {
    static const char *h1[] = {"http/1.1"};
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_async_client async_client = {0};
    chttp_tls_profile first_profile = {0};
    chttp_tls_profile second_profile = {0};
    chttp_tls_profile transient_profile = {0};
    chttp_tls_test_probe probe = {0};
    chttp_tls_async_probe async_probe = {0};
    chttp_server_config server_config = chttp_tls_test_server_config();
    chttp_client_config client_config = chttp_tls_test_client_config();
    cnet_tls_server_config server_tls;
    cnet_tls_client_config client_tls;
    chttp_response response = {0};
    chttp_server_stats stats = {0};
    chttp_header cookie_header;
    chttp_request async_request = {0};
    chttp_request_options async_options;
    char cookie[128];
    char uri[64];
    char *cert_path = tt_make_temp_file("chttp-cert", ".pem");
    char *key_path = tt_make_temp_file("chttp-key", ".pem");
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
                                          .ca_file = cert_path,
                                          .client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED,
                                          .alpn_protocols = h1,
                                          .alpn_protocol_count = 1u};
    client_tls = (cnet_tls_client_config){.size = sizeof(client_tls),
                                          .ca_file = cert_path,
                                          .cert_file = cert_path,
                                          .key_file = key_path,
                                          .server_name = "localhost",
                                          .alpn_protocols = h1,
                                          .alpn_protocol_count = 1u};
    server_config.tls = &server_tls;

    check_equal(chttp_tls_profile_init(&first_profile, &client_tls), SALTS_OK);
    check_equal(chttp_tls_profile_init(&second_profile, &client_tls), SALTS_OK);
    check_equal(chttp_tls_profile_init(&transient_profile, &client_tls), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_use(&server, chttp_tls_test_middleware, &probe), SALTS_OK);
    check_equal(chttp_server_get(&server, "/session", chttp_tls_test_session, NULL), SALTS_OK);
    check_equal(tt_remove_file(cert_path), 0);
    check_equal(tt_remove_file(key_path), 0);
    free(cert_path);
    free(key_path);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    check_equal(chttp_tls_test_get(&client, uri, &first_profile, NULL, 0u, &response), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_equal(response.body, "1", 1u);
    check_equal(chttp_response_header(&response, "X-TLS-Middleware"), "yes");
    {
      const char *set_cookie = chttp_response_header(&response, "Set-Cookie");
      check_not_null(set_cookie);
      check_true(set_cookie != NULL && strstr(set_cookie, "Secure") != NULL);
    }
    check_equal(chttp_tls_test_cookie(&response, cookie, sizeof(cookie)), SALTS_OK);
    chttp_response_destroy(&response);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)1u);

    cookie_header = (chttp_header){"Cookie", cookie};
    check_equal(chttp_tls_test_get(&client, uri, &first_profile, &cookie_header, 1u, &response),
                SALTS_OK);
    check_equal(response.body, "2", 1u);
    chttp_response_destroy(&response);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)1u);

    check_equal(chttp_tls_test_get(&client, uri, &second_profile, &cookie_header, 1u, &response),
                SALTS_OK);
    check_equal(response.body, "3", 1u);
    chttp_response_destroy(&response);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)2u);
    check_equal(stats.requests, (uint64_t)3u);
    check_equal(probe.middleware_calls, (size_t)3u);
    check_equal(probe.peer.family, CNET_DATAGRAM_ADDRESS_IPV4);
    check_equal(probe.peer.address[0], (uint8_t)127u);
    check_true(probe.peer.port != 0u);
    check_equal(strlen(probe.peer_certificate_sha256), (size_t)64u);

    async_options = (chttp_request_options){.connection_uri = uri,
                                            .authority = "localhost",
                                            .target = "/session",
                                            .method = CHTTP_METHOD_GET,
                                            .headers = &cookie_header,
                                            .header_count = 1u,
                                            .on_complete = chttp_tls_test_async_complete,
                                            .user = &async_probe,
                                            .tls = &transient_profile};
    check_equal(chttp_async_client_init(&async_client, &client_config), SALTS_OK);
    check_equal(chttp_async_client_submit(&async_client, &async_options, &async_request), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&transient_profile), SALTS_OK);
    check_null(transient_profile.impl);
    check_equal(chttp_tls_test_poll_until(&async_client, &async_probe), SALTS_OK);
    check_equal(async_probe.status, SALTS_OK);
    check_equal(async_probe.status_code, 200u);
    check_equal(async_probe.body_size, (size_t)1u);
    check_equal(async_probe.body, "4", 1u);
    check_equal(chttp_async_client_stop(&async_client, CHTTP_TLS_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&async_client), SALTS_OK);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)3u);
    check_equal(stats.requests, (uint64_t)4u);
    check_equal(probe.middleware_calls, (size_t)4u);

    check_equal(chttp_tls_profile_destroy(&first_profile), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&second_profile), SALTS_OK);
    check_equal(chttp_client_destroy(&client, CHTTP_TLS_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_TLS_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }
}
