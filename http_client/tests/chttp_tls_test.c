#include "chttp_tls_test_material.h"
#include "tinytest.h"

#include <http_client/http.h>
#include <http_server/http.h>
#include <salts/clock.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CHTTP_TLS_TEST_TIMEOUT_MS = 5000 };

#if defined(CHTTP_X509_MATRIX_ACCEPTANCE_TESTS)
#define CHTTP_X509_PATH(name) CHTTP_X509_FIXTURE_DIR "/" name
#endif

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
  static char digits[] = "0123456789";
  const char *previous = chttp_session_get(request->session, "visits");
  char current[2] = {'1', '\0'};
  mem_buffer_t *buffer;
  int status;
  (void)user;
  if (previous != NULL) current[0] = (char)(previous[0] + 1);
  if (current[0] < '0' || current[0] > '9') return SALTS_ERANGE;
  status = chttp_session_set(request->session, "visits", current);
  if (status != SALTS_OK) return status;
  buffer = mem_wrap_external(
      &digits[(size_t)(current[0] - '0')], 1u, NULL, NULL);
  if (buffer == NULL) return SALTS_ENOMEM;
  mem_set_used(buffer, 1u);
  status = chttp_server_reply_buffer(
      response, 200u, "text/plain", buffer);
  mem_buffer_release(buffer);
  return status;
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
  const uint64_t deadline = cmeta_monotonic_ms() + CHTTP_TLS_TEST_TIMEOUT_MS;
  while (probe->called == 0) {
    size_t completions = 0u;
    const int status = chttp_async_client_poll(client, 5u, &completions);
    if (status != SALTS_OK) return status;
    if (cmeta_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
  }
  return SALTS_OK;
}

#if defined(CHTTP_X509_MATRIX_ACCEPTANCE_TESTS)
static int chttp_tls_test_matrix_case(const char *name, const char *ca_file,
                                      const char *cert_file, const char *key_file,
                                      int expect_success) {
  static const char *h1[] = {"http/1.1"};
  chttp_server server = {0};
  chttp_client client = {0};
  chttp_tls_profile profile = {0};
  chttp_tls_test_probe probe = {0};
  chttp_server_config server_config = chttp_tls_test_server_config();
  chttp_client_config client_config = chttp_tls_test_client_config();
  cnet_tls_server_config server_tls = {.size = sizeof(server_tls),
                                        .cert_file = cert_file,
                                        .key_file = key_file,
                                        .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
                                        .alpn_protocols = h1,
                                        .alpn_protocol_count = 1u};
  cnet_tls_client_config client_tls = {.size = sizeof(client_tls),
                                        .ca_file = ca_file,
                                        .server_name = "localhost",
                                        .alpn_protocols = h1,
                                        .alpn_protocol_count = 1u};
  chttp_response response = {0};
  char uri[64];
  uint16_t port = 0u;
  int profile_ready = 0, server_ready = 0, server_started = 0, client_ready = 0;
  int status, result = SALTS_EPROTO;

  status = chttp_tls_profile_init(&profile, &client_tls);
  if (status != SALTS_OK) {
    result = expect_success ? status :
                              (probe.middleware_calls == 0u ? SALTS_OK : SALTS_EPROTO);
    goto cleanup;
  }
  profile_ready = 1;
  server_config.tls = &server_tls;
  status = chttp_server_init(&server, &server_config);
  if (status != SALTS_OK) {
    result = expect_success ? status :
                              (probe.middleware_calls == 0u ? SALTS_OK : SALTS_EPROTO);
    goto cleanup;
  }
  server_ready = 1;
  if (chttp_server_use(&server, chttp_tls_test_middleware, &probe) != SALTS_OK ||
      chttp_server_get(&server, "/session", chttp_tls_test_session, NULL) != SALTS_OK ||
      chttp_server_start(&server) != SALTS_OK) {
    result = expect_success ? SALTS_EIO :
                              (probe.middleware_calls == 0u ? SALTS_OK : SALTS_EPROTO);
    goto cleanup;
  }
  server_started = 1;
  if (chttp_server_port(&server, &port) != SALTS_OK ||
      snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port) <= 0 ||
      chttp_client_init(&client, &client_config) != SALTS_OK) {
    result = SALTS_EIO;
    goto cleanup;
  }
  client_ready = 1;
  status = chttp_tls_test_get(&client, uri, &profile, NULL, 0u, &response);
  if (expect_success) {
    result = status == SALTS_OK && response.status_code == 200u &&
                     response.body_size == 1u && response.body != NULL &&
                     memcmp(response.body, "1", 1u) == 0 &&
                     probe.middleware_calls == 1u
                 ? SALTS_OK
                 : SALTS_EPROTO;
  } else {
    result = status != SALTS_OK && probe.middleware_calls == 0u
                 ? SALTS_OK
                 : SALTS_EPROTO;
  }

