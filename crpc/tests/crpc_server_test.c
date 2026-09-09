#include "chttp_tls_test_material.h"
#include "tinytest.h"

#include <crpc/crpc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CRPC_SERVER_TEST_TIMEOUT_MS = 5000,
  CRPC_SERVER_TEST_BUFFER_BYTES = 64u * 1024u,
  CRPC_SERVER_TEST_H2_STREAM_CAPACITY = 8,
  CRPC_SERVER_TEST_H2_SETTINGS_CAPACITY = 16,
  CRPC_SERVER_TEST_MIN_BATCH_RESPONSE_BYTES = 389
};

typed_any(idempotent, int, crpc_server_test_identity, (int value)) { return value; }

typedef struct crpc_server_test_probe {
  size_t calls;
  size_t notifications;
  size_t middleware_calls;
  size_t callable_calls;
  int second_response_status;
} crpc_server_test_probe;

static const char crpc_server_test_h2_batch[] =
    "[{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":2},"
    "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}]";
static const char crpc_server_test_h2_batch_reply[] =
    "[{\"jsonrpc\":\"2.0\",\"result\":1,\"id\":2}]";

static native_io_backend_kind crpc_server_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config crpc_server_test_network(size_t connection_capacity) {
  const cnet_client_config config = {.backend = crpc_server_test_backend(),
                                     .connection_capacity = connection_capacity,
                                     .command_capacity = 32u,
                                     .request_capacity = 16u,
                                     .completion_batch_capacity = 8u,
                                     .event_capacity = 32u,
                                     .max_send_bytes = CRPC_SERVER_TEST_BUFFER_BYTES,
                                     .receive_buffer_bytes = 4096u,
                                     .connect_timeout_ms = CRPC_SERVER_TEST_TIMEOUT_MS,
                                     .read_timeout_ms = CRPC_SERVER_TEST_TIMEOUT_MS,
                                     .write_timeout_ms = CRPC_SERVER_TEST_TIMEOUT_MS,
                                     .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
                                     .tls_handshake_timeout_ms = CRPC_SERVER_TEST_TIMEOUT_MS};
  return config;
}

static chttp_server_config crpc_server_test_http_config(int enable_http2) {
  chttp_server_config config = {.host = "127.0.0.1",
                                .port = 0u,
                                .backlog = 8u,
                                .network = crpc_server_test_network(8u),
                                .route_capacity = 8u,
                                .middleware_capacity = 4u,
                                .max_route_middleware_count = 4u,
                                .max_route_param_count = 4u,
                                .max_route_param_bytes = 128u,
                                .max_target_bytes = 256u,
                                .max_header_count = 16u,
                                .max_header_bytes = 4096u,
                                .max_request_body_bytes = 8192u,
                                .max_response_header_count = 16u,
                                .max_response_header_bytes = 4096u,
                                .max_response_body_bytes = 8192u,
                                .session_capacity = 8u,
                                .session_entry_capacity = 4u,
                                .max_session_key_bytes = 32u,
                                .max_session_value_bytes = 64u,
                                .session_idle_timeout_ms = 60000u,
                                .session_cookie_name = "crpc_sid",
                                .poll_slice_ms = 2u,
                                .max_buffered_response_body_bytes = 8192u};
  if (enable_http2) {
    config.enable_http2 = 1;
    config.h2_stream_capacity = CRPC_SERVER_TEST_H2_STREAM_CAPACITY;
    config.h2_input_buffer_bytes = CRPC_SERVER_TEST_BUFFER_BYTES;
    config.h2_output_buffer_bytes = CRPC_SERVER_TEST_BUFFER_BYTES;
    config.h2_hpack_dynamic_table_bytes = 4096u;
    config.h2_max_settings_count = CRPC_SERVER_TEST_H2_SETTINGS_CAPACITY;
  }
  return config;
}

static crpc_server_config crpc_server_test_config(int enable_http2) {
  const crpc_server_config config = {.http = crpc_server_test_http_config(enable_http2),
                                     .method_capacity = 8u,
                                     .max_method_bytes = 64u,
                                     .max_json_depth = 8u,
                                     .max_batch_items = 4u};
  return config;
}

