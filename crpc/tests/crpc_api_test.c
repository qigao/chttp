#include "tinytest.h"
#include <crpc/crpc.h>

#include <salts/clock.h>
#include <salts/thread.h>

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET crpc_test_socket;
  #define CRPC_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int crpc_test_socket;
  #define CRPC_TEST_INVALID_SOCKET (-1)
#endif

enum { CRPC_TEST_TIMEOUT_MS = 5000, CRPC_TEST_LISTEN_BACKLOG = 4 };

typed_any(idempotent, int, crpc_test_identity, (int value)) { return value; }

typedef struct crpc_test_probe {
  int called;
  int status;
  int64_t result;
  uint64_t request_id;
  unsigned int http_status;
  cmeta_sig callable_signature;
  cmeta_effects callable_effects;
  cmeta_properties callable_properties;
  size_t callable_param_count;
  cmeta_fn_protocol callable_protocol;
  char stage[32];
} crpc_test_probe;

typedef struct crpc_test_reentrant_probe {
  crpc_async_client *client;
  crpc_request other_request;
  crpc_options nested_options;
  crpc_complete_fn nested_complete;
  void *nested_user;
  int called;
  int status;
  int nested_call_status;
  int nested_poll_status;
  int nested_stop_status;
  int nested_destroy_status;
  int own_cancel_status;
  int other_cancel_status;
  size_t nested_completions;
} crpc_test_reentrant_probe;

typedef struct crpc_test_server {
  crpc_test_socket listener;
  const void *const *expected;
  const size_t *expected_sizes;
  const void *const *responses;
  const size_t *response_sizes;
  size_t exchange_count;
  int reuse_connection;
  int status;
} crpc_test_server;

