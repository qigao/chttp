#include "tinytest.h"
#include <chttp/chttp.h>

#include <salts/clock.h>

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET chttp_test_socket;
  #define CHTTP_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int chttp_test_socket;
  #define CHTTP_TEST_INVALID_SOCKET (-1)
#endif

enum { CHTTP_TEST_TIMEOUT_MS = 5000 };

typedef struct chttp_test_probe {
  int called;
  int status;
  unsigned int response_status;
  char reason[32];
  char content_type[64];
  unsigned char body[128];
  size_t body_size;
  int response_body_is_null;
} chttp_test_probe;

typedef struct chttp_test_source {
  const unsigned char *data;
  size_t size;
  size_t offset;
  size_t chunk_size;
  size_t calls;
} chttp_test_source;

static int chttp_test_source_read(void *user, void *buffer, size_t capacity, size_t *out_size) {
  chttp_test_source *source = (chttp_test_source *)user;
  size_t size;
  if (source == NULL || buffer == NULL || out_size == NULL || capacity == 0u) return SALTS_EINVAL;
  ++source->calls;
  if (source->offset == source->size) {
    *out_size = 0u;
    return SALTS_OK;
  }
  size = source->size - source->offset;
  if (size > source->chunk_size) size = source->chunk_size;
  if (size > capacity) size = capacity;
  memcpy(buffer, source->data + source->offset, size);
  source->offset += size;
  *out_size = size;
  return SALTS_OK;
}

static int chttp_test_response_sink(void *user, const void *data, size_t size) {
  chttp_test_probe *probe = (chttp_test_probe *)user;
  if (probe == NULL || (data == NULL && size != 0u) ||
      size > sizeof(probe->body) - probe->body_size)
    return SALTS_EMSGSIZE;
  memcpy(probe->body + probe->body_size, data, size);
  probe->body_size += size;
  return SALTS_OK;
}

static int chttp_test_server_handler(void *user, const chttp_server_request_view *request,
                                     chttp_server_response *response) {
  (void)user;
  if (request == NULL || response == NULL) return SALTS_EINVAL;
  return chttp_server_reply(response, 200u, "text/plain", "ok", 2u);
}