static chttp_client_config crpc_server_test_http_client_config(void) {
  const chttp_client_config config = {.network = crpc_server_test_network(4u),
                                      .request_capacity = 4u,
                                      .max_start_line_bytes = 256u,
                                      .max_header_count = 16u,
                                      .max_header_bytes = 4096u,
                                      .max_request_body_bytes = 8192u,
                                      .max_response_body_bytes = 8192u,
                                      .max_informational_responses = 2u,
                                      .h2_input_buffer_bytes = CRPC_SERVER_TEST_BUFFER_BYTES,
                                      .h2_hpack_dynamic_table_bytes = 4096u,
                                      .h2_max_settings_count =
                                          CRPC_SERVER_TEST_H2_SETTINGS_CAPACITY};
  return config;
}

static crpc_client_config crpc_server_test_client_config(void) {
  const crpc_client_config config = {.http = crpc_server_test_http_client_config(),
                                     .request_capacity = 2u,
                                     .max_method_bytes = 64u,
                                     .max_json_depth = 8u};
  return config;
}

static cserde_status crpc_server_test_write(cserde_writer *writer, cserde_token token) {
  return cserde_writer_write(writer, &token);
}

static cserde_status crpc_server_test_encode_uint(void *user, cserde_writer *writer) {
  const uint64_t value = *(const uint64_t *)user;
  return crpc_server_test_write(writer, (cserde_token){.kind = CSERDE_UINT, .value.uint = value});
}

static cserde_status crpc_server_test_encode_string(void *user, cserde_writer *writer) {
  const char *value = (const char *)user;
  return crpc_server_test_write(writer,
                                (cserde_token){.kind = CSERDE_STRING,
                                               .value.slice = {(const unsigned char *)value,
                                                               strlen(value), CSERDE_VIEW_STABLE}});
}

static cserde_status crpc_server_test_encode_params(void *user, cserde_writer *writer) {
  cserde_status status;
  (void)user;
  status = crpc_server_test_write(writer, (cserde_token){.kind = CSERDE_ARRAY_BEGIN});
  if (status == CSERDE_OK)
    status = crpc_server_test_write(writer,
                                    (cserde_token){.kind = CSERDE_UINT, .value.uint = UINT64_C(7)});
  if (status == CSERDE_OK)
    status = crpc_server_test_write(writer, (cserde_token){.kind = CSERDE_ARRAY_END});
  return status;
}

static int crpc_server_test_math(void *user, const crpc_server_request_view *request,
                                 crpc_server_response *response) {
  static const uint64_t result = UINT64_C(14);
  crpc_server_test_probe *probe = (crpc_server_test_probe *)user;
  cserde_token token = {0};
  const cmeta_sig_desc *signature;
  int status;
  if (probe == NULL || request == NULL || response == NULL || request->params == NULL ||
      strcmp(request->target, "/rpc/math") != 0 || strcmp(request->method, "math.double") != 0 ||
      cserde_reader_next(request->params, &token) != CSERDE_OK ||
      token.kind != CSERDE_ARRAY_BEGIN ||
      cserde_reader_next(request->params, &token) != CSERDE_OK || token.kind != CSERDE_UINT ||
      token.value.uint != UINT64_C(7) || cserde_reader_next(request->params, &token) != CSERDE_OK ||
      token.kind != CSERDE_ARRAY_END || cserde_reader_next(request->params, &token) != CSERDE_DONE)
    return SALTS_EPROTO;
  ++probe->calls;
  if (request->notification) ++probe->notifications;
  if (request->callable != NULL) {
    signature = cmeta_callable_signature(*request->callable);
    if (signature == NULL || signature->protocol != CMETA_FN_PROTOCOL_VALUE) return SALTS_EPROTO;
    ++probe->callable_calls;
  }
  status = crpc_server_response_result(response, crpc_server_test_encode_uint, (void *)&result);
  probe->second_response_status =
      crpc_server_response_result(response, crpc_server_test_encode_uint, (void *)&result);
  return status;
}