cleanup:
  chttp_response_destroy(&response);
  if (client_ready && chttp_client_destroy(&client, CHTTP_TLS_TEST_TIMEOUT_MS) != SALTS_OK)
    result = SALTS_EIO;
  if (server_started && chttp_server_stop(&server, CHTTP_TLS_TEST_TIMEOUT_MS) != SALTS_OK)
    result = SALTS_EIO;
  if (server_ready && chttp_server_destroy(&server) != SALTS_OK) result = SALTS_EIO;
  if (profile_ready && chttp_tls_profile_destroy(&profile) != SALTS_OK) result = SALTS_EIO;
  if (result != SALTS_OK) fprintf(stderr, "X.509 matrix case failed: %s\n", name);
  return result;
}
#endif

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

#if defined(CHTTP_P384_SHA384_ACCEPTANCE_TESTS)
  it("verifies a P-384 SHA-384 issuing CA before dispatching HTTPS requests") {
    static const char *h1[] = {"http/1.1"};
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_tls_profile valid_profile = {0};
    chttp_tls_profile wrong_ca_profile = {0};
    chttp_tls_profile wrong_name_profile = {0};
    chttp_tls_test_probe probe = {0};
    chttp_server_config server_config = chttp_tls_test_server_config();
    chttp_client_config client_config = chttp_tls_test_client_config();
    cnet_tls_server_config server_tls;
    cnet_tls_client_config valid_tls;
    cnet_tls_client_config wrong_ca_tls;
    cnet_tls_client_config wrong_name_tls;
    chttp_response response = {0};
    const struct {
      const char *name;
      const char *certificate;
      size_t certificate_size;
    } rejected_server_certificates[] = {
        {"expired", CHTTP_TLS_TEST_P384_EXPIRED_SERVER_CERTIFICATE,
         sizeof(CHTTP_TLS_TEST_P384_EXPIRED_SERVER_CERTIFICATE) - 1u},
        {"tampered", CHTTP_TLS_TEST_P384_TAMPERED_SERVER_CERTIFICATE,
         sizeof(CHTTP_TLS_TEST_P384_TAMPERED_SERVER_CERTIFICATE) - 1u}};
    char uri[64];
    char *server_cert_path = tt_make_temp_file("chttp-p384-server-cert", ".pem");
    char *server_key_path = tt_make_temp_file("chttp-p384-server-key", ".pem");
    char *ca_path = tt_make_temp_file("chttp-p384-ca", ".pem");
    char *wrong_ca_path = tt_make_temp_file("chttp-p384-wrong-ca", ".pem");
    uint16_t port = 0u;
    int status;

    check_not_null(server_cert_path);
    check_not_null(server_key_path);
    check_not_null(ca_path);
    check_not_null(wrong_ca_path);
    check_equal(tt_write_file(server_cert_path, CHTTP_TLS_TEST_P384_SERVER_CERTIFICATE,
                              sizeof(CHTTP_TLS_TEST_P384_SERVER_CERTIFICATE) - 1u),
                0);
    check_equal(tt_write_file(server_key_path, CHTTP_TLS_TEST_P384_SERVER_KEY,
                              sizeof(CHTTP_TLS_TEST_P384_SERVER_KEY) - 1u),
                0);
    check_equal(tt_write_file(ca_path, CHTTP_TLS_TEST_P384_CA_CERTIFICATE,
                              sizeof(CHTTP_TLS_TEST_P384_CA_CERTIFICATE) - 1u),
                0);
    check_equal(tt_write_file(wrong_ca_path, CHTTP_TLS_TEST_P384_WRONG_CA_CERTIFICATE,
                              sizeof(CHTTP_TLS_TEST_P384_WRONG_CA_CERTIFICATE) - 1u),
                0);

    server_tls = (cnet_tls_server_config){.size = sizeof(server_tls),
                                          .cert_file = server_cert_path,
                                          .key_file = server_key_path,
                                          .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
                                          .alpn_protocols = h1,
                                          .alpn_protocol_count = 1u};
    valid_tls = (cnet_tls_client_config){.size = sizeof(valid_tls),
                                         .ca_file = ca_path,
                                         .server_name = "localhost",
                                         .alpn_protocols = h1,
                                         .alpn_protocol_count = 1u};
    wrong_ca_tls = valid_tls;
    wrong_ca_tls.ca_file = wrong_ca_path;
    wrong_name_tls = valid_tls;
    wrong_name_tls.server_name = "wrong.example";
    server_config.tls = &server_tls;

    check_equal(chttp_tls_profile_init(&valid_profile, &valid_tls), SALTS_OK);
    check_equal(chttp_tls_profile_init(&wrong_ca_profile, &wrong_ca_tls), SALTS_OK);
    check_equal(chttp_tls_profile_init(&wrong_name_profile, &wrong_name_tls), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_use(&server, chttp_tls_test_middleware, &probe), SALTS_OK);
    check_equal(chttp_server_get(&server, "/session", chttp_tls_test_session, NULL), SALTS_OK);

    check_equal(tt_remove_file(server_cert_path), 0);
    check_equal(tt_remove_file(server_key_path), 0);
    check_equal(tt_remove_file(ca_path), 0);
    check_equal(tt_remove_file(wrong_ca_path), 0);
    free(server_cert_path);
    free(server_key_path);
    free(ca_path);
    free(wrong_ca_path);

    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    check_equal(chttp_tls_test_get(&client, uri, &valid_profile, NULL, 0u, &response), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_equal(response.body, "1", 1u);
    chttp_response_destroy(&response);
    check_equal(probe.middleware_calls, (size_t)1u);

    status = chttp_tls_test_get(&client, uri, &wrong_ca_profile, NULL, 0u, &response);
    check_not_equal(status, SALTS_OK);
    check_equal(probe.middleware_calls, (size_t)1u);
    chttp_response_destroy(&response);

    status = chttp_tls_test_get(&client, uri, &wrong_name_profile, NULL, 0u, &response);
    check_not_equal(status, SALTS_OK);
    check_equal(probe.middleware_calls, (size_t)1u);
    chttp_response_destroy(&response);

    check_equal(chttp_client_destroy(&client, CHTTP_TLS_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_TLS_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);

    for (size_t case_index = 0u;
         case_index < sizeof(rejected_server_certificates) /
                          sizeof(rejected_server_certificates[0]);
         ++case_index) {
      char *rejected_cert_path = tt_make_temp_file(
          rejected_server_certificates[case_index].name, ".pem");
      char *rejected_key_path = tt_make_temp_file("chttp-p384-rejected-key", ".pem");
      check_not_null(rejected_cert_path);
      check_not_null(rejected_key_path);
      check_equal(tt_write_file(rejected_cert_path,
                                rejected_server_certificates[case_index].certificate,
                                rejected_server_certificates[case_index].certificate_size),
                  0);
      check_equal(tt_write_file(rejected_key_path, CHTTP_TLS_TEST_P384_SERVER_KEY,
                                sizeof(CHTTP_TLS_TEST_P384_SERVER_KEY) - 1u),
                  0);

      server = (chttp_server){0};
      client = (chttp_client){0};
      server_tls.cert_file = rejected_cert_path;
      server_tls.key_file = rejected_key_path;
      server_config.tls = &server_tls;
      check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
      check_equal(chttp_server_use(&server, chttp_tls_test_middleware, &probe), SALTS_OK);
      check_equal(chttp_server_get(&server, "/session", chttp_tls_test_session, NULL), SALTS_OK);
      check_equal(tt_remove_file(rejected_cert_path), 0);
      check_equal(tt_remove_file(rejected_key_path), 0);
      free(rejected_cert_path);
      free(rejected_key_path);

      check_equal(chttp_server_start(&server), SALTS_OK);
      check_equal(chttp_server_port(&server, &port), SALTS_OK);
      check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
      check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
      status = chttp_tls_test_get(&client, uri, &valid_profile, NULL, 0u, &response);
      check_not_equal(status, SALTS_OK);
      check_equal(probe.middleware_calls, (size_t)1u);
      chttp_response_destroy(&response);
      check_equal(chttp_client_destroy(&client, CHTTP_TLS_TEST_TIMEOUT_MS), SALTS_OK);
      check_equal(chttp_server_stop(&server, CHTTP_TLS_TEST_TIMEOUT_MS), SALTS_OK);
      check_equal(chttp_server_destroy(&server), SALTS_OK);
    }

    check_equal(chttp_tls_profile_destroy(&valid_profile), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&wrong_ca_profile), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&wrong_name_profile), SALTS_OK);
  }