static void crpc_test_close_socket(crpc_test_socket socket_value) {
  if (socket_value == CRPC_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int crpc_test_set_timeout(crpc_test_socket socket_value) {
#if defined(_WIN32)
  const DWORD timeout_ms = CRPC_TEST_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                    (int)sizeof(timeout_ms)) == 0
             ? SALTS_OK
             : SALTS_EIO;
#else
  const struct timeval timeout = {CRPC_TEST_TIMEOUT_MS / 1000,
                                  (CRPC_TEST_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout)) ==
                 0
             ? SALTS_OK
             : SALTS_EIO;
#endif
}

static int crpc_test_listener(crpc_test_socket *out_listener, uint16_t *out_port) {
  struct sockaddr_in address;
#if defined(_WIN32)
  int length = (int)sizeof(address);
#else
  socklen_t length = (socklen_t)sizeof(address);
#endif
  if (out_listener == NULL || out_port == NULL) return SALTS_EINVAL;
  *out_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (*out_listener == CRPC_TEST_INVALID_SOCKET) return SALTS_EIO;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(*out_listener, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      getsockname(*out_listener, (struct sockaddr *)&address, &length) != 0 ||
      listen(*out_listener, CRPC_TEST_LISTEN_BACKLOG) != 0) {
    crpc_test_close_socket(*out_listener);
    *out_listener = CRPC_TEST_INVALID_SOCKET;
    return SALTS_EIO;
  }
  *out_port = ntohs(address.sin_port);
  return SALTS_OK;
}

static int crpc_test_recv_all(crpc_test_socket socket_value, void *data, size_t size) {
  size_t offset = 0u;
  while (offset < size) {
    const int received = recv(socket_value, (char *)data + offset, (int)(size - offset), 0);
    if (received <= 0) return SALTS_EIO;
    offset += (size_t)received;
  }
  return SALTS_OK;
}

static int crpc_test_send_all(crpc_test_socket socket_value, const void *data, size_t size) {
  size_t offset = 0u;
  while (offset < size) {
    const int sent = send(socket_value, (const char *)data + offset, (int)(size - offset), 0);
    if (sent <= 0) return SALTS_EIO;
    offset += (size_t)sent;
  }
  return SALTS_OK;
}

static void crpc_test_serve(void *user) {
  crpc_test_server *server = (crpc_test_server *)user;
  crpc_test_socket accepted = CRPC_TEST_INVALID_SOCKET;
  unsigned char received[2048];
  size_t index;
  if (server == NULL) return;
  if (server->reuse_connection) {
    accepted = accept(server->listener, NULL, NULL);
    if (accepted == CRPC_TEST_INVALID_SOCKET) {
      server->status = SALTS_EIO;
      return;
    }
    server->status = crpc_test_set_timeout(accepted);
  }
  for (index = 0u; index < server->exchange_count; ++index) {
    if (!server->reuse_connection) {
      accepted = accept(server->listener, NULL, NULL);
      if (accepted == CRPC_TEST_INVALID_SOCKET) {
        server->status = SALTS_EIO;
        return;
      }
      server->status = crpc_test_set_timeout(accepted);
    }
    if (server->status == SALTS_OK && server->expected_sizes[index] > sizeof(received))
      server->status = SALTS_EMSGSIZE;
    if (server->status == SALTS_OK)
      server->status = crpc_test_recv_all(accepted, received, server->expected_sizes[index]);
    if (server->status == SALTS_OK &&
        memcmp(received, server->expected[index], server->expected_sizes[index]) != 0)
      server->status = SALTS_EPROTO;
    if (server->status == SALTS_OK)
      server->status =
          crpc_test_send_all(accepted, server->responses[index], server->response_sizes[index]);
    if (!server->reuse_connection) {
      crpc_test_close_socket(accepted);
      accepted = CRPC_TEST_INVALID_SOCKET;
    }
    if (server->status != SALTS_OK) break;
  }
  crpc_test_close_socket(accepted);
}

static cserde_status crpc_test_encode_params(void *user, cserde_writer *writer) {
  cserde_token token = {.kind = CSERDE_ARRAY_BEGIN};
  cserde_status status;
  (void)user;
  status = cserde_writer_write(writer, &token);
  token = (cserde_token){.kind = CSERDE_UINT, .value.uint = UINT64_C(7)};
  if (status == CSERDE_OK) status = cserde_writer_write(writer, &token);
  token = (cserde_token){.kind = CSERDE_ARRAY_END};
  if (status == CSERDE_OK) status = cserde_writer_write(writer, &token);
  return status;
}

static void crpc_test_complete(void *user, crpc_request request, const crpc_response_view *response,
                               const crpc_error *error) {
  crpc_test_probe *probe = (crpc_test_probe *)user;
  cserde_token token = {0};
  (void)request;
  ++probe->called;
  if (error != NULL) {
    probe->status = error->status;
    if (error->stage != NULL)
      (void)snprintf(probe->stage, sizeof(probe->stage), "%s", error->stage);
    return;
  }
  if (response == NULL || response->kind != CRPC_RESPONSE_RESULT ||
      cserde_reader_next(response->value.result, &token) != CSERDE_OK ||
      token.kind != CSERDE_UINT || token.value.uint > (uint64_t)INT64_MAX) {
    probe->status = SALTS_EPROTO;
    return;
  }
  probe->status = SALTS_OK;
  probe->result = (int64_t)token.value.uint;
  probe->request_id = response->request_id;
  probe->http_status = response->http_status;
  if (response->callable != NULL) {
    const cmeta_sig_desc *signature = cmeta_callable_signature(*response->callable);
    if (signature == NULL) {
      probe->status = SALTS_EPROTO;
      return;
    }
    probe->callable_signature = signature->sig;
    probe->callable_effects = response->callable->meta.effects;
    probe->callable_properties = response->callable->meta.properties;
    probe->callable_param_count = signature->param_count;
    probe->callable_protocol = signature->protocol;
  }
}

static void crpc_test_reentrant_complete(void *user, crpc_request request,
                                         const crpc_response_view *response,
                                         const crpc_error *error) {
  crpc_test_reentrant_probe *probe = (crpc_test_reentrant_probe *)user;
  crpc_request nested_request = {7u, 9u};
  (void)response;
  ++probe->called;
  probe->status = error != NULL ? error->status : SALTS_OK;
  probe->nested_call_status =
      crpc_async_client_submit(probe->client, &probe->nested_options, probe->nested_complete,
                               probe->nested_user, &nested_request);
  probe->nested_poll_status = crpc_async_client_poll(probe->client, 0u, &probe->nested_completions);
  probe->nested_stop_status = crpc_async_client_stop(probe->client, 0u);
  probe->nested_destroy_status = crpc_async_client_destroy(probe->client);
  probe->own_cancel_status = crpc_async_request_cancel(probe->client, request);
  probe->other_cancel_status = crpc_async_request_cancel(probe->client, probe->other_request);
}

static crpc_client_config crpc_test_config(void) {
  const crpc_client_config config = {
      .http = {.network = {.backend =
#if defined(_WIN32)
                               NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
                               NATIVE_IO_BACKEND_EPOLL,
#else
                               NATIVE_IO_BACKEND_KQUEUE,
#endif
                           .connection_capacity = 2u,
                           .command_capacity = 8u,
                           .request_capacity = 4u,
                           .completion_batch_capacity = 4u,
                           .event_capacity = 8u,
                           .max_send_bytes = 2048u,
                           .receive_buffer_bytes = 32u,
                           .connect_timeout_ms = CRPC_TEST_TIMEOUT_MS,
                           .read_timeout_ms = CRPC_TEST_TIMEOUT_MS,
                           .write_timeout_ms = CRPC_TEST_TIMEOUT_MS},
               .request_capacity = 2u,
               .max_start_line_bytes = 256u,
               .max_header_count = 16u,
               .max_header_bytes = 1024u,
               .max_request_body_bytes = 512u,
               .max_response_body_bytes = 512u,
               .max_informational_responses = 2u},
      .request_capacity = 2u,
      .max_method_bytes = 64u,
      .max_json_depth = 8u};
  return config;
}

static int crpc_test_poll_until(crpc_async_client *client, crpc_test_probe *probe) {
  const uint64_t deadline = salts_monotonic_ms() + CRPC_TEST_TIMEOUT_MS;
  while (probe->called == 0) {
    size_t completions = 0u;
    const int status = crpc_async_client_poll(client, 5u, &completions);
    if (status != SALTS_OK) return status;
    if (salts_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
  }
  return SALTS_OK;
}

static int crpc_test_poll_until_both(crpc_async_client *client,
                                     const crpc_test_reentrant_probe *first,
                                     const crpc_test_probe *second) {
  const uint64_t deadline = salts_monotonic_ms() + CRPC_TEST_TIMEOUT_MS;
  while (first->called == 0 || second->called == 0) {
    size_t completions = 0u;
    const int status = crpc_async_client_poll(client, 5u, &completions);
    if (status != SALTS_OK) return status;
    if (salts_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
  }
  return SALTS_OK;
}

spec("CRPC blocking request/reply client") {
  it("calls multiple endpoints at one site without caller polling") {
    static const char first_body[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"math.double\",\"params\":[7],\"id\":9}";
    static const char second_body[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"system.status\",\"id\":10}";
    static const char first_response_body[] = "{\"jsonrpc\":\"2.0\",\"result\":14,\"id\":9}";
    static const char second_response_body[] =
        "{\"jsonrpc\":\"2.0\",\"result\":\"ready\",\"id\":10}";
    crpc_client client = {0};
    crpc_client_config config = crpc_test_config();
    crpc_test_socket listener = CRPC_TEST_INVALID_SOCKET;
    crpc_test_server server = {0};
    salts_thread_t thread = NULL;
    crpc_response response = {0};
    crpc_error error = {0};
    crpc_options options;
    cserde_token token = {0};
    char uri[64];
    char authority[64];
    char first_expected[1024];
    char second_expected[1024];
    char first_wire_response[512];
    char second_wire_response[512];
    const void *expected[2];
    size_t expected_sizes[2];
    const void *responses[2];
    size_t response_sizes[2];
    uint16_t port = 0u;
    int size;

    check_equal(crpc_client_init(&client, &config), SALTS_OK);
    check_equal(crpc_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    size = snprintf(first_expected, sizeof(first_expected),
                    "POST /rpc/math HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: keep-alive\r\n"
                    "Content-Type: application/json\r\n"
                    "Accept: application/json\r\n"
                    "\r\n"
                    "%s",
                    authority, sizeof(first_body) - 1u, first_body);
    check_true(size > 0 && (size_t)size < sizeof(first_expected));
    expected[0] = first_expected;
    expected_sizes[0] = (size_t)size;
    size = snprintf(second_expected, sizeof(second_expected),
                    "POST /rpc/status HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: keep-alive\r\n"
                    "Content-Type: application/json\r\n"
                    "Accept: application/json\r\n"
                    "\r\n"
                    "%s",
                    authority, sizeof(second_body) - 1u, second_body);
    check_true(size > 0 && (size_t)size < sizeof(second_expected));
    expected[1] = second_expected;
    expected_sizes[1] = (size_t)size;

    size = snprintf(first_wire_response, sizeof(first_wire_response),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"
                    "%s",
                    sizeof(first_response_body) - 1u, first_response_body);
    check_true(size > 0 && (size_t)size < sizeof(first_wire_response));
    responses[0] = first_wire_response;
    response_sizes[0] = (size_t)size;
    size = snprintf(second_wire_response, sizeof(second_wire_response),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "%s",
                    sizeof(second_response_body) - 1u, second_response_body);
    check_true(size > 0 && (size_t)size < sizeof(second_wire_response));
    responses[1] = second_wire_response;
    response_sizes[1] = (size_t)size;

    server = (crpc_test_server){.listener = listener,
                                .expected = expected,
                                .expected_sizes = expected_sizes,
                                .responses = responses,
                                .response_sizes = response_sizes,
                                .exchange_count = 2u,
                                .reuse_connection = 1};
    check_equal(salts_thread_create(&thread, crpc_test_serve, &server), SALTS_OK);

    options = (crpc_options){
        .connection_uri = uri,
        .authority = authority,
        .target = "/rpc/math",
        .method = {.service = "math", .name = "double", .callable = &crpc_test_identity},
        .request_id = UINT64_C(9),
        .encode_params = crpc_test_encode_params,
        .deadline_ms = CRPC_TEST_TIMEOUT_MS};
    check_equal(crpc_request_reply(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.kind, CRPC_RESPONSE_RESULT);
    check_equal(response.request_id, UINT64_C(9));
    check_not_null(response.callable);
    check_equal(cserde_reader_next(response.value.result, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_UINT);
    check_equal(token.value.uint, UINT64_C(14));
    crpc_response_destroy(&response);

    options = (crpc_options){.connection_uri = uri,
                             .authority = authority,
                             .target = "/rpc/status",
                             .method = {.service = "system", .name = "status"},
                             .request_id = UINT64_C(10),
                             .deadline_ms = CRPC_TEST_TIMEOUT_MS};
    check_equal(crpc_request_reply(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.kind, CRPC_RESPONSE_RESULT);
    check_equal(response.request_id, UINT64_C(10));
    check_equal(cserde_reader_next(response.value.result, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_STRING);
    check_equal(token.value.slice.size, (size_t)5u);
    check_equal(token.value.slice.data, "ready", 5u);
    crpc_response_destroy(&response);

    check_equal(crpc_client_destroy(&client, CRPC_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    crpc_test_close_socket(listener);
  }
}

spec("CRPC advanced async client API") {
  it("round-trips one typed JSON-RPC result over CHTTP and CNet") {
    static const char request_body[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"math.double\",\"params\":[7],\"id\":9}";
    static const char response_body[] = "{\"jsonrpc\":\"2.0\",\"result\":14,\"id\":9}";
    crpc_async_client client = {0};
    crpc_client_config config = crpc_test_config();
    crpc_test_probe probe = {0};
    crpc_test_socket listener = CRPC_TEST_INVALID_SOCKET;
    crpc_test_socket accepted = CRPC_TEST_INVALID_SOCKET;
    crpc_request request = {0};
    crpc_options options;
    char uri[64];
    char authority[64];
    char expected[1024];
    char response[512];
    unsigned char received[1024];
    uint16_t port = 0u;
    size_t completions = 0u;
    int expected_size;
    int response_size;

    check_equal(crpc_async_client_init(&client, &config), SALTS_OK);
    check_equal(crpc_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    options = (crpc_options){
        .connection_uri = uri,
        .authority = authority,
        .target = "/rpc",
        .method = {.service = "math", .name = "double", .callable = &crpc_test_identity},
        .request_id = UINT64_C(9),
        .encode_params = crpc_test_encode_params};
    check_equal(crpc_async_client_submit(&client, &options, crpc_test_complete, &probe, &request),
                SALTS_OK);
    check_true(request.slot != 0u && request.generation != 0u);
    check_equal(crpc_async_client_poll(&client, CRPC_TEST_TIMEOUT_MS, &completions), SALTS_OK);
    check_equal(completions, (size_t)0u);
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != CRPC_TEST_INVALID_SOCKET);
    check_equal(crpc_test_set_timeout(accepted), SALTS_OK);
    check_equal(crpc_async_client_poll(&client, 10u, &completions), SALTS_OK);

    expected_size = snprintf(expected, sizeof(expected),
                             "POST /rpc HTTP/1.1\r\n"
                             "Host: %s\r\n"
                             "Content-Length: %zu\r\n"
                             "Connection: keep-alive\r\n"
                             "Content-Type: application/json\r\n"
                             "Accept: application/json\r\n"
                             "\r\n"
                             "%s",
                             authority, sizeof(request_body) - 1u, request_body);
    check_true(expected_size > 0 && (size_t)expected_size < sizeof(expected));
    check_equal(crpc_test_recv_all(accepted, received, (size_t)expected_size), SALTS_OK);
    check_equal(received, expected, (size_t)expected_size);

    response_size = snprintf(response, sizeof(response),
                             "HTTP/1.1 200 OK\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: %zu\r\n"
                             "Connection: close\r\n"
                             "\r\n"
                             "%s",
                             sizeof(response_body) - 1u, response_body);
    check_true(response_size > 0 && (size_t)response_size < sizeof(response));
    check_equal(crpc_test_send_all(accepted, response, (size_t)response_size), SALTS_OK);
    check_equal(crpc_test_poll_until(&client, &probe), SALTS_OK);
    check_equal(probe.called, 1);
    check_equal(probe.status, SALTS_OK);
    check_equal(probe.result, (int64_t)14);
    check_equal(probe.request_id, UINT64_C(9));
    check_equal(probe.http_status, 200u);
    check_true(probe.callable_signature != CMETA_SIG_INVALID);
    check_equal(probe.callable_param_count, (size_t)1u);
    check_equal(probe.callable_protocol, CMETA_FN_PROTOCOL_VALUE);
    check_true(cmeta_effects_are_pure(probe.callable_effects));
    check_true(cmeta_properties_include(probe.callable_properties, CMETA_PROP_IDEMPOTENT));
    check_equal(crpc_async_request_cancel(&client, request), SALTS_ENOENT);

    check_equal(crpc_async_client_stop(&client, CRPC_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_async_client_destroy(&client), SALTS_OK);
    crpc_test_close_socket(accepted);
    crpc_test_close_socket(listener);
  }

  it("owns an overall deadline separately from socket read timeout") {
    crpc_async_client client = {0};
    crpc_client_config config = crpc_test_config();
    crpc_test_probe probe = {0};
    crpc_test_socket listener = CRPC_TEST_INVALID_SOCKET;
    crpc_request request = {0};
    crpc_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(crpc_async_client_init(&client, &config), SALTS_OK);
    check_equal(crpc_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    options = (crpc_options){.connection_uri = uri,
                             .authority = authority,
                             .target = "/rpc",
                             .method = {.name = "wait"},
                             .request_id = UINT64_C(11),
                             .deadline_ms = 10u};
    check_equal(crpc_async_client_submit(&client, &options, crpc_test_complete, &probe, &request),
                SALTS_OK);
    check_equal(crpc_test_poll_until(&client, &probe), SALTS_OK);
    check_equal(probe.called, 1);
    check_equal(probe.status, SALTS_ETIMEDOUT);
    check_equal(probe.stage, "rpc-deadline");
    check_equal(crpc_async_request_cancel(&client, request), SALTS_ENOENT);

    check_equal(crpc_async_client_stop(&client, CRPC_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_async_client_destroy(&client), SALTS_OK);
    crpc_test_close_socket(listener);
  }

  it("rejects a duplicate active JSON-RPC request id") {
    crpc_async_client client = {0};
    crpc_client_config config = crpc_test_config();
    crpc_test_probe probe = {0};
    crpc_test_socket listener = CRPC_TEST_INVALID_SOCKET;
    crpc_request first = {0};
    crpc_request duplicate = {7u, 9u};
    crpc_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(crpc_async_client_init(&client, &config), SALTS_OK);
    check_equal(crpc_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    options = (crpc_options){.connection_uri = uri,
                             .authority = authority,
                             .target = "/rpc",
                             .method = {.name = "duplicate"},
                             .request_id = UINT64_C(23)};
    check_equal(crpc_async_client_submit(&client, &options, crpc_test_complete, &probe, &first),
                SALTS_OK);
    check_equal(crpc_async_client_submit(&client, &options, crpc_test_complete, &probe, &duplicate),
                SALTS_EALREADY);
    check_equal(duplicate.slot, 0u);
    check_equal(duplicate.generation, 0u);
    check_equal(probe.called, 0);
    check_equal(crpc_async_request_cancel(&client, first), SALTS_OK);
    check_equal(crpc_test_poll_until(&client, &probe), SALTS_OK);
    check_equal(crpc_async_client_stop(&client, CRPC_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_async_client_destroy(&client), SALTS_OK);
    crpc_test_close_socket(listener);
  }

  it("delivers manual cancellation exactly once") {
    crpc_async_client client = {0};
    crpc_client_config config = crpc_test_config();
    crpc_test_probe probe = {0};
    crpc_test_socket listener = CRPC_TEST_INVALID_SOCKET;
    crpc_request request = {0};
    crpc_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(crpc_async_client_init(&client, &config), SALTS_OK);
    check_equal(crpc_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    options = (crpc_options){.connection_uri = uri,
                             .authority = authority,
                             .target = "/rpc",
                             .method = {.name = "cancel"},
                             .request_id = UINT64_C(29)};
    check_equal(crpc_async_client_submit(&client, &options, crpc_test_complete, &probe, &request),
                SALTS_OK);
    check_equal(crpc_async_request_cancel(&client, request), SALTS_OK);
    check_equal(crpc_async_request_cancel(&client, request), SALTS_EALREADY);
    check_equal(crpc_test_poll_until(&client, &probe), SALTS_OK);
    check_equal(probe.called, 1);
    check_equal(probe.status, SALTS_ECANCELED);
    check_equal(probe.stage, "cancel");
    check_equal(crpc_async_request_cancel(&client, request), SALTS_ENOENT);
    check_equal(crpc_async_client_stop(&client, CRPC_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_async_client_destroy(&client), SALTS_OK);
    crpc_test_close_socket(listener);
  }

  it("enforces callback reentrancy while allowing cancellation of another request") {
    crpc_async_client client = {0};
    crpc_client_config config = crpc_test_config();
    crpc_test_probe other_probe = {0};
    crpc_test_probe nested_probe = {0};
    crpc_test_reentrant_probe probe = {.client = &client};
    crpc_test_socket listener = CRPC_TEST_INVALID_SOCKET;
    crpc_request first = {0};
    crpc_request other = {0};
    crpc_options first_options;
    crpc_options other_options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(crpc_async_client_init(&client, &config), SALTS_OK);
    check_equal(crpc_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    other_options = (crpc_options){.connection_uri = uri,
                                   .authority = authority,
                                   .target = "/rpc",
                                   .method = {.name = "other"},
                                   .request_id = UINT64_C(31)};
    first_options = (crpc_options){.connection_uri = uri,
                                   .authority = authority,
                                   .target = "/rpc",
                                   .method = {.name = "first"},
                                   .request_id = UINT64_C(37)};
    probe.nested_options = (crpc_options){.connection_uri = uri,
                                          .authority = authority,
                                          .target = "/rpc",
                                          .method = {.name = "nested"},
                                          .request_id = UINT64_C(41)};
    probe.nested_complete = crpc_test_complete;
    probe.nested_user = &nested_probe;
    check_equal(
        crpc_async_client_submit(&client, &other_options, crpc_test_complete, &other_probe, &other),
        SALTS_OK);
    probe.other_request = other;
    check_equal(crpc_async_client_submit(&client, &first_options, crpc_test_reentrant_complete,
                                         &probe, &first),
                SALTS_OK);
    check_equal(crpc_async_request_cancel(&client, first), SALTS_OK);
    check_equal(crpc_test_poll_until_both(&client, &probe, &other_probe), SALTS_OK);
    check_equal(probe.called, 1);
    check_equal(probe.status, SALTS_ECANCELED);
    check_equal(probe.nested_call_status, SALTS_EBUSY);
    check_equal(probe.nested_poll_status, SALTS_EBUSY);
    check_equal(probe.nested_completions, (size_t)0u);
    check_equal(probe.nested_stop_status, SALTS_EBUSY);
    check_equal(probe.nested_destroy_status, SALTS_EBUSY);
    check_equal(probe.own_cancel_status, SALTS_EALREADY);
    check_equal(probe.other_cancel_status, SALTS_OK);
    check_equal(other_probe.called, 1);
    check_equal(other_probe.status, SALTS_ECANCELED);
    check_equal(nested_probe.called, 0);
    check_equal(crpc_async_client_stop(&client, CRPC_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(crpc_async_client_destroy(&client), SALTS_OK);
    crpc_test_close_socket(listener);
  }
}