static int crpc_server_test_status(void *user, const crpc_server_request_view *request,
                                   crpc_server_response *response) {
  crpc_server_test_probe *probe = (crpc_server_test_probe *)user;
  (void)request;
  ++probe->calls;
  return crpc_server_response_result(response, crpc_server_test_encode_string, "ready");
}

static int crpc_server_test_ping(void *user, const crpc_server_request_view *request,
                                 crpc_server_response *response) {
  static const uint64_t result = UINT64_C(1);
  crpc_server_test_probe *probe = (crpc_server_test_probe *)user;
  ++probe->calls;
  if (request->notification) ++probe->notifications;
  return crpc_server_response_result(response, crpc_server_test_encode_uint, (void *)&result);
}

static int crpc_server_test_large_result(void *user, const crpc_server_request_view *request,
                                         crpc_server_response *response) {
  static const char result[] =
      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
  crpc_server_test_probe *probe = (crpc_server_test_probe *)user;
  ++probe->calls;
  if (request->notification) ++probe->notifications;
  return crpc_server_response_result(response, crpc_server_test_encode_string, (void *)result);
}

static int crpc_server_test_error(void *user, const crpc_server_request_view *request,
                                  crpc_server_response *response) {
  crpc_server_test_probe *probe = (crpc_server_test_probe *)user;
  (void)request;
  ++probe->calls;
  return crpc_server_response_error(response, -32000, "failed", crpc_server_test_encode_string,
                                    "detail");
}

static int crpc_server_test_incomplete(void *user, const crpc_server_request_view *request,
                                       crpc_server_response *response) {
  crpc_server_test_probe *probe = (crpc_server_test_probe *)user;
  (void)request;
  (void)response;
  ++probe->calls;
  return SALTS_OK;
}

static int crpc_server_test_middleware(void *user, const chttp_server_request_view *request,
                                       chttp_server_response *response, chttp_server_next *next) {
  crpc_server_test_probe *probe = (crpc_server_test_probe *)user;
  int status;
  if (probe == NULL || request == NULL || request->session == NULL) return SALTS_EPROTO;
  ++probe->middleware_calls;
  status = chttp_session_set(request->session, "transport", "crpc");
  if (status == SALTS_OK)
    status = chttp_server_response_set_header(response, "X-CRPC-Middleware", "yes");
  return status == SALTS_OK ? chttp_server_next_call(next) : status;
}

static int crpc_server_test_http_post(chttp_client *client, const char *uri, const char *target,
                                      const void *body, size_t body_size,
                                      const chttp_tls_profile *tls, chttp_protocol protocol,
                                      chttp_response *response) {
  static const chttp_header headers[] = {{"Content-Type", "application/json"},
                                         {"Accept", "application/json"}};
  const chttp_options options = {.connection_uri = uri,
                                 .authority = "localhost",
                                 .target = target,
                                 .headers = headers,
                                 .header_count = 2u,
                                 .body = body,
                                 .body_size = body_size,
                                 .timeout_ms = CRPC_SERVER_TEST_TIMEOUT_MS,
                                 .tls = tls,
                                 .protocol = protocol};
  chttp_error error = {0};
  return chttp_post(client, &options, response, &error);
}