#endif

#if defined(CHTTP_X509_MATRIX_ACCEPTANCE_TESTS)
  it("qualifies common X.509 profiles through real HTTPS dispatch") {
    const struct {
      const char *name;
      const char *ca_file;
      const char *cert_file;
      const char *key_file;
      int expect_success;
    } cases[] = {
        {"P384 intermediate chain", CHTTP_X509_PATH("p384-chain-root-cert.pem"),
         CHTTP_X509_P384_FULLCHAIN, CHTTP_X509_PATH("p384-chain-server-key.pem"), 1},
        {"missing P384 intermediate", CHTTP_X509_PATH("p384-chain-root-cert.pem"),
         CHTTP_X509_PATH("p384-chain-server-cert.pem"),
         CHTTP_X509_PATH("p384-chain-server-key.pem"), 0},
        {"CA false intermediate", CHTTP_X509_PATH("invalid-ca-root-cert.pem"),
         CHTTP_X509_INVALID_CA_FULLCHAIN,
         CHTTP_X509_PATH("invalid-ca-server-key.pem"), 0},
        {"P256 SHA256", CHTTP_X509_PATH("p256-test-root-cert.pem"),
         CHTTP_X509_PATH("p256-test-server-cert.pem"),
         CHTTP_X509_PATH("p256-test-server-key.pem"), 1},
        {"RSA3072 SHA256", CHTTP_X509_PATH("rsa3072-test-root-cert.pem"),
         CHTTP_X509_PATH("rsa3072-test-server-cert.pem"),
         CHTTP_X509_PATH("rsa3072-test-server-key.pem"), 1},
        {"RSA-PSS certificate signature", CHTTP_X509_PATH("rsa3072-test-root-cert.pem"),
         CHTTP_X509_PATH("rsa3072-test-server-pss-cert.pem"),
         CHTTP_X509_PATH("rsa3072-test-server-key.pem"), 0},
        {"ECDSA SHA512 certificate signature", CHTTP_X509_PATH("sha512-mixed-root-cert.pem"),
         CHTTP_X509_PATH("sha512-mixed-server-cert.pem"),
         CHTTP_X509_PATH("sha512-mixed-server-key.pem"), 0},
        {"P521 issuer", CHTTP_X509_PATH("unsupported-p521-root-cert.pem"),
         CHTTP_X509_PATH("unsupported-p521-server-cert.pem"),
         CHTTP_X509_PATH("sha512-mixed-server-key.pem"), 0},
        {"Ed25519 identity", CHTTP_X509_PATH("p384-chain-root-cert.pem"),
         CHTTP_X509_PATH("unsupported-ed25519-server-cert.pem"),
         CHTTP_X509_PATH("unsupported-ed25519-server-key.pem"), 0}};
    size_t case_index;
    for (case_index = 0u; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index)
      check_equal(chttp_tls_test_matrix_case(
                      cases[case_index].name, cases[case_index].ca_file,
                      cases[case_index].cert_file, cases[case_index].key_file,
                      cases[case_index].expect_success),
                  SALTS_OK);
  }