static void chttp_test_close_socket(chttp_test_socket socket_value) {
  if (socket_value == CHTTP_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int chttp_test_set_timeout(chttp_test_socket socket_value) {
#if defined(_WIN32)
  const DWORD timeout_ms = CHTTP_TEST_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                    (int)sizeof(timeout_ms)) == 0
             ? SALTS_OK
             : SALTS_EIO;
#else
  const struct timeval timeout = {CHTTP_TEST_TIMEOUT_MS / 1000,
                                  (CHTTP_TEST_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout)) ==
                 0
             ? SALTS_OK
             : SALTS_EIO;
#endif
}

static int chttp_test_listener(chttp_test_socket *out_listener, uint16_t *out_port) {
  struct sockaddr_in address;
#if defined(_WIN32)
  int length = (int)sizeof(address);
#else
  socklen_t length = (socklen_t)sizeof(address);
#endif
  if (out_listener == NULL || out_port == NULL) return SALTS_EINVAL;
  *out_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (*out_listener == CHTTP_TEST_INVALID_SOCKET) return SALTS_EIO;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(*out_listener, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      getsockname(*out_listener, (struct sockaddr *)&address, &length) != 0 ||
      listen(*out_listener, 1) != 0) {
    chttp_test_close_socket(*out_listener);
    *out_listener = CHTTP_TEST_INVALID_SOCKET;
    return SALTS_EIO;
  }
  *out_port = ntohs(address.sin_port);
  return SALTS_OK;
}

static int chttp_test_recv_all(chttp_test_socket socket_value, void *data, size_t size) {
  size_t offset = 0u;
  while (offset < size) {
    const int received = recv(socket_value, (char *)data + offset, (int)(size - offset), 0);
    if (received <= 0) return SALTS_EIO;
    offset += (size_t)received;
  }
  return SALTS_OK;
}

static int chttp_test_send_all(chttp_test_socket socket_value, const void *data, size_t size) {
  size_t offset = 0u;
  while (offset < size) {
    const int sent = send(socket_value, (const char *)data + offset, (int)(size - offset), 0);
    if (sent <= 0) return SALTS_EIO;
    offset += (size_t)sent;
  }
  return SALTS_OK;
}

static void chttp_test_complete(void *user, chttp_request request,
                                const chttp_response_view *response, const chttp_error *error) {
  chttp_test_probe *probe = (chttp_test_probe *)user;
  const char *content_type;
  (void)request;
  ++probe->called;
  if (error != NULL) {
    probe->status = error->status;
    return;
  }
  probe->status = SALTS_OK;
  probe->response_status = response->status_code;
  (void)snprintf(probe->reason, sizeof(probe->reason), "%s", response->reason);
  content_type = chttp_response_view_header(response, "Content-Type");
  if (content_type != NULL)
    (void)snprintf(probe->content_type, sizeof(probe->content_type), "%s", content_type);
  probe->body_size = response->body_size;
  probe->response_body_is_null = response->body == NULL;
  if (response->body != NULL && probe->body_size <= sizeof(probe->body))
    memcpy(probe->body, response->body, probe->body_size);
  else if (response->body != NULL) probe->status = SALTS_EMSGSIZE;
}

static chttp_client_config chttp_test_config(void) {
  const chttp_client_config config = {.network = {.backend =
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
                                                  .max_send_bytes = 1024u,
                                                  .receive_buffer_bytes = 32u,
                                                  .connect_timeout_ms = CHTTP_TEST_TIMEOUT_MS,
                                                  .read_timeout_ms = CHTTP_TEST_TIMEOUT_MS,
                                                  .write_timeout_ms = CHTTP_TEST_TIMEOUT_MS},
                                      .request_capacity = 2u,
                                      .max_start_line_bytes = 256u,
                                      .max_header_count = 16u,
                                      .max_header_bytes = 512u,
                                      .max_request_body_bytes = 128u,
                                      .max_response_body_bytes = 128u,
                                      .max_informational_responses = 4u};
  return config;
}

static int chttp_test_poll_until(chttp_async_client *client, chttp_test_probe *probe) {
  const uint64_t deadline = salts_monotonic_ms() + CHTTP_TEST_TIMEOUT_MS;
  while (probe->called == 0) {
    size_t completions = 0u;
    const int status = chttp_async_client_poll(client, 5u, &completions);
    if (status != SALTS_OK) return status;
    if (salts_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
  }
  return SALTS_OK;
}

spec("CHTTP advanced async client API") {
  it("exposes an explicit no-poll server lifecycle") {
    chttp_server server = {0};
    chttp_server_config config = {0};
    chttp_server_stats stats = {0};

    check_equal(chttp_server_init(NULL, &config), SALTS_EINVAL);
    check_equal(
        chttp_server_route(&server, CHTTP_METHOD_GET, "/health", chttp_test_server_handler, NULL),
        SALTS_EINVAL);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_EINVAL);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("round-trips one bounded HTTP response over CNet") {
    static const char response[] = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: 11\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "hello world";
    const chttp_header headers[] = {{"Accept", "text/plain"}};
    chttp_async_client client = {0};
    chttp_client_config config = chttp_test_config();
    chttp_test_probe probe = {0};
    chttp_test_socket listener = CHTTP_TEST_INVALID_SOCKET;
    chttp_test_socket accepted = CHTTP_TEST_INVALID_SOCKET;
    chttp_request request = {0};
    chttp_request_options options;
    char uri[64];
    char authority[64];
    char expected[512];
    unsigned char received[512];
    uint16_t port = 0u;
    int expected_size;
    size_t completions = 0u;

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/hello?x=1",
                                      .method = CHTTP_METHOD_GET,
                                      .headers = headers,
                                      .header_count = 1u,
                                      .on_complete = chttp_test_complete,
                                      .user = &probe};
    check_equal(chttp_async_client_submit(&client, &options, &request), SALTS_OK);
    check_true(request.slot != 0u && request.generation != 0u);
    check_equal(chttp_async_client_poll(&client, CHTTP_TEST_TIMEOUT_MS, &completions), SALTS_OK);
    check_equal(completions, (size_t)0u);
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != CHTTP_TEST_INVALID_SOCKET);
    check_equal(chttp_test_set_timeout(accepted), SALTS_OK);
    check_equal(chttp_async_client_poll(&client, 10u, &completions), SALTS_OK);

    expected_size = snprintf(expected, sizeof(expected),
                             "GET /hello?x=1 HTTP/1.1\r\n"
                             "Host: %s\r\n"
                             "Content-Length: 0\r\n"
                             "Connection: keep-alive\r\n"
                             "Accept: text/plain\r\n"
                             "\r\n",
                             authority);
    check_true(expected_size > 0 && (size_t)expected_size < sizeof(expected));
    check_equal(chttp_test_recv_all(accepted, received, (size_t)expected_size), SALTS_OK);
    check_equal(received, expected, (size_t)expected_size);

    check_equal(chttp_test_send_all(accepted, response, 19u), SALTS_OK);
    check_equal(chttp_async_client_poll(&client, 5u, &completions), SALTS_OK);
    check_equal(chttp_test_send_all(accepted, response + 19u, sizeof(response) - 1u - 19u),
                SALTS_OK);
    check_equal(chttp_test_poll_until(&client, &probe), SALTS_OK);
    check_equal(probe.called, 1);
    check_equal(probe.status, SALTS_OK);
    check_equal(probe.response_status, 200u);
    check_equal(probe.reason, "OK");
    check_equal(probe.content_type, "text/plain");
    check_equal(probe.body_size, (size_t)11u);
    check_equal(probe.body, "hello world", 11u);

    check_equal(chttp_async_client_stop(&client, CHTTP_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    chttp_test_close_socket(accepted);
    chttp_test_close_socket(listener);
  }

  it("streams an unknown-length H1 request and response without retaining response bytes") {
    static const char response[] = "HTTP/1.1 200 OK\r\n"
                                   "Content-Length: 5\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "world";
    static const char upload[] = "hello";
    static const char chunk1[] = "2\r\nhe\r\n";
    static const char chunk2[] = "2\r\nll\r\n";
    static const char chunk3[] = "1\r\no\r\n";
    static const char chunk_end[] = "0\r\n\r\n";
    chttp_async_client client = {0};
    chttp_client_config config = chttp_test_config();
    chttp_test_probe probe = {0};
    chttp_test_source source_state = {
        .data = (const unsigned char *)upload, .size = sizeof(upload) - 1u, .chunk_size = 2u};
    const chttp_body_source source = {.read = chttp_test_source_read, .user = &source_state};
    const chttp_body_sink sink = {.write = chttp_test_response_sink, .user = &probe};
    chttp_test_socket listener = CHTTP_TEST_INVALID_SOCKET;
    chttp_test_socket accepted = CHTTP_TEST_INVALID_SOCKET;
    chttp_request request = {0};
    chttp_request_options options;
    char uri[64];
    char authority[64];
    char expected_head[512];
    unsigned char received[512];
    uint16_t port = 0u;
    int expected_head_size;
    size_t completions = 0u;

    config.stream_chunk_bytes = 2u;
    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/stream",
                                      .method = CHTTP_METHOD_POST,
                                      .body_source = &source,
                                      .body_sink = &sink,
                                      .on_complete = chttp_test_complete,
                                      .user = &probe};
    check_equal(chttp_async_client_submit(&client, &options, &request), SALTS_OK);
    check_equal(chttp_async_client_poll(&client, CHTTP_TEST_TIMEOUT_MS, &completions), SALTS_OK);
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != CHTTP_TEST_INVALID_SOCKET);
    check_equal(chttp_test_set_timeout(accepted), SALTS_OK);
    check_equal(chttp_async_client_poll(&client, 10u, &completions), SALTS_OK);

    expected_head_size = snprintf(expected_head, sizeof(expected_head),
                                  "POST /stream HTTP/1.1\r\n"
                                  "Host: %s\r\n"
                                  "Transfer-Encoding: chunked\r\n"
                                  "Connection: keep-alive\r\n"
                                  "\r\n",
                                  authority);
    check_true(expected_head_size > 0 && (size_t)expected_head_size < sizeof(expected_head));
    check_equal(chttp_test_recv_all(accepted, received, (size_t)expected_head_size), SALTS_OK);
    check_equal(received, expected_head, (size_t)expected_head_size);

    check_equal(chttp_async_client_poll(&client, 10u, &completions), SALTS_OK);
    check_equal(chttp_test_recv_all(accepted, received, sizeof(chunk1) - 1u), SALTS_OK);
    check_equal(received, chunk1, sizeof(chunk1) - 1u);
    check_equal(chttp_async_client_poll(&client, 10u, &completions), SALTS_OK);
    check_equal(chttp_test_recv_all(accepted, received, sizeof(chunk2) - 1u), SALTS_OK);
    check_equal(received, chunk2, sizeof(chunk2) - 1u);
    check_equal(chttp_async_client_poll(&client, 10u, &completions), SALTS_OK);
    check_equal(chttp_test_recv_all(accepted, received, sizeof(chunk3) - 1u), SALTS_OK);
    check_equal(received, chunk3, sizeof(chunk3) - 1u);
    check_equal(chttp_async_client_poll(&client, 10u, &completions), SALTS_OK);
    check_equal(chttp_test_recv_all(accepted, received, sizeof(chunk_end) - 1u), SALTS_OK);
    check_equal(received, chunk_end, sizeof(chunk_end) - 1u);

    check_equal(chttp_test_send_all(accepted, response, sizeof(response) - 1u), SALTS_OK);
    check_equal(chttp_test_poll_until(&client, &probe), SALTS_OK);
    check_equal(probe.called, 1);
    check_equal(probe.status, SALTS_OK);
    check_equal(probe.response_status, 200u);
    check_equal(probe.response_body_is_null, 1);
    check_equal(probe.body_size, (size_t)5u);
    check_equal(probe.body, "world", 5u);
    check_equal(source_state.offset, (size_t)5u);
    check_equal(source_state.calls, (size_t)4u);

    check_equal(chttp_async_client_stop(&client, CHTTP_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    chttp_test_close_socket(accepted);
    chttp_test_close_socket(listener);
  }

  it("rejects datagram transport before admission") {
    chttp_async_client client = {0};
    chttp_client_config config = chttp_test_config();
    chttp_test_probe probe = {0};
    chttp_request request = {7u, 9u};
    const chttp_request_options options = {.connection_uri = "udp://127.0.0.1:9000",
                                           .authority = "127.0.0.1:9000",
                                           .target = "/",
                                           .method = CHTTP_METHOD_GET,
                                           .on_complete = chttp_test_complete,
                                           .user = &probe};

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_async_client_submit(&client, &options, &request), SALTS_ENOTSUP);
    check_equal(request.slot, 0u);
    check_equal(request.generation, 0u);
    check_equal(probe.called, 0);
    check_equal(chttp_async_client_stop(&client, CHTTP_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
  }

  it("keeps a canceled request until its terminal callback") {
    chttp_async_client client = {0};
    chttp_client_config config = chttp_test_config();
    chttp_test_probe probe = {0};
    chttp_test_socket listener = CHTTP_TEST_INVALID_SOCKET;
    chttp_request request = {0};
    chttp_request_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/cancel",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_test_complete,
                                      .user = &probe};
    check_equal(chttp_async_client_submit(&client, &options, &request), SALTS_OK);
    check_equal(chttp_async_request_cancel(&client, request), SALTS_OK);
    check_equal(chttp_async_request_cancel(&client, request), SALTS_EALREADY);
    check_equal(chttp_test_poll_until(&client, &probe), SALTS_OK);
    check_equal(probe.called, 1);
    check_equal(probe.status, SALTS_ECANCELED);
    check_equal(chttp_async_request_cancel(&client, request), SALTS_ENOENT);
    check_equal(chttp_async_client_stop(&client, CHTTP_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    chttp_test_close_socket(listener);
  }
}