spec("CRPC server") {
  it("validates bounded configuration, registry identity, and lifecycle") {
    crpc_server server = {0};
    crpc_server_config config = crpc_server_test_config(0);
    crpc_method ping = {.name = "ping"};
    crpc_method status = {.service = "system", .name = "status"};
    crpc_server_test_probe probe = {0};
    uint16_t port = 0u;

    check_equal(crpc_server_init(NULL, &config), SALTS_EINVAL);
    check_equal(crpc_server_init(&server, NULL), SALTS_EINVAL);
    config.method_capacity = 0u;
    check_equal(crpc_server_init(&server, &config), SALTS_EINVAL);
    config = crpc_server_test_config(0);
    config.max_json_depth = 1u;
    check_equal(crpc_server_init(&server, &config), SALTS_EINVAL);
    config = crpc_server_test_config(0);
    config.max_batch_items = 0u;
    check_equal(crpc_server_init(&server, &config), SALTS_EINVAL);
    config = crpc_server_test_config(0);
    config.http.max_buffered_response_body_bytes = 0u;
    check_equal(crpc_server_init(&server, &config), SALTS_EINVAL);
    config = crpc_server_test_config(0);
    config.http.max_buffered_response_body_bytes = 95u;
    check_equal(crpc_server_init(&server, &config), SALTS_EMSGSIZE);
    config = crpc_server_test_config(0);
    config.http.max_buffered_response_body_bytes = CRPC_SERVER_TEST_MIN_BATCH_RESPONSE_BYTES - 1u;
    check_equal(crpc_server_init(&server, &config), SALTS_EMSGSIZE);

    config = crpc_server_test_config(0);
    config.method_capacity = 3u;
    check_equal(crpc_server_init(&server, &config), SALTS_OK);
    check_not_null(crpc_server_http(&server));
    check_equal(crpc_server_init(&server, &config), SALTS_EALREADY);
    check_equal(crpc_server_register(&server, "/rpc/:tenant", &ping, crpc_server_test_ping, &probe),
                SALTS_EINVAL);
    check_equal(crpc_server_register(&server, "/rpc", &ping, crpc_server_test_ping, &probe),
                SALTS_OK);
    check_equal(crpc_server_register(&server, "/rpc", &ping, crpc_server_test_ping, &probe),
                SALTS_EALREADY);
    check_equal(crpc_server_register(&server, "/rpc", &status, crpc_server_test_status, &probe),
                SALTS_OK);
    check_equal(crpc_server_register(&server, "/other", &ping, crpc_server_test_ping, &probe),
                SALTS_OK);
    check_equal(crpc_server_register(&server, "/rpc", &ping, crpc_server_test_ping, &probe),
                SALTS_EALREADY);
    check_equal(crpc_server_register(&server, "/full", &ping, crpc_server_test_ping, &probe),
                SALTS_ENOBUFS);
    check_equal(crpc_server_start(&server), SALTS_OK);
    check_equal(crpc_server_port(&server, &port), SALTS_OK);
    check_true(port != 0u);
    check_equal(crpc_server_start(&server), SALTS_EALREADY);
    check_equal(crpc_server_register(&server, "/late", &ping, crpc_server_test_ping, &probe),
                SALTS_EBUSY);
    check_equal(crpc_server_destroy(&server), SALTS_EBUSY);
    check_equal(crpc_server_stop(&server, CRPC_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_server_destroy(&server), SALTS_OK);
    check_null(server.impl);
    check_equal(crpc_server_destroy(&server), SALTS_OK);
  }

  it("serves CMeta and CSerde unary methods on multiple endpoints with middleware sessions") {
    crpc_server server = {0};
    crpc_client client = {0};
    crpc_server_config server_config = crpc_server_test_config(0);
    crpc_client_config client_config = crpc_server_test_client_config();
    crpc_server_test_probe method_probe = {0};
    crpc_server_test_probe middleware_probe = {0};
    crpc_method math = {
        .service = "math", .name = "double", .callable = &crpc_server_test_identity};
    crpc_method status = {.service = "system", .name = "status"};
    crpc_options options;
    crpc_response response = {0};
    crpc_error error = {0};
    cserde_token token = {0};
    char uri[64];
    uint16_t port = 0u;

    check_equal(crpc_server_init(&server, &server_config), SALTS_OK);
    check_equal(
        chttp_server_use(crpc_server_http(&server), crpc_server_test_middleware, &middleware_probe),
        SALTS_OK);
    check_equal(
        crpc_server_register(&server, "/rpc/math", &math, crpc_server_test_math, &method_probe),
        SALTS_OK);
    check_equal(crpc_server_register(&server, "/rpc/status", &status, crpc_server_test_status,
                                     &method_probe),
                SALTS_OK);
    check_equal(crpc_server_start(&server), SALTS_OK);
    check_equal(crpc_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(crpc_client_init(&client, &client_config), SALTS_OK);

    options = (crpc_options){.connection_uri = uri,
                             .authority = "localhost",
                             .target = "/rpc/math",
                             .method = math,
                             .request_id = UINT64_C(9),
                             .deadline_ms = CRPC_SERVER_TEST_TIMEOUT_MS,
                             .encode_params = crpc_server_test_encode_params};
    check_equal(crpc_request_reply(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.kind, CRPC_RESPONSE_RESULT);
    check_not_null(response.callable);
    check_equal(cserde_reader_next(response.value.result, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_UINT);
    check_equal(token.value.uint, UINT64_C(14));
    crpc_response_destroy(&response);

    options = (crpc_options){.connection_uri = uri,
                             .authority = "localhost",
                             .target = "/rpc/status",
                             .method = status,
                             .request_id = UINT64_C(10),
                             .deadline_ms = CRPC_SERVER_TEST_TIMEOUT_MS};
    check_equal(crpc_request_reply(&client, &options, &response, &error), SALTS_OK);
    check_equal(cserde_reader_next(response.value.result, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_STRING);
    check_equal(token.value.slice.data, (const unsigned char *)"ready", 5u);
    crpc_response_destroy(&response);

    check_equal(method_probe.calls, (size_t)2u);
    check_equal(method_probe.callable_calls, (size_t)1u);
    check_equal(method_probe.second_response_status, SALTS_EALREADY);
    check_equal(middleware_probe.middleware_calls, (size_t)2u);
    check_equal(crpc_client_destroy(&client, CRPC_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_server_stop(&server, CRPC_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_server_destroy(&server), SALTS_OK);
  }

  it("suppresses notifications and emits bounded ordered batch responses") {
    static const char notification[] = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}";
    static const char unknown_notification[] = "{\"jsonrpc\":\"2.0\",\"method\":\"missing\"}";
    static const char invalid_notification[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"params\":1}";
    static const char batch[] = "[{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":3},"
                                "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"},7,"
                                "{\"jsonrpc\":\"2.0\",\"method\":\"missing\",\"id\":4}]";
    static const char batch_response[] =
        "[{\"jsonrpc\":\"2.0\",\"result\":1,\"id\":3},"
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"Invalid "
        "Request\"},\"id\":null},"
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32601,\"message\":\"Method not "
        "found\"},\"id\":4}]";
    static const char all_notifications[] = "[{\"jsonrpc\":\"2.0\",\"method\":\"ping\"},"
                                            "{\"jsonrpc\":\"2.0\",\"method\":\"missing\"}]";
    static const char too_many[] = "[{\"jsonrpc\":\"2.0\",\"method\":\"ping\"},"
                                   "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"},"
                                   "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"},"
                                   "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"},"
                                   "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}]";
    static const char invalid_response[] =
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"Invalid "
        "Request\"},\"id\":null}";
    static const char parse_response[] =
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"Parse "
        "error\"},\"id\":null}";
    static const char app_error[] = "{\"jsonrpc\":\"2.0\",\"method\":\"fail\",\"id\":5}";
    static const char app_error_response[] =
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32000,\"message\":\"failed\","
        "\"data\":\"detail\"},\"id\":5}";
    static const char incomplete[] = "{\"jsonrpc\":\"2.0\",\"method\":\"incomplete\",\"id\":6}";
    static const char internal_response[] =
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Internal "
        "error\"},\"id\":6}";
    static const char duplicate_version[] =
        "{\"jsonrpc\":\"2.0\",\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":7}";
    static const char wrong_version[] = "{\"jsonrpc\":\"1.0\",\"method\":\"ping\",\"id\":7}";
    static const char reserved_method[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"rpc.internal\",\"id\":7}";
    static const char null_id[] = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":null}";
    static const char scalar_params[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"params\":1,\"id\":7}";
    crpc_server server = {0};
    chttp_client client = {0};
    crpc_server_config server_config = crpc_server_test_config(0);
    chttp_client_config client_config = crpc_server_test_http_client_config();
    crpc_server_test_probe probe = {0};
    crpc_method ping = {.name = "ping"};
    crpc_method fail = {.name = "fail"};
    crpc_method no_reply = {.name = "incomplete"};
    chttp_response response = {0};
    char uri[64];
    uint16_t port = 0u;

    check_equal(crpc_server_init(&server, &server_config), SALTS_OK);
    check_equal(crpc_server_register(&server, "/rpc", &ping, crpc_server_test_ping, &probe),
                SALTS_OK);
    check_equal(crpc_server_register(&server, "/rpc", &fail, crpc_server_test_error, &probe),
                SALTS_OK);
    check_equal(
        crpc_server_register(&server, "/rpc", &no_reply, crpc_server_test_incomplete, &probe),
        SALTS_OK);
    check_equal(crpc_server_start(&server), SALTS_OK);
    check_equal(crpc_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", notification,
                                           sizeof(notification) - 1u, NULL, CHTTP_HTTP_1_1,
                                           &response),
                SALTS_OK);
    check_equal(response.status_code, 204u);
    check_equal(response.body_size, (size_t)0u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", unknown_notification,
                                           sizeof(unknown_notification) - 1u, NULL, CHTTP_HTTP_1_1,
                                           &response),
                SALTS_OK);
    check_equal(response.status_code, 204u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", invalid_notification,
                                           sizeof(invalid_notification) - 1u, NULL, CHTTP_HTTP_1_1,
                                           &response),
                SALTS_OK);
    check_equal(response.status_code, 204u);
    chttp_response_destroy(&response);

    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", batch, sizeof(batch) - 1u, NULL,
                                           CHTTP_HTTP_1_1, &response),
                SALTS_OK);
    check_equal(response.status_code, 200u);
    check_equal(response.body, batch_response, sizeof(batch_response) - 1u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", all_notifications,
                                           sizeof(all_notifications) - 1u, NULL, CHTTP_HTTP_1_1,
                                           &response),
                SALTS_OK);
    check_equal(response.status_code, 204u);
    chttp_response_destroy(&response);
    check_equal(
        crpc_server_test_http_post(&client, uri, "/rpc", "[]", 2u, NULL, CHTTP_HTTP_1_1, &response),
        SALTS_OK);
    check_equal(response.body, invalid_response, sizeof(invalid_response) - 1u);
    chttp_response_destroy(&response);
    check_equal(
        crpc_server_test_http_post(&client, uri, "/rpc", "{", 1u, NULL, CHTTP_HTTP_1_1, &response),
        SALTS_OK);
    check_equal(response.body, parse_response, sizeof(parse_response) - 1u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", too_many, sizeof(too_many) - 1u,
                                           NULL, CHTTP_HTTP_1_1, &response),
                SALTS_OK);
    check_equal(response.body, invalid_response, sizeof(invalid_response) - 1u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", app_error, sizeof(app_error) - 1u,
                                           NULL, CHTTP_HTTP_1_1, &response),
                SALTS_OK);
    check_equal(response.body, app_error_response, sizeof(app_error_response) - 1u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", incomplete,
                                           sizeof(incomplete) - 1u, NULL, CHTTP_HTTP_1_1,
                                           &response),
                SALTS_OK);
    check_equal(response.body, internal_response, sizeof(internal_response) - 1u);
    chttp_response_destroy(&response);

    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", duplicate_version,
                                           sizeof(duplicate_version) - 1u, NULL, CHTTP_HTTP_1_1,
                                           &response),
                SALTS_OK);
    check_equal(response.body, invalid_response, sizeof(invalid_response) - 1u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", wrong_version,
                                           sizeof(wrong_version) - 1u, NULL, CHTTP_HTTP_1_1,
                                           &response),
                SALTS_OK);
    check_equal(response.body, invalid_response, sizeof(invalid_response) - 1u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", reserved_method,
                                           sizeof(reserved_method) - 1u, NULL, CHTTP_HTTP_1_1,
                                           &response),
                SALTS_OK);
    check_equal(response.body, invalid_response, sizeof(invalid_response) - 1u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", null_id, sizeof(null_id) - 1u,
                                           NULL, CHTTP_HTTP_1_1, &response),
                SALTS_OK);
    check_equal(response.body, invalid_response, sizeof(invalid_response) - 1u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", scalar_params,
                                           sizeof(scalar_params) - 1u, NULL, CHTTP_HTTP_1_1,
                                           &response),
                SALTS_OK);
    check_equal(response.body, invalid_response, sizeof(invalid_response) - 1u);
    chttp_response_destroy(&response);

    check_equal(probe.calls, (size_t)6u);
    check_equal(probe.notifications, (size_t)3u);
    check_equal(chttp_client_destroy(&client, CRPC_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_server_stop(&server, CRPC_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_server_destroy(&server), SALTS_OK);
  }

  it("maps per-item batch response overflow without skipping later calls") {
    static const char batch[] = "[{\"jsonrpc\":\"2.0\",\"method\":\"large\",\"id\":1},"
                                "{\"jsonrpc\":\"2.0\",\"method\":\"large\",\"id\":2},"
                                "{\"jsonrpc\":\"2.0\",\"method\":\"large\"},"
                                "{\"jsonrpc\":\"2.0\",\"method\":\"large\",\"id\":3}]";
    static const char internal_responses[] =
        "[{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Internal "
        "error\"},\"id\":1},{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":"
        "\"Internal error\"},\"id\":2},{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,"
        "\"message\":\"Internal error\"},\"id\":3}]";
    crpc_server server = {0};
    chttp_client client = {0};
    crpc_server_config server_config = crpc_server_test_config(0);
    chttp_client_config client_config = crpc_server_test_http_client_config();
    crpc_server_test_probe probe = {0};
    crpc_method method = {.name = "large"};
    chttp_response response = {0};
    char uri[64];
    uint16_t port = 0u;

    server_config.http.max_response_body_bytes = 400u;
    server_config.http.max_buffered_response_body_bytes = 400u;
    check_equal(crpc_server_init(&server, &server_config), SALTS_OK);
    check_equal(
        crpc_server_register(&server, "/rpc", &method, crpc_server_test_large_result, &probe),
        SALTS_OK);
    check_equal(crpc_server_start(&server), SALTS_OK);
    check_equal(crpc_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", batch, sizeof(batch) - 1u, NULL,
                                           CHTTP_HTTP_1_1, &response),
                SALTS_OK);
    check_equal(response.status_code, 200u);
    check_equal(response.body, internal_responses, sizeof(internal_responses) - 1u);
    check_equal(probe.calls, (size_t)4u);
    check_equal(probe.notifications, (size_t)1u);
    chttp_response_destroy(&response);
    check_equal(chttp_client_destroy(&client, CRPC_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_server_stop(&server, CRPC_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_server_destroy(&server), SALTS_OK);
  }

  it("serves the same RPC endpoint over cleartext HTTP/2") {
    static const char call[] = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":1}";
    static const char reply[] = "{\"jsonrpc\":\"2.0\",\"result\":1,\"id\":1}";
    crpc_server server = {0};
    chttp_client client = {0};
    crpc_server_config server_config = crpc_server_test_config(1);
    chttp_client_config client_config = crpc_server_test_http_client_config();
    crpc_server_test_probe probe = {0};
    crpc_method ping = {.name = "ping"};
    chttp_response response = {0};
    char uri[64];
    uint16_t port = 0u;

    check_equal(crpc_server_init(&server, &server_config), SALTS_OK);
    check_equal(crpc_server_register(&server, "/rpc", &ping, crpc_server_test_ping, &probe),
                SALTS_OK);
    check_equal(crpc_server_start(&server), SALTS_OK);
    check_equal(crpc_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", call, sizeof(call) - 1u, NULL,
                                           CHTTP_HTTP_2, &response),
                SALTS_OK);
    check_equal(response.http_major, 2u);
    check_equal(response.body, reply, sizeof(reply) - 1u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", crpc_server_test_h2_batch,
                                           sizeof(crpc_server_test_h2_batch) - 1u, NULL,
                                           CHTTP_HTTP_2, &response),
                SALTS_OK);
    check_equal(response.http_major, 2u);
    check_equal(response.body, crpc_server_test_h2_batch_reply,
                sizeof(crpc_server_test_h2_batch_reply) - 1u);
    chttp_response_destroy(&response);
    check_equal(chttp_client_destroy(&client, CRPC_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_server_stop(&server, CRPC_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_server_destroy(&server), SALTS_OK);
  }

  it("serves the same RPC endpoint over TLS HTTP/1.1 and HTTP/2") {
    static const char *server_alpn[] = {"h2", "http/1.1"};
    static const char *h2[] = {"h2"};
    static const char *h1[] = {"http/1.1"};
    static const char call[] = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":1}";
    static const char reply[] = "{\"jsonrpc\":\"2.0\",\"result\":1,\"id\":1}";
    crpc_server server = {0};
    chttp_client client = {0};
    chttp_tls_profile h2_profile = {0};
    chttp_tls_profile h1_profile = {0};
    crpc_server_config server_config = crpc_server_test_config(1);
    chttp_client_config client_config = crpc_server_test_http_client_config();
    crpc_server_test_probe probe = {0};
    crpc_method ping = {.name = "ping"};
    cnet_tls_server_config server_tls;
    cnet_tls_client_config h2_tls;
    cnet_tls_client_config h1_tls;
    chttp_response response = {0};
    char *cert_path = tt_make_temp_file("crpc-server-cert", ".pem");
    char *key_path = tt_make_temp_file("crpc-server-key", ".pem");
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
    server_config.http.tls = &server_tls;

    check_equal(chttp_tls_profile_init(&h2_profile, &h2_tls), SALTS_OK);
    check_equal(chttp_tls_profile_init(&h1_profile, &h1_tls), SALTS_OK);
    check_equal(crpc_server_init(&server, &server_config), SALTS_OK);
    check_equal(crpc_server_register(&server, "/rpc", &ping, crpc_server_test_ping, &probe),
                SALTS_OK);
    check_equal(tt_remove_file(cert_path), 0);
    check_equal(tt_remove_file(key_path), 0);
    free(cert_path);
    free(key_path);
    check_equal(crpc_server_start(&server), SALTS_OK);
    check_equal(crpc_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", call, sizeof(call) - 1u,
                                           &h2_profile, CHTTP_HTTP_2, &response),
                SALTS_OK);
    check_equal(response.http_major, 2u);
    check_equal(response.body, reply, sizeof(reply) - 1u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", crpc_server_test_h2_batch,
                                           sizeof(crpc_server_test_h2_batch) - 1u, &h2_profile,
                                           CHTTP_HTTP_2, &response),
                SALTS_OK);
    check_equal(response.http_major, 2u);
    check_equal(response.body, crpc_server_test_h2_batch_reply,
                sizeof(crpc_server_test_h2_batch_reply) - 1u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", call, sizeof(call) - 1u,
                                           &h1_profile, CHTTP_HTTP_1_1, &response),
                SALTS_OK);
    check_equal(response.http_major, 1u);
    check_equal(response.body, reply, sizeof(reply) - 1u);
    chttp_response_destroy(&response);
    check_equal(crpc_server_test_http_post(&client, uri, "/rpc", crpc_server_test_h2_batch,
                                           sizeof(crpc_server_test_h2_batch) - 1u, &h1_profile,
                                           CHTTP_HTTP_1_1, &response),
                SALTS_OK);
    check_equal(response.http_major, 1u);
    check_equal(response.body, crpc_server_test_h2_batch_reply,
                sizeof(crpc_server_test_h2_batch_reply) - 1u);
    chttp_response_destroy(&response);
    check_equal(chttp_client_destroy(&client, CRPC_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&h1_profile), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&h2_profile), SALTS_OK);
    check_equal(crpc_server_stop(&server, CRPC_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_server_destroy(&server), SALTS_OK);
  }
}