#endif

  it("serves HTTPS middleware and sessions across fixed owners") {
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
    chttp_server_execution_options execution =
        (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
    cnet_tls_server_config server_tls;
    cnet_tls_client_config client_tls;
    chttp_response response = {0};
    chttp_server_stats stats = {0};
    chttp_header cookie_header;
    chttp_request async_request = {0};
    chttp_request_options async_options;
    char cookie[128];
    char uri[64];
    char *server_cert_path = tt_make_temp_file("chttp-server-cert", ".pem");
    char *server_key_path = tt_make_temp_file("chttp-server-key", ".pem");
    char *server_ca_path = tt_make_temp_file("chttp-client-ca", ".pem");
    char *client_cert_path = tt_make_temp_file("chttp-client-cert", ".pem");
    char *client_key_path = tt_make_temp_file("chttp-client-key", ".pem");
    char *client_ca_path = tt_make_temp_file("chttp-server-ca", ".pem");
    uint16_t port = 0u;

    check_not_null(server_cert_path);
    check_not_null(server_key_path);
    check_not_null(server_ca_path);
    check_not_null(client_cert_path);
    check_not_null(client_key_path);
    check_not_null(client_ca_path);
    check_equal(
        tt_write_file(server_cert_path, CHTTP_TLS_TEST_CERTIFICATE,
                      sizeof(CHTTP_TLS_TEST_CERTIFICATE) - 1u),
        0);
    check_equal(
        tt_write_file(server_key_path, CHTTP_TLS_TEST_KEY,
                      sizeof(CHTTP_TLS_TEST_KEY) - 1u),
        0);
    check_equal(
        tt_write_file(server_ca_path, CHTTP_TLS_TEST_CLIENT_CA_CERTIFICATE,
                      sizeof(CHTTP_TLS_TEST_CLIENT_CA_CERTIFICATE) - 1u),
        0);
    check_equal(
        tt_write_file(client_cert_path, CHTTP_TLS_TEST_CLIENT_CERTIFICATE,
                      sizeof(CHTTP_TLS_TEST_CLIENT_CERTIFICATE) - 1u),
        0);
    check_equal(
        tt_write_file(client_key_path, CHTTP_TLS_TEST_CLIENT_KEY,
                      sizeof(CHTTP_TLS_TEST_CLIENT_KEY) - 1u),
        0);
    check_equal(
        tt_write_file(client_ca_path, CHTTP_TLS_TEST_CA_CERTIFICATE,
                      sizeof(CHTTP_TLS_TEST_CA_CERTIFICATE) - 1u),
        0);
    server_tls = (cnet_tls_server_config){
        .size = sizeof(server_tls),
        .cert_file = server_cert_path,
        .key_file = server_key_path,
        .ca_file = server_ca_path,
        .client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED,
        .alpn_protocols = h1,
        .alpn_protocol_count = 1u};
    client_tls = (cnet_tls_client_config){
        .size = sizeof(client_tls),
        .ca_file = client_ca_path,
        .cert_file = client_cert_path,
        .key_file = client_key_path,
        .server_name = "localhost",
        .alpn_protocols = h1,
        .alpn_protocol_count = 1u};
    server_config.tls = &server_tls;

    check_equal(chttp_tls_profile_init(&first_profile, &client_tls), SALTS_OK);
    check_equal(chttp_tls_profile_init(&second_profile, &client_tls), SALTS_OK);
    check_equal(chttp_tls_profile_init(&transient_profile, &client_tls), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    execution.owner_count = 2u;
    check_equal(chttp_server_set_execution_options(&server, &execution), SALTS_OK);
    check_equal(chttp_server_use(&server, chttp_tls_test_middleware, &probe), SALTS_OK);
    check_equal(chttp_server_get(&server, "/session", chttp_tls_test_session, NULL), SALTS_OK);
    check_equal(tt_remove_file(server_cert_path), 0);
    check_equal(tt_remove_file(server_key_path), 0);
    check_equal(tt_remove_file(server_ca_path), 0);
    check_equal(tt_remove_file(client_cert_path), 0);
    check_equal(tt_remove_file(client_key_path), 0);
    check_equal(tt_remove_file(client_ca_path), 0);
    free(server_cert_path);
    free(server_key_path);
    free(server_ca_path);
    free(client_cert_path);
    free(client_key_path);
    free(client_ca_path);
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
