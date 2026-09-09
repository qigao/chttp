#include "chttp_h2_frame.h"
#include "chttp_h2_proto.h"
#include "chttp_server_runtime.h"
#include "chttp_tls_test_material.h"
#include "tinytest.h"

#include <chttp/chttp.h>
#include <salts/clock.h>
#include <salts/thread.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET chttp_h2_server_test_socket;
  #define CHTTP_H2_SERVER_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int chttp_h2_server_test_socket;
  #define CHTTP_H2_SERVER_TEST_INVALID_SOCKET (-1)
#endif

enum { CHTTP_H2_SERVER_TEST_TIMEOUT_MS = 5000 };

typedef struct chttp_h2_server_test_probe {
  size_t middleware_calls;
  size_t handler_calls;
} chttp_h2_server_test_probe;

typedef struct chttp_h2_server_test_completion {
  size_t calls;
  int status;
  unsigned int response_status;
  char body[32];
  size_t body_size;
} chttp_h2_server_test_completion;

typedef struct chttp_h2_server_test_peer_result {
  int32_t stream_id;
  unsigned int status;
  uint32_t error_code;
  char body[32];
  size_t body_size;
  size_t received_body_size;
  bool closed;
} chttp_h2_server_test_peer_result;

typedef struct chttp_h2_server_test_peer {
  chttp_h2_proto *protocol;
  chttp_h2_server_test_peer_result results[4];
  size_t result_count;
  size_t close_count;
  size_t goaway_count;
  uint32_t goaway_error;
} chttp_h2_server_test_peer;

typedef struct chttp_h2_server_test_payload {
  const void *data;
  size_t size;
  atomic_int *handler_called;
} chttp_h2_server_test_payload;

typedef struct chttp_h2_server_test_deferred {
  chttp_server_deferred handle;
  atomic_int admitted;
  int defer_status;
} chttp_h2_server_test_deferred;

typedef struct chttp_h2_server_test_stop {
  chttp_server *server;
  int status;
} chttp_h2_server_test_stop;

typedef struct chttp_h2_server_test_source {
  size_t calls;
} chttp_h2_server_test_source;

typedef struct chttp_h2_stream_probe {
  const unsigned char *source_data;
  size_t source_size;
  size_t source_offset;
  size_t source_chunk;
  unsigned char sink_data[64];
  size_t sink_size;
  size_t source_calls;
  size_t sink_calls;
} chttp_h2_stream_probe;

typedef struct chttp_h2_stream_completion {
  size_t calls;
  int status;
  unsigned int response_status;
  unsigned char body[32];
  size_t body_size;
} chttp_h2_stream_completion;

typedef struct chttp_h2_server_stream_probe {
  unsigned char data[64];
  size_t size;
  size_t writes;
  size_t opens;
  size_t closes;
  size_t handler_calls;
  size_t response_offset;
  size_t response_reads;
  int close_status;
} chttp_h2_server_stream_probe;

typedef struct chttp_h2_websocket_probe {
  atomic_int opens;
  atomic_int messages;
} chttp_h2_websocket_probe;

static int chttp_h2_websocket_open(void *user, chttp_websocket *websocket,
                                   const chttp_server_request_view *request,
                                   chttp_server_response *response) {
  chttp_h2_websocket_probe *probe = (chttp_h2_websocket_probe *)user;
  const char *id;
  (void)websocket;
  (void)response;
  if (probe == NULL || request == NULL) return SALTS_EPROTO;
  id = chttp_server_request_param(request, "id");
  if (request->http_major != 2u || strcmp(request->path, "/ws/42") != 0 || id == NULL ||
      strcmp(id, "42") != 0)
    return SALTS_EPROTO;
  atomic_fetch_add_explicit(&probe->opens, 1, memory_order_release);
  return SALTS_OK;
}

static int chttp_h2_websocket_jwt_open(void *user, chttp_websocket *websocket,
                                       const chttp_server_request_view *request,
                                       chttp_server_response *response) {
  chttp_h2_websocket_probe *probe = (chttp_h2_websocket_probe *)user;
  const char *id;
  (void)websocket;
  (void)response;
  if (probe == NULL || request == NULL || request->jwt_claims == NULL ||
      request->jwt_claims->subject == NULL || strcmp(request->jwt_claims->subject, "alice") != 0)
    return SALTS_EPROTO;
  id = chttp_server_request_param(request, "id");
  if (request->http_major != 2u || strcmp(request->path, "/jwt-ws/42") != 0 || id == NULL ||
      strcmp(id, "42") != 0)
    return SALTS_EPROTO;
  atomic_fetch_add_explicit(&probe->opens, 1, memory_order_release);
  return SALTS_OK;
}

static void chttp_h2_websocket_event(void *user, chttp_websocket *websocket,
                                     const chttp_websocket_event *event) {
  chttp_h2_websocket_probe *probe = (chttp_h2_websocket_probe *)user;
  if (probe == NULL || websocket == NULL || event == NULL ||
      event->kind != CHTTP_WEBSOCKET_EVENT_MESSAGE)
    return;
  atomic_fetch_add_explicit(&probe->messages, 1, memory_order_release);
  (void)chttp_websocket_send_text(websocket, event->data, event->size);
}

static int chttp_h2_stream_read(void *user, void *buffer, size_t capacity, size_t *out_size) {
  chttp_h2_stream_probe *probe = (chttp_h2_stream_probe *)user;
  size_t size;
  if (probe == NULL || buffer == NULL || out_size == NULL || capacity == 0u) return SALTS_EINVAL;
  ++probe->source_calls;
  if (probe->source_offset == probe->source_size) {
    *out_size = 0u;
    return SALTS_OK;
  }
  size = probe->source_size - probe->source_offset;
  if (size > probe->source_chunk) size = probe->source_chunk;
  if (size > capacity) size = capacity;
  memcpy(buffer, probe->source_data + probe->source_offset, size);
  probe->source_offset += size;
  *out_size = size;
  return SALTS_OK;
}

static int chttp_h2_stream_write(void *user, const void *data, size_t size) {
  chttp_h2_stream_probe *probe = (chttp_h2_stream_probe *)user;
  if (probe == NULL || (data == NULL && size != 0u) ||
      size > sizeof(probe->sink_data) - probe->sink_size)
    return SALTS_EMSGSIZE;
  ++probe->sink_calls;
  memcpy(probe->sink_data + probe->sink_size, data, size);
  probe->sink_size += size;
  return SALTS_OK;
}

static int chttp_h2_stream_fail_write(void *user, const void *data, size_t size) {
  (void)user;
  (void)data;
  (void)size;
  return SALTS_EIO;
}

static int chttp_h2_server_stream_write(void *user, const void *data, size_t size) {
  chttp_h2_server_stream_probe *probe = (chttp_h2_server_stream_probe *)user;
  if (probe == NULL || (data == NULL && size != 0u) || size > sizeof(probe->data) - probe->size)
    return SALTS_EMSGSIZE;
  ++probe->writes;
  memcpy(probe->data + probe->size, data, size);
  probe->size += size;
  return SALTS_OK;
}

static int chttp_h2_server_stream_open(void *user, const chttp_server_request_view *request,
                                       chttp_body_sink *out_sink) {
  chttp_h2_server_stream_probe *probe = (chttp_h2_server_stream_probe *)user;
  if (probe == NULL || request == NULL || out_sink == NULL || request->http_major != 2u ||
      strcmp(request->path, "/echo") != 0)
    return SALTS_EINVAL;
  ++probe->opens;
  *out_sink = (chttp_body_sink){.write = chttp_h2_server_stream_write, .user = probe};
  return SALTS_OK;
}

static int chttp_h2_server_jwt_stream_open(void *user, const chttp_server_request_view *request,
                                           chttp_body_sink *out_sink) {
  chttp_h2_server_stream_probe *probe = (chttp_h2_server_stream_probe *)user;
  if (probe == NULL || request == NULL || out_sink == NULL || request->http_major != 2u ||
      strcmp(request->path, "/jwt-echo") != 0 || request->jwt_claims == NULL ||
      request->jwt_claims->subject == NULL || strcmp(request->jwt_claims->subject, "alice") != 0)
    return SALTS_EPROTO;
  ++probe->opens;
  *out_sink = (chttp_body_sink){.write = chttp_h2_server_stream_write, .user = probe};
  return SALTS_OK;
}

static int chttp_h2_server_failing_stream_open(void *user, const chttp_server_request_view *request,
                                               chttp_body_sink *out_sink) {
  chttp_h2_server_stream_probe *probe = (chttp_h2_server_stream_probe *)user;
  if (probe == NULL || request == NULL || out_sink == NULL || request->http_major != 2u ||
      strcmp(request->path, "/fail-upload") != 0)
    return SALTS_EINVAL;
  ++probe->opens;
  *out_sink = (chttp_body_sink){.write = chttp_h2_stream_fail_write, .user = probe};
  return SALTS_OK;
}

static void chttp_h2_server_stream_close(void *user, chttp_body_sink *sink, int status) {
  chttp_h2_server_stream_probe *probe = (chttp_h2_server_stream_probe *)user;
  if (probe == NULL || sink == NULL) return;
  ++probe->closes;
  probe->close_status = status;
}

static int chttp_h2_server_response_read(void *user, void *buffer, size_t capacity,
                                         size_t *out_size) {
  chttp_h2_server_stream_probe *probe = (chttp_h2_server_stream_probe *)user;
  size_t size;
  if (probe == NULL || buffer == NULL || capacity == 0u || out_size == NULL) return SALTS_EINVAL;
  ++probe->response_reads;
  if (probe->response_offset == probe->size) {
    *out_size = 0u;
    return SALTS_OK;
  }
  size = probe->size - probe->response_offset;
  if (size > 2u) size = 2u;
  if (size > capacity) size = capacity;
  memcpy(buffer, probe->data + probe->response_offset, size);
  probe->response_offset += size;
  *out_size = size;
  return SALTS_OK;
}

static int chttp_h2_server_stream_handler(void *user, const chttp_server_request_view *request,
                                          chttp_server_response *response) {
  chttp_h2_server_stream_probe *probe = (chttp_h2_server_stream_probe *)user;
  chttp_body_source source;
  if (probe == NULL || request == NULL || !request->body_streamed || request->body != NULL ||
      request->body_size != probe->size || probe->closes != 1u || probe->close_status != SALTS_OK)
    return SALTS_EPROTO;
  source = (chttp_body_source){.read = chttp_h2_server_response_read,
                               .user = probe,
                               .content_length = probe->size,
                               .content_length_known = 1};
  return chttp_server_response_source(response, 200u, "application/octet-stream", &source);
}

static int chttp_h2_server_jwt_stream_handler(void *user, const chttp_server_request_view *request,
                                              chttp_server_response *response) {
  chttp_h2_server_stream_probe *probe = (chttp_h2_server_stream_probe *)user;
  if (probe == NULL || request == NULL) return SALTS_EPROTO;
  ++probe->handler_calls;
  if (request->jwt_claims == NULL || request->jwt_claims->subject == NULL ||
      strcmp(request->jwt_claims->subject, "alice") != 0 || !request->body_streamed ||
      request->body != NULL || request->body_size != probe->size || probe->closes != 1u ||
      probe->close_status != SALTS_OK)
    return SALTS_EPROTO;
  return chttp_server_reply(response, 200u, "text/plain", "ok", 2u);
}

static void chttp_h2_stream_complete(void *user, chttp_request request,
                                     const chttp_response_view *response,
                                     const chttp_error *error) {
  chttp_h2_stream_completion *completion = (chttp_h2_stream_completion *)user;
  (void)request;
  if (completion == NULL) return;
  ++completion->calls;
  if (error != NULL) {
    completion->status = error->status;
    return;
  }
  completion->status = SALTS_OK;
  completion->response_status = response->status_code;
  completion->body_size = response->body_size;
  if (response->body != NULL && response->body_size <= sizeof(completion->body))
    memcpy(completion->body, response->body, response->body_size);
}

static native_io_backend_kind chttp_h2_server_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config chttp_h2_server_test_network(size_t connections) {
  const cnet_client_config config = {.backend = chttp_h2_server_test_backend(),
                                     .connection_capacity = connections,
                                     .command_capacity = 16u,
                                     .request_capacity = 16u,
                                     .completion_batch_capacity = 8u,
                                     .event_capacity = 32u,
                                     .max_send_bytes = 64u * 1024u,
                                     .receive_buffer_bytes = 4096u,
                                     .connect_timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS,
                                     .read_timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS,
                                     .write_timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS};
  return config;
}

static chttp_server_config chttp_h2_server_test_config(void) {
  const chttp_server_config config = {.host = "127.0.0.1",
                                      .port = 0u,
                                      .backlog = 8u,
                                      .network = chttp_h2_server_test_network(4u),
                                      .route_capacity = 8u,
                                      .middleware_capacity = 4u,
                                      .max_route_middleware_count = 4u,
                                      .max_route_param_count = 4u,
                                      .max_route_param_bytes = 128u,
                                      .max_target_bytes = 256u,
                                      .max_header_count = 16u,
                                      .max_header_bytes = 4096u,
                                      .max_request_body_bytes = 4096u,
                                      .max_response_header_count = 16u,
                                      .max_response_header_bytes = 4096u,
                                      .max_response_body_bytes = 4096u,
                                      .session_capacity = 4u,
                                      .session_entry_capacity = 4u,
                                      .max_session_key_bytes = 32u,
                                      .max_session_value_bytes = 64u,
                                      .session_idle_timeout_ms = 60000u,
                                      .session_cookie_name = "h2_sid",
                                      .poll_slice_ms = 2u,
                                      .enable_http2 = 1,
                                      .h2_stream_capacity = 8u,
                                      .h2_input_buffer_bytes = 64u * 1024u,
                                      .h2_output_buffer_bytes = 64u * 1024u,
                                      .h2_hpack_dynamic_table_bytes = 4096u,
                                      .h2_max_settings_count = 16u};
  return config;
}

static chttp_client_config chttp_h2_server_test_client_config(void) {
  const chttp_client_config config = {.network = chttp_h2_server_test_network(2u),
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

static int chttp_h2_server_test_middleware(void *user, const chttp_server_request_view *request,
                                           chttp_server_response *response,
                                           chttp_server_next *next) {
  chttp_h2_server_test_probe *probe = (chttp_h2_server_test_probe *)user;
  (void)request;
  (void)response;
  ++probe->middleware_calls;
  return chttp_server_next_call(next);
}

static int chttp_h2_server_test_handler(void *user, const chttp_server_request_view *request,
                                        chttp_server_response *response) {
  chttp_h2_server_test_probe *probe = (chttp_h2_server_test_probe *)user;
  const char *id = chttp_server_request_param(request, "id");
  ++probe->handler_calls;
  if (request->http_major != 2u || request->http_minor != 0u || id == NULL ||
      strcmp(id, "42") != 0 ||
      strcmp(chttp_server_request_header(request, "x-request"), "h2c") != 0)
    return SALTS_EPROTO;
  if (chttp_server_response_set_header(response, "X-Transport", "h2c") != SALTS_OK)
    return SALTS_EIO;
  return chttp_server_reply(response, 200u, "text/plain", "pong", 4u);
}

static int chttp_h2_server_test_value_handler(void *user, const chttp_server_request_view *request,
                                              chttp_server_response *response) {
  const char *value = chttp_server_request_param(request, "value");
  (void)user;
  if (value == NULL) return SALTS_EPROTO;
  return chttp_server_reply(response, 200u, "text/plain", value, strlen(value));
}

static int chttp_h2_server_test_body_handler(void *user, const chttp_server_request_view *request,
                                             chttp_server_response *response) {
  (void)user;
  return chttp_server_reply(response, 200u, "application/octet-stream", request->body,
                            request->body_size);
}

static int chttp_h2_server_test_head_handler(void *user, const chttp_server_request_view *request,
                                             chttp_server_response *response) {
  (void)user;
  (void)request;
  return chttp_server_reply(response, 200u, "text/plain", "head-body", 9u);
}

static int chttp_h2_server_test_session_handler(void *user,
                                                const chttp_server_request_view *request,
                                                chttp_server_response *response) {
  const char *value;
  (void)user;
  if (request->session == NULL) return SALTS_EPROTO;
  value = chttp_session_get(request->session, "value");
  if (value == NULL) {
    if (chttp_session_set(request->session, "value", "persisted") != SALTS_OK) return SALTS_EIO;
    value = "created";
  }
  return chttp_server_reply(response, 200u, "text/plain", value, strlen(value));
}

static int chttp_h2_server_test_version_handler(void *user,
                                                const chttp_server_request_view *request,
                                                chttp_server_response *response) {
  const char *version = request->http_major == 2u ? "h2" : "h1";
  (void)user;
  if (chttp_server_response_set_header(response, "X-Protocol", version) != SALTS_OK)
    return SALTS_EIO;
  return chttp_server_reply(response, 200u, "text/plain", version, 2u);
}

static int chttp_h2_server_test_payload_handler(void *user,
                                                const chttp_server_request_view *request,
                                                chttp_server_response *response) {
  const chttp_h2_server_test_payload *payload = (const chttp_h2_server_test_payload *)user;
  int status;
  (void)request;
  if (payload == NULL) return SALTS_EINVAL;
  status =
      chttp_server_reply(response, 200u, "application/octet-stream", payload->data, payload->size);
  if (status == SALTS_OK && payload->handler_called != NULL)
    atomic_store_explicit(payload->handler_called, 1, memory_order_release);
  return status;
}

static int chttp_h2_server_test_deferred_handler(void *user,
                                                 const chttp_server_request_view *request,
                                                 chttp_server_response *response) {
  chttp_h2_server_test_deferred *deferred = (chttp_h2_server_test_deferred *)user;
  if (deferred == NULL || request == NULL || request->http_major != 2u) return SALTS_EPROTO;
  deferred->defer_status = chttp_server_response_defer(response, &deferred->handle);
  atomic_fetch_add_explicit(&deferred->admitted, 1, memory_order_release);
  return deferred->defer_status;
}

static int chttp_h2_server_test_wait_atomic(atomic_int *value, int expected) {
  const uint64_t deadline = salts_monotonic_ms() + CHTTP_H2_SERVER_TEST_TIMEOUT_MS;
  while (atomic_load_explicit(value, memory_order_acquire) < expected &&
         salts_monotonic_ms() < deadline)
    salts_thread_yield();
  return atomic_load_explicit(value, memory_order_acquire) >= expected ? SALTS_OK : SALTS_ETIMEDOUT;
}

static int chttp_h2_server_test_file_handler(void *user, const chttp_server_request_view *request,
                                             chttp_server_response *response) {
  const char *path = (const char *)user;
  (void)request;
  return chttp_server_response_file(response, 200u, "application/octet-stream", path);
}

static int chttp_h2_server_failing_response_read(void *user, void *buffer, size_t capacity,
                                                 size_t *out_size) {
  (void)user;
  (void)buffer;
  (void)capacity;
  (void)out_size;
  return SALTS_EIO;
}

static int chttp_h2_server_failing_response_handler(void *user,
                                                    const chttp_server_request_view *request,
                                                    chttp_server_response *response) {
  const chttp_body_source source = {.read = chttp_h2_server_failing_response_read};
  (void)user;
  (void)request;
  return chttp_server_response_source(response, 200u, "application/octet-stream", &source);
}

static void chttp_h2_server_test_complete(void *user, chttp_request request,
                                          const chttp_response_view *response,
                                          const chttp_error *error) {
  chttp_h2_server_test_completion *completion = (chttp_h2_server_test_completion *)user;
  (void)request;
  ++completion->calls;
  completion->status = error == NULL ? SALTS_OK : error->status;
  if (response == NULL) return;
  completion->response_status = response->status_code;
  completion->body_size = response->body_size < sizeof(completion->body) - 1u
                              ? response->body_size
                              : sizeof(completion->body) - 1u;
  if (completion->body_size != 0u) memcpy(completion->body, response->body, completion->body_size);
  completion->body[completion->body_size] = '\0';
}

static int chttp_h2_server_test_start(chttp_server *server, chttp_server_config *config,
                                      uint16_t *out_port) {
  int status = chttp_server_init(server, config);
  if (status == SALTS_OK) status = chttp_server_start(server);
  if (status == SALTS_OK) status = chttp_server_port(server, out_port);
  return status;
}

static int chttp_h2_server_test_endpoint(uint16_t port, char *uri, size_t uri_capacity,
                                         char *authority, size_t authority_capacity) {
  const int uri_size = snprintf(uri, uri_capacity, "tcp://127.0.0.1:%u", (unsigned int)port);
  const int authority_size =
      snprintf(authority, authority_capacity, "127.0.0.1:%u", (unsigned int)port);
  return uri_size > 0 && (size_t)uri_size < uri_capacity && authority_size > 0 &&
                 (size_t)authority_size < authority_capacity
             ? SALTS_OK
             : SALTS_EMSGSIZE;
}

static void chttp_h2_server_test_socket_close(chttp_h2_server_test_socket socket_value) {
  if (socket_value == CHTTP_H2_SERVER_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
  (void)WSACleanup();
#else
  (void)close(socket_value);
#endif
}

static int chttp_h2_server_test_socket_timeout(chttp_h2_server_test_socket socket_value) {
#if defined(_WIN32)
  const DWORD timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                    sizeof(timeout_ms)) == 0
             ? SALTS_OK
             : SALTS_EIO;
#else
  const struct timeval timeout = {CHTTP_H2_SERVER_TEST_TIMEOUT_MS / 1000,
                                  (CHTTP_H2_SERVER_TEST_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0
             ? SALTS_OK
             : SALTS_EIO;
#endif
}

static int chttp_h2_server_test_socket_connect(uint16_t port,
                                               chttp_h2_server_test_socket *out_socket) {
  struct sockaddr_in address;
  chttp_h2_server_test_socket socket_value;
  if (out_socket == NULL || port == 0u) return SALTS_EINVAL;
  *out_socket = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
#if defined(_WIN32)
  {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return SALTS_EIO;
  }
#endif
  socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_value == CHTTP_H2_SERVER_TEST_INVALID_SOCKET) {
#if defined(_WIN32)
    (void)WSACleanup();
#endif
    return SALTS_EIO;
  }
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (chttp_h2_server_test_socket_timeout(socket_value) != SALTS_OK ||
      connect(socket_value, (const struct sockaddr *)&address, sizeof(address)) != 0) {
    chttp_h2_server_test_socket_close(socket_value);
    return SALTS_EIO;
  }
  *out_socket = socket_value;
  return SALTS_OK;
}

static int chttp_h2_server_test_socket_send(chttp_h2_server_test_socket socket_value,
                                            const void *data, size_t size) {
  const unsigned char *bytes = (const unsigned char *)data;
  size_t offset = 0u;
  while (offset < size) {
    const size_t remaining = size - offset;
    const int chunk = remaining > (size_t)INT_MAX ? INT_MAX : (int)remaining;
    const int sent = send(socket_value, (const char *)bytes + offset, chunk, 0);
    if (sent <= 0) return SALTS_EIO;
    offset += (size_t)sent;
  }
  return SALTS_OK;
}

static chttp_h2_server_test_peer_result *
chttp_h2_server_test_peer_result_find(chttp_h2_server_test_peer *peer, int32_t stream_id) {
  size_t index;
  for (index = 0u; index < peer->result_count; ++index)
    if (peer->results[index].stream_id == stream_id) return &peer->results[index];
  return NULL;
}

static int chttp_h2_server_test_peer_begin(void *user, int32_t stream_id) {
  return chttp_h2_server_test_peer_result_find((chttp_h2_server_test_peer *)user, stream_id) == NULL
             ? -1
             : 0;
}

static int chttp_h2_server_test_peer_header(void *user, int32_t stream_id, const char *name,
                                            size_t name_size, const char *value,
                                            size_t value_size) {
  chttp_h2_server_test_peer_result *result =
      chttp_h2_server_test_peer_result_find((chttp_h2_server_test_peer *)user, stream_id);
  if (result == NULL) return -1;
  if (name_size == sizeof(":status") - 1u && memcmp(name, ":status", name_size) == 0) {
    if (value_size != 3u || value[0] < '1' || value[0] > '9' || value[1] < '0' || value[1] > '9' ||
        value[2] < '0' || value[2] > '9')
      return -1;
    result->status = (unsigned int)(value[0] - '0') * 100u + (unsigned int)(value[1] - '0') * 10u +
                     (unsigned int)(value[2] - '0');
  }
  return 0;
}

static int chttp_h2_server_test_peer_end(void *user, int32_t stream_id, int end_stream) {
  (void)end_stream;
  return chttp_h2_server_test_peer_result_find((chttp_h2_server_test_peer *)user, stream_id) == NULL
             ? -1
             : 0;
}

static int chttp_h2_server_test_peer_data(void *user, int32_t stream_id, const uint8_t *data,
                                          size_t size) {
  chttp_h2_server_test_peer *peer = (chttp_h2_server_test_peer *)user;
  chttp_h2_server_test_peer_result *result = chttp_h2_server_test_peer_result_find(peer, stream_id);
  size_t copied;
  if (result == NULL) return -1;
  if (size != 0u) {
    copied = size < sizeof(result->body) - 1u - result->body_size
                 ? size
                 : sizeof(result->body) - 1u - result->body_size;
    if (copied != 0u) memcpy(result->body + result->body_size, data, copied);
    result->body_size += copied;
    result->received_body_size += size;
    result->body[result->body_size] = '\0';
    if (chttp_h2_proto_consume_stream(peer->protocol, stream_id, size) != 0 ||
        chttp_h2_proto_consume_connection(peer->protocol, size) != 0)
      return -1;
  }
  return 0;
}

static int chttp_h2_server_test_peer_close(void *user, int32_t stream_id, uint32_t error_code) {
  chttp_h2_server_test_peer *peer = (chttp_h2_server_test_peer *)user;
  chttp_h2_server_test_peer_result *result = chttp_h2_server_test_peer_result_find(peer, stream_id);
  if (result == NULL || result->closed) return 0;
  result->closed = true;
  result->error_code = error_code;
  ++peer->close_count;
  return 0;
}

static void chttp_h2_server_test_peer_goaway(void *user, uint32_t last_stream_id,
                                             uint32_t error_code) {
  chttp_h2_server_test_peer *peer = (chttp_h2_server_test_peer *)user;
  (void)last_stream_id;
  ++peer->goaway_count;
  peer->goaway_error = error_code;
}

static int chttp_h2_server_test_peer_init(chttp_h2_server_test_peer *peer) {
  const chttp_h2_proto_config config = {.stream_capacity = 8u,
                                        .output_buffer_bytes = 64u * 1024u,
                                        .input_buffer_bytes = 64u * 1024u,
                                        .header_block_bytes = 4096u,
                                        .max_header_list_bytes = 4096u,
                                        .hpack_dynamic_table_bytes = 4096u,
                                        .max_hpack_string_bytes = 4096u,
                                        .max_settings_count = 16u};
  const chttp_h2_proto_callbacks callbacks = {.user_data = peer,
                                              .on_begin_headers = chttp_h2_server_test_peer_begin,
                                              .on_header = chttp_h2_server_test_peer_header,
                                              .on_end_headers = chttp_h2_server_test_peer_end,
                                              .on_data = chttp_h2_server_test_peer_data,
                                              .on_stream_close = chttp_h2_server_test_peer_close,
                                              .on_goaway = chttp_h2_server_test_peer_goaway};
  memset(peer, 0, sizeof(*peer));
  peer->protocol = chttp_h2_proto_create(CHTTP_H2_PROTO_CLIENT, &config, &callbacks);
  return peer->protocol == NULL ? SALTS_ENOMEM : SALTS_OK;
}

static int chttp_h2_server_test_peer_submit(chttp_h2_server_test_peer *peer,
                                            const chttp_h2_hpack_header *headers,
                                            size_t header_count) {
  int32_t stream_id = 0;
  if (peer == NULL || peer->protocol == NULL ||
      peer->result_count >= sizeof(peer->results) / sizeof(peer->results[0]))
    return SALTS_ENOBUFS;
  if (chttp_h2_proto_submit_request(peer->protocol, headers, header_count, NULL, 0u, &stream_id) !=
      0)
    return SALTS_EPROTO;
  peer->results[peer->result_count++].stream_id = stream_id;
  return SALTS_OK;
}

static int chttp_h2_server_test_peer_submit_body(chttp_h2_server_test_peer *peer,
                                                 const chttp_h2_hpack_header *headers,
                                                 size_t header_count, const void *body,
                                                 size_t body_size) {
  int32_t stream_id = 0;
  if (peer == NULL || peer->protocol == NULL || (body_size != 0u && body == NULL) ||
      peer->result_count >= sizeof(peer->results) / sizeof(peer->results[0]))
    return SALTS_ENOBUFS;
  if (chttp_h2_proto_submit_request(peer->protocol, headers, header_count, body, body_size,
                                    &stream_id) != 0)
    return SALTS_EPROTO;
  peer->results[peer->result_count++].stream_id = stream_id;
  return SALTS_OK;
}

static chttp_h2_proto_source_result chttp_h2_server_test_hold_source(void *user, uint8_t *buffer,
                                                                     size_t capacity) {
  chttp_h2_server_test_source *source = (chttp_h2_server_test_source *)user;
  if (source == NULL || buffer == NULL || capacity == 0u)
    return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
  ++source->calls;
  buffer[0] = 'x';
  return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_DATA, 1u};
}

static int chttp_h2_server_test_peer_submit_open(chttp_h2_server_test_peer *peer,
                                                 const chttp_h2_hpack_header *headers,
                                                 size_t header_count,
                                                 chttp_h2_server_test_source *source,
                                                 int32_t *out_stream_id) {
  int32_t stream_id = 0;
  if (peer == NULL || peer->protocol == NULL || source == NULL || out_stream_id == NULL ||
      peer->result_count >= sizeof(peer->results) / sizeof(peer->results[0]))
    return SALTS_ENOBUFS;
  if (chttp_h2_proto_submit_request_ex(peer->protocol, headers, header_count, NULL, 0u,
                                       chttp_h2_server_test_hold_source, source, 0u, 0, 0,
                                       &stream_id) != 0)
    return SALTS_EPROTO;
  peer->results[peer->result_count++].stream_id = stream_id;
  *out_stream_id = stream_id;
  return SALTS_OK;
}

static int chttp_h2_server_test_peer_submit_tunnel(chttp_h2_server_test_peer *peer,
                                                   const chttp_h2_hpack_header *headers,
                                                   size_t header_count, int32_t *out_stream_id) {
  int32_t stream_id = 0;
  if (peer == NULL || peer->protocol == NULL || out_stream_id == NULL ||
      peer->result_count >= sizeof(peer->results) / sizeof(peer->results[0]))
    return SALTS_ENOBUFS;
  if (chttp_h2_proto_submit_request_headers(peer->protocol, headers, header_count, 0, &stream_id) !=
      0)
    return SALTS_EPROTO;
  peer->results[peer->result_count++].stream_id = stream_id;
  *out_stream_id = stream_id;
  return SALTS_OK;
}

static int chttp_h2_server_test_peer_send_once(chttp_h2_server_test_peer *peer,
                                               chttp_h2_server_test_socket socket_value) {
  const uint8_t *wire = NULL;
  const ptrdiff_t wire_size = chttp_h2_proto_send(peer->protocol, &wire);
  if (wire_size <= 0) return SALTS_EPROTO;
  return chttp_h2_server_test_socket_send(socket_value, wire, (size_t)wire_size);
}

static int chttp_h2_server_test_peer_limit_stream_window(chttp_h2_server_test_peer *peer,
                                                         uint32_t window_size) {
  const uint32_t identifiers[] = {CHTTP_H2_SETTING_INITIAL_WINDOW_SIZE};
  const uint32_t values[] = {window_size};
  uint8_t wire[CHTTP_H2_FRAME_HEADER_SIZE + 6u];
  size_t header_size = 0u;
  size_t payload_size = 0u;
  if (peer == NULL || peer->protocol == NULL ||
      chttp_h2_frame_settings_encode(wire + CHTTP_H2_FRAME_HEADER_SIZE, 6u, &payload_size,
                                     identifiers, values, 1u) != 0 ||
      chttp_h2_frame_header_encode(wire, CHTTP_H2_FRAME_HEADER_SIZE, &header_size,
                                   (uint32_t)payload_size, CHTTP_H2_FRAME_SETTINGS, 0u, 0u) != 0)
    return SALTS_EPROTO;
  return chttp_h2_proto_recv(peer->protocol, wire, header_size + payload_size) ==
                 (ptrdiff_t)(header_size + payload_size)
             ? SALTS_OK
             : SALTS_EPROTO;
}

static int chttp_h2_server_test_send_header_block(chttp_h2_server_test_socket socket_value,
                                                  int32_t stream_id,
                                                  const chttp_h2_hpack_header *headers,
                                                  size_t header_count, uint8_t flags) {
  const chttp_h2_hpack_config config = {
      .max_dynamic_table_bytes = 4096u, .max_header_block_bytes = 4096u, .max_string_bytes = 4096u};
  chttp_h2_hpack_buffer block = {0};
  chttp_h2_hpack *hpack = NULL;
  uint8_t frame_header[CHTTP_H2_FRAME_HEADER_SIZE];
  size_t frame_header_size = 0u;
  int status = SALTS_EPROTO;
  hpack = chttp_h2_hpack_create(&config);
  if (hpack == NULL || chttp_h2_hpack_encoder_set_max_size(hpack, 0u) != 0 ||
      chttp_h2_hpack_buffer_init(&block, 64u, config.max_header_block_bytes) != 0 ||
      chttp_h2_hpack_encode(hpack, &block, headers, header_count) != 0 || block.size > UINT32_MAX ||
      chttp_h2_frame_header_encode(frame_header, sizeof(frame_header), &frame_header_size,
                                   (uint32_t)block.size, CHTTP_H2_FRAME_HEADERS, flags,
                                   (uint32_t)stream_id) != 0)
    goto cleanup;
  status = chttp_h2_server_test_socket_send(socket_value, frame_header, frame_header_size);
  if (status == SALTS_OK)
    status = chttp_h2_server_test_socket_send(socket_value, block.data, block.size);

cleanup:
  chttp_h2_hpack_buffer_destroy(&block);
  chttp_h2_hpack_destroy(hpack);
  return status;
}

static int chttp_h2_server_test_peer_send(chttp_h2_server_test_peer *peer,
                                          chttp_h2_server_test_socket socket_value) {
  const uint8_t *wire = NULL;
  ptrdiff_t wire_size;
  while (chttp_h2_proto_want_write(peer->protocol)) {
    wire_size = chttp_h2_proto_send(peer->protocol, &wire);
    if (wire_size < 0) return SALTS_EPROTO;
    if (wire_size == 0) return SALTS_OK;
    if (chttp_h2_server_test_socket_send(socket_value, wire, (size_t)wire_size) != SALTS_OK)
      return SALTS_EIO;
  }
  return SALTS_OK;
}

static int chttp_h2_server_test_peer_receive_once(chttp_h2_server_test_peer *peer,
                                                  chttp_h2_server_test_socket socket_value) {
  unsigned char input[4096];
  const int received = recv(socket_value, (char *)input, sizeof(input), 0);
  if (received <= 0) return SALTS_EIO;
  return chttp_h2_proto_recv(peer->protocol, input, (size_t)received) == received ? SALTS_OK
                                                                                  : SALTS_EPROTO;
}

static int chttp_h2_server_test_peer_pump(chttp_h2_server_test_peer *peer,
                                          chttp_h2_server_test_socket socket_value,
                                          size_t expected_closes) {
  unsigned char input[4096];
  size_t attempts = 0u;
  while (peer->close_count < expected_closes && attempts++ < 40u) {
    int received;
    int status = chttp_h2_server_test_peer_send(peer, socket_value);
    if (status != SALTS_OK) return status;
    received = recv(socket_value, (char *)input, sizeof(input), 0);
    if (received <= 0) return SALTS_EIO;
    if (chttp_h2_proto_recv(peer->protocol, input, (size_t)received) != received)
      return SALTS_EPROTO;
  }
  return peer->close_count == expected_closes ? SALTS_OK : SALTS_ETIMEDOUT;
}

static int chttp_h2_server_test_peer_receive(chttp_h2_server_test_peer *peer,
                                             chttp_h2_server_test_socket socket_value,
                                             size_t expected_closes) {
  unsigned char input[4096];
  size_t attempts = 0u;
  if (peer == NULL || expected_closes == 0u ||
      expected_closes > sizeof(peer->results) / sizeof(peer->results[0]))
    return SALTS_EINVAL;
  while (
      peer->close_count < expected_closes &&
      (peer->result_count < expected_closes || peer->results[expected_closes - 1u].status == 0u) &&
      attempts++ < 40u) {
    const int received = recv(socket_value, (char *)input, sizeof(input), 0);
    if (received <= 0) return SALTS_EIO;
    if (chttp_h2_proto_recv(peer->protocol, input, (size_t)received) != received)
      return SALTS_EPROTO;
  }
  return peer->close_count == expected_closes || (peer->result_count >= expected_closes &&
                                                  peer->results[expected_closes - 1u].status != 0u)
             ? SALTS_OK
             : SALTS_ETIMEDOUT;
}

static int chttp_h2_server_test_peer_read_to_close(chttp_h2_server_test_peer *peer,
                                                   chttp_h2_server_test_socket socket_value) {
  unsigned char input[4096];
  for (;;) {
    const int received = recv(socket_value, (char *)input, sizeof(input), 0);
    if (received == 0) return SALTS_OK;
    if (received < 0) return SALTS_EIO;
    if (chttp_h2_proto_recv(peer->protocol, input, (size_t)received) != received)
      return SALTS_EPROTO;
  }
}

static void chttp_h2_server_test_peer_destroy(chttp_h2_server_test_peer *peer) {
  if (peer == NULL) return;
  chttp_h2_proto_destroy(peer->protocol);
  memset(peer, 0, sizeof(*peer));
}

static void chttp_h2_server_test_stop_thread(void *user) {
  chttp_h2_server_test_stop *stop = (chttp_h2_server_test_stop *)user;
  stop->status = chttp_server_stop(stop->server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS);
}

spec("CHTTP background HTTP/2 server") {
  it("completes an h2c response through a cross-thread deferred handle") {
    chttp_server server = {0};
    chttp_async_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_h2_server_test_deferred deferred = {0};
    chttp_h2_server_test_completion completion = {0};
    chttp_server_deferred duplicate = CHTTP_SERVER_DEFERRED_INIT;
    chttp_server_deferred_response response = {0};
    chttp_request request = {0};
    chttp_request_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t polls = 0u;

    atomic_init(&deferred.admitted, 0);
    server_config.session_capacity = 0u;
    server_config.session_entry_capacity = 0u;
    server_config.max_session_key_bytes = 0u;
    server_config.max_session_value_bytes = 0u;
    server_config.session_idle_timeout_ms = 0u;
    server_config.session_cookie_name = NULL;
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/deferred", chttp_h2_server_test_deferred_handler, &deferred),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_async_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/deferred",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_server_test_complete,
                                      .user = &completion,
                                      .protocol = CHTTP_HTTP_2};
    check_equal(chttp_async_client_submit(&client, &options, &request), SALTS_OK);
    while (!atomic_load_explicit(&deferred.admitted, memory_order_acquire) && polls++ < 40u)
      check_equal(chttp_async_client_poll(&client, 25u, &completions), SALTS_OK);

    check_equal(atomic_load_explicit(&deferred.admitted, memory_order_acquire), 1);
    check_equal(deferred.defer_status, SALTS_OK);
    response.size = sizeof(response);
    response.status_code = 202u;
    response.content_type = "text/plain";
    response.body = "later";
    response.body_size = 5u;
    duplicate = deferred.handle;
    check_equal(chttp_server_deferred_reply(&deferred.handle, &response), SALTS_OK);
    {
      const int duplicate_status = chttp_server_deferred_reply(&duplicate, &response);
      check_true(duplicate_status == SALTS_EALREADY || duplicate_status == SALTS_ENOENT);
    }
    while (completion.calls == 0u && polls++ < 80u)
      check_equal(chttp_async_client_poll(&client, 25u, &completions), SALTS_OK);

    check_equal(completion.calls, 1u);
    check_equal(completion.status, SALTS_OK);
    check_equal(completion.response_status, 202u);
    check_equal(completion.body, "later");
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("submits a deferred stream while a sibling remains flow-control blocked") {
    static const char blocked_body[] = "blocked";
    static const chttp_h2_server_test_payload payload = {blocked_body, sizeof(blocked_body) - 1u};
    static const chttp_h2_hpack_header blocked_headers[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/blocked", sizeof("/blocked") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    static const chttp_h2_hpack_header deferred_headers[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/deferred", sizeof("/deferred") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    static const chttp_h2_hpack_header barrier_headers[] = {
        {":method", sizeof(":method") - 1u, "HEAD", sizeof("HEAD") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/barrier", sizeof("/barrier") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    static const chttp_server_deferred_response response = {
        .size = sizeof(chttp_server_deferred_response), .status_code = 204u};
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_deferred deferred = {0};
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    uint16_t port = 0u;

    atomic_init(&deferred.admitted, 0);
    config.session_capacity = 0u;
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(chttp_server_get(&server, "/blocked", chttp_h2_server_test_payload_handler,
                                 (void *)&payload),
                SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/deferred", chttp_h2_server_test_deferred_handler, &deferred),
        SALTS_OK);
    check_equal(chttp_server_head(&server, "/barrier", chttp_h2_server_test_head_handler, NULL),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(chttp_h2_proto_set_local_settings(peer.protocol, 4096u, 0u, 8u, 0u, 16384u, 4096u),
                0);
    check_equal(chttp_h2_server_test_peer_submit(
                    &peer, blocked_headers, sizeof(blocked_headers) / sizeof(blocked_headers[0])),
                SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_receive(&peer, socket_value, 1u), SALTS_OK);
    check_equal(peer.results[0].status, 200u);
    check(!peer.results[0].closed);

    check_equal(
        chttp_h2_server_test_peer_submit(&peer, deferred_headers,
                                         sizeof(deferred_headers) / sizeof(deferred_headers[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_wait_atomic(&deferred.admitted, 1), SALTS_OK);
    check_equal(deferred.defer_status, SALTS_OK);
    check_equal(chttp_server_deferred_reply(&deferred.handle, &response), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_submit(
                    &peer, barrier_headers, sizeof(barrier_headers) / sizeof(barrier_headers[0])),
                SALTS_OK);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 2u), SALTS_OK);
    check(peer.results[1].closed);
    check_equal(peer.results[1].error_code, CHTTP_H2_ERR_NO_ERROR);
    check_equal(peer.results[1].status, 204u);
    check(peer.results[2].closed);
    check_equal(peer.results[2].status, 200u);

    check_equal(chttp_h2_proto_submit_rst_stream(peer.protocol, peer.results[0].stream_id,
                                                 CHTTP_H2_ERR_CANCEL),
                0);
    if (!peer.results[1].closed)
      check_equal(chttp_h2_proto_submit_rst_stream(peer.protocol, peer.results[1].stream_id,
                                                   CHTTP_H2_ERR_CANCEL),
                  0);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("completes a deferred response on TLS negotiated ALPN h2") {
    static const char *alpn[] = {"h2"};
    char *cert_path = tt_make_temp_file("chttp-h2-deferred-cert", ".pem");
    char *key_path = tt_make_temp_file("chttp-h2-deferred-key", ".pem");
    chttp_server server = {0};
    chttp_async_client client = {0};
    chttp_tls_profile profile = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_h2_server_test_deferred deferred = {0};
    chttp_h2_server_test_completion completion = {0};
    cnet_tls_server_config server_tls;
    cnet_tls_client_config client_tls;
    chttp_server_deferred_response response = {.size = sizeof(response),
                                               .status_code = 202u,
                                               .content_type = "text/plain",
                                               .body = "tls-later",
                                               .body_size = sizeof("tls-later") - 1u};
    chttp_request request = {0};
    chttp_request_options options;
    char uri[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t polls = 0u;

    check_not_null(cert_path);
    check_not_null(key_path);
    check_equal(tt_write_file(cert_path, CHTTP_TLS_TEST_CERTIFICATE,
                              sizeof(CHTTP_TLS_TEST_CERTIFICATE) - 1u),
                0);
    check_equal(tt_write_file(key_path, CHTTP_TLS_TEST_KEY, sizeof(CHTTP_TLS_TEST_KEY) - 1u), 0);
    atomic_init(&deferred.admitted, 0);
    server_config.session_capacity = 0u;
    server_config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    server_config.network.tls_handshake_timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS;
    client_config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    client_config.network.tls_handshake_timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS;
    server_tls = (cnet_tls_server_config){.size = sizeof(server_tls),
                                          .cert_file = cert_path,
                                          .key_file = key_path,
                                          .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
                                          .alpn_protocols = alpn,
                                          .alpn_protocol_count = 1u};
    client_tls = (cnet_tls_client_config){.size = sizeof(client_tls),
                                          .ca_file = cert_path,
                                          .server_name = "localhost",
                                          .alpn_protocols = alpn,
                                          .alpn_protocol_count = 1u};
    server_config.tls = &server_tls;

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/deferred", chttp_h2_server_test_deferred_handler, &deferred),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(chttp_tls_profile_init(&profile, &client_tls), SALTS_OK);
    check_equal(chttp_async_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = "localhost",
                                      .target = "/deferred",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_server_test_complete,
                                      .user = &completion,
                                      .tls = &profile,
                                      .protocol = CHTTP_HTTP_2};
    check_equal(chttp_async_client_submit(&client, &options, &request), SALTS_OK);
    while (!atomic_load_explicit(&deferred.admitted, memory_order_acquire) && polls++ < 80u)
      check_equal(chttp_async_client_poll(&client, 25u, &completions), SALTS_OK);
    check_equal(deferred.defer_status, SALTS_OK);
    check_equal(chttp_server_deferred_reply(&deferred.handle, &response), SALTS_OK);
    while (completion.calls == 0u && polls++ < 120u)
      check_equal(chttp_async_client_poll(&client, 25u, &completions), SALTS_OK);

    check_equal(completion.calls, 1u);
    check_equal(completion.status, SALTS_OK);
    check_equal(completion.response_status, 202u);
    check_equal(completion.body, "tls-later");
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(tt_remove_file(cert_path), 0);
    check_equal(tt_remove_file(key_path), 0);
    free(cert_path);
    free(key_path);
  }

  it("bounds deferred response copies and rejects a stale H2 generation") {
    static const chttp_h2_hpack_header headers[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/deferred", sizeof("/deferred") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    unsigned char oversized[4097] = {0};
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_deferred deferred = {0};
    chttp_server_deferred stale = CHTTP_SERVER_DEFERRED_INIT;
    chttp_server_deferred_response response = {.size = sizeof(response),
                                               .status_code = 200u,
                                               .content_type = "application/octet-stream",
                                               .body = oversized,
                                               .body_size = sizeof(oversized)};
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    uint16_t port = 0u;
    int duplicate_status;

    atomic_init(&deferred.admitted, 0);
    config.session_capacity = 0u;
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/deferred", chttp_h2_server_test_deferred_handler, &deferred),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(
        chttp_h2_server_test_peer_submit(&peer, headers, sizeof(headers) / sizeof(headers[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_wait_atomic(&deferred.admitted, 1), SALTS_OK);

    stale = deferred.handle;
    check_equal(chttp_server_deferred_reply(&deferred.handle, &response), SALTS_EMSGSIZE);
    check_not_null(deferred.handle.impl);
    check_equal(chttp_server_deferred_cancel(&deferred.handle), SALTS_OK);
    duplicate_status = chttp_server_deferred_cancel(&stale);
    check_true(duplicate_status == SALTS_EALREADY || duplicate_status == SALTS_ENOENT);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 1u), SALTS_OK);

    check_equal(
        chttp_h2_server_test_peer_submit(&peer, headers, sizeof(headers) / sizeof(headers[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_wait_atomic(&deferred.admitted, 2), SALTS_OK);
    check_true(deferred.handle.generation != stale.generation);
    check_equal(chttp_server_deferred_cancel(&stale), SALTS_ENOENT);
    check_equal(chttp_server_deferred_cancel(&deferred.handle), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 2u), SALTS_OK);

    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("retires a reset deferred stream without failing its H2 sibling") {
    static const chttp_h2_hpack_header deferred_headers[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/deferred", sizeof("/deferred") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    static const chttp_h2_hpack_header sibling_headers[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/values/sibling", sizeof("/values/sibling") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    static const chttp_server_deferred_response response = {
        .size = sizeof(chttp_server_deferred_response),
        .status_code = 200u,
        .content_type = "text/plain",
        .body = "too-late",
        .body_size = sizeof("too-late") - 1u};
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_deferred deferred = {0};
    chttp_server_deferred stale = CHTTP_SERVER_DEFERRED_INIT;
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    uint16_t port = 0u;

    atomic_init(&deferred.admitted, 0);
    config.session_capacity = 0u;
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/deferred", chttp_h2_server_test_deferred_handler, &deferred),
        SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(
        chttp_h2_server_test_peer_submit(&peer, deferred_headers,
                                         sizeof(deferred_headers) / sizeof(deferred_headers[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_submit(
                    &peer, sibling_headers, sizeof(sibling_headers) / sizeof(sibling_headers[0])),
                SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_wait_atomic(&deferred.admitted, 1), SALTS_OK);
    stale = deferred.handle;
    check_equal(chttp_h2_proto_submit_rst_stream(peer.protocol, peer.results[0].stream_id,
                                                 CHTTP_H2_ERR_CANCEL),
                0);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 2u), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_submit(
                    &peer, sibling_headers, sizeof(sibling_headers) / sizeof(sibling_headers[0])),
                SALTS_OK);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 3u), SALTS_OK);

    check_equal(chttp_server_deferred_reply(&stale, &response), SALTS_ENOENT);
    check_equal(peer.results[0].error_code, CHTTP_H2_ERR_CANCEL);
    check_equal(peer.results[1].error_code, CHTTP_H2_ERR_NO_ERROR);
    check_equal(peer.results[1].status, 200u);
    check_equal(peer.results[1].body, "sibling");
    check_equal(peer.results[2].error_code, CHTTP_H2_ERR_NO_ERROR);
    check_equal(peer.results[2].status, 200u);

    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("quarantines a reset H2 stream while a terminal writer owns its generation") {
    static const chttp_h2_hpack_header deferred_headers[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/deferred", sizeof("/deferred") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    static const chttp_h2_hpack_header sibling_headers[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/values/barrier", sizeof("/values/barrier") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    static const chttp_server_deferred_response response = {
        .size = sizeof(chttp_server_deferred_response),
        .status_code = 200u,
        .content_type = "text/plain",
        .body = "discarded",
        .body_size = sizeof("discarded") - 1u};
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_deferred deferred = {0};
    chttp_server_deferred stale = CHTTP_SERVER_DEFERRED_INIT;
    chttp_server_deferred_target *target;
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    uint64_t deadline;
    uint16_t port = 0u;

    atomic_init(&deferred.admitted, 0);
    config.session_capacity = 0u;
    config.h2_stream_capacity = 2u;
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/deferred", chttp_h2_server_test_deferred_handler, &deferred),
        SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(
        chttp_h2_server_test_peer_submit(&peer, deferred_headers,
                                         sizeof(deferred_headers) / sizeof(deferred_headers[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_submit(
                    &peer, sibling_headers, sizeof(sibling_headers) / sizeof(sibling_headers[0])),
                SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_wait_atomic(&deferred.admitted, 1), SALTS_OK);
    stale = deferred.handle;
    target = (chttp_server_deferred_target *)stale.impl;
    check_not_null(target);
    check_equal(chttp_server_deferred_claim(target->token, stale.generation), SALTS_OK);
    check_equal(chttp_h2_proto_submit_rst_stream(peer.protocol, peer.results[0].stream_id,
                                                 CHTTP_H2_ERR_CANCEL),
                0);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 2u), SALTS_OK);
    check_equal(chttp_server_deferred_token_state(
                    atomic_load_explicit(target->token, memory_order_acquire)),
                CHTTP_SERVER_DEFERRED_WRITING);

    atomic_store_explicit(
        target->token,
        chttp_server_deferred_token(stale.generation, CHTTP_SERVER_DEFERRED_CANCELED),
        memory_order_release);
    check_equal(cnet_client_wake(&target->server->network), SALTS_OK);
    deadline = salts_monotonic_ms() + CHTTP_H2_SERVER_TEST_TIMEOUT_MS;
    while (chttp_server_deferred_token_state(atomic_load_explicit(
               target->token, memory_order_acquire)) != CHTTP_SERVER_DEFERRED_IDLE &&
           salts_monotonic_ms() < deadline)
      salts_thread_yield();
    check_equal(chttp_server_deferred_token_state(
                    atomic_load_explicit(target->token, memory_order_acquire)),
                CHTTP_SERVER_DEFERRED_IDLE);
    check_equal(chttp_server_deferred_reply(&stale, &response), SALTS_ENOENT);

    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("makes a deferred H2 handle stale after peer connection close") {
    static const chttp_h2_hpack_header headers[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/deferred", sizeof("/deferred") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_deferred deferred = {0};
    chttp_server_deferred stale = CHTTP_SERVER_DEFERRED_INIT;
    chttp_server_stats stats = {0};
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    uint64_t deadline;
    uint16_t port = 0u;

    atomic_init(&deferred.admitted, 0);
    config.session_capacity = 0u;
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/deferred", chttp_h2_server_test_deferred_handler, &deferred),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(
        chttp_h2_server_test_peer_submit(&peer, headers, sizeof(headers) / sizeof(headers[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_wait_atomic(&deferred.admitted, 1), SALTS_OK);
    stale = deferred.handle;

    chttp_h2_server_test_socket_close(socket_value);
    socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    deadline = salts_monotonic_ms() + CHTTP_H2_SERVER_TEST_TIMEOUT_MS;
    do {
      check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
      if (stats.active_connections == 0u) break;
      salts_thread_yield();
    } while (salts_monotonic_ms() < deadline);
    check_equal(stats.active_connections, 0u);
    check_equal(chttp_server_deferred_cancel(&stale), SALTS_ENOENT);

    chttp_h2_server_test_peer_destroy(&peer);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("drains an admitted deferred stream after GOAWAY before stopping") {
    static const chttp_h2_hpack_header headers[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/deferred", sizeof("/deferred") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    static const chttp_server_deferred_response response = {
        .size = sizeof(chttp_server_deferred_response),
        .status_code = 202u,
        .content_type = "text/plain",
        .body = "after-goaway",
        .body_size = sizeof("after-goaway") - 1u};
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_deferred deferred = {0};
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    uint16_t port = 0u;
    size_t attempts = 0u;
    int receive_status;

    atomic_init(&deferred.admitted, 0);
    config.session_capacity = 0u;
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/deferred", chttp_h2_server_test_deferred_handler, &deferred),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(
        chttp_h2_server_test_peer_submit(&peer, headers, sizeof(headers) / sizeof(headers[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_wait_atomic(&deferred.admitted, 1), SALTS_OK);

    check_equal(chttp_server_stop(&server, 10u), SALTS_ETIMEDOUT);
    while (peer.goaway_count == 0u && attempts++ < 8u) {
      check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
      check_equal(chttp_h2_server_test_peer_receive_once(&peer, socket_value), SALTS_OK);
    }
    check_equal(peer.goaway_count, 1u);
    check_equal(peer.goaway_error, CHTTP_H2_ERR_NO_ERROR);

    check_equal(chttp_server_deferred_reply(&deferred.handle, &response), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 1u), SALTS_OK);
    check_equal(peer.results[0].error_code, CHTTP_H2_ERR_NO_ERROR);
    check_equal(peer.results[0].status, 202u);
    check_equal(peer.results[0].body, "after-goaway");
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    receive_status = chttp_h2_server_test_peer_receive_once(&peer, socket_value);
    if (receive_status == SALTS_OK)
      check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);

    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("advertises and accepts RFC 8441 WebSocket extended CONNECT") {
    static const chttp_h2_hpack_header headers[] = {
        {":method", sizeof(":method") - 1u, "CONNECT", sizeof("CONNECT") - 1u},
        {":protocol", sizeof(":protocol") - 1u, "websocket", sizeof("websocket") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/ws/42", sizeof("/ws/42") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u},
        {"sec-websocket-version", sizeof("sec-websocket-version") - 1u, "13", 2u}};
    static const uint8_t masked_text[] = {0x81u, 0x82u, 0x01u, 0x02u, 0x03u, 0x04u, 0x69u, 0x6bu};
    static const uint8_t echoed_text[] = {0x81u, 0x02u, 'h', 'i'};
    static const uint8_t masked_close[] = {0x88u, 0x82u, 0x01u, 0x02u, 0x03u, 0x04u, 0x02u, 0xeau};
    static const uint8_t close_reply[] = {0x88u, 0x02u, 0x03u, 0xe8u};
    chttp_h2_websocket_probe probe;
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    int32_t stream_id = 0;
    uint16_t port = 0u;

    atomic_init(&probe.opens, 0);
    atomic_init(&probe.messages, 0);
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(chttp_server_websocket(&server, "/ws/:id", chttp_h2_websocket_open,
                                       chttp_h2_websocket_event, &probe),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);

    check_equal(chttp_h2_server_test_peer_send_once(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_receive_once(&peer, socket_value), SALTS_OK);
    check(chttp_h2_proto_peer_settings_received(peer.protocol));
    check_equal(chttp_h2_proto_peer_enable_connect_protocol(peer.protocol), 1u);

    check_equal(chttp_h2_server_test_peer_submit_tunnel(
                    &peer, headers, sizeof(headers) / sizeof(headers[0]), &stream_id),
                SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_receive(&peer, socket_value, 1u), SALTS_OK);
    check_equal(peer.results[0].status, 200u);
    check(!peer.results[0].closed);
    check_equal(atomic_load_explicit(&probe.opens, memory_order_acquire), 1);

    check_equal(
        chttp_h2_proto_submit_data(peer.protocol, stream_id, masked_text, sizeof(masked_text), 0),
        0);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_receive_once(&peer, socket_value), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.messages, memory_order_acquire), 1);
    check_equal(peer.results[0].body_size, sizeof(echoed_text));
    check_equal(memcmp(peer.results[0].body, echoed_text, sizeof(echoed_text)), 0);

    check_equal(
        chttp_h2_proto_submit_data(peer.protocol, stream_id, masked_close, sizeof(masked_close), 1),
        0);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 1u), SALTS_OK);
    check(peer.results[0].closed);
    check_equal(peer.results[0].error_code, CHTTP_H2_ERR_NO_ERROR);
    check_equal(peer.results[0].body_size, sizeof(echoed_text) + sizeof(close_reply));
    check_equal(
        memcmp(peer.results[0].body + sizeof(echoed_text), close_reply, sizeof(close_reply)), 0);
    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("authenticates RFC 8441 websocket before opening") {
    static const unsigned char key[] = "0123456789abcdef0123456789abcdef";
    static const chttp_h2_hpack_header base_headers[] = {
        {":method", sizeof(":method") - 1u, "CONNECT", sizeof("CONNECT") - 1u},
        {":protocol", sizeof(":protocol") - 1u, "websocket", sizeof("websocket") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/jwt-ws/42", sizeof("/jwt-ws/42") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u},
        {"sec-websocket-version", sizeof("sec-websocket-version") - 1u, "13", 2u}};
    static const uint8_t masked_close[] = {0x88u, 0x82u, 0x01u, 0x02u, 0x03u, 0x04u, 0x02u, 0xeau};
    const chttp_jwt_claims claims = {.subject = "alice", .expires_at = INT64_C(3000000000)};
    const chttp_jwt_bearer_validator_options validator_options = {
        .size = sizeof(validator_options), .key = key, .key_size = sizeof(key) - 1u};
    chttp_h2_hpack_header authorized[sizeof(base_headers) / sizeof(base_headers[0]) + 1u];
    chttp_h2_websocket_probe probe;
    chttp_jwt_bearer_validator validator = {0};
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_server_websocket_options websocket_options = {.size = sizeof(websocket_options),
                                                        .path = "/jwt-ws/:id",
                                                        .on_open = chttp_h2_websocket_jwt_open,
                                                        .on_event = chttp_h2_websocket_event,
                                                        .user = &probe};
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    chttp_header authorization = {0};
    char authorization_storage[512];
    char *token = NULL;
    int32_t anonymous_stream = 0;
    int32_t authorized_stream = 0;
    uint16_t port = 0u;

    atomic_init(&probe.opens, 0);
    atomic_init(&probe.messages, 0);
    check_equal(chttp_jwt_hs256_token_create(&claims, key, sizeof(key) - 1u, &token), SALTS_OK);
    check_equal(chttp_jwt_bearer_header(token, authorization_storage, sizeof(authorization_storage),
                                        &authorization),
                SALTS_OK);
    memcpy(authorized, base_headers, sizeof(base_headers));
    authorized[sizeof(base_headers) / sizeof(base_headers[0])] =
        (chttp_h2_hpack_header){"authorization", sizeof("authorization") - 1u, authorization.value,
                                strlen(authorization.value)};

    check_equal(chttp_jwt_bearer_validator_init(&validator, &validator_options), SALTS_OK);
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(chttp_server_websocket_with_jwt_bearer(&server, &websocket_options, &validator),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send_once(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_receive_once(&peer, socket_value), SALTS_OK);
    check(chttp_h2_proto_peer_settings_received(peer.protocol));
    check_equal(chttp_h2_proto_peer_enable_connect_protocol(peer.protocol), 1u);

    check_equal(
        chttp_h2_server_test_peer_submit_tunnel(
            &peer, base_headers, sizeof(base_headers) / sizeof(base_headers[0]), &anonymous_stream),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_receive(&peer, socket_value, 1u), SALTS_OK);
    check_equal(peer.results[0].stream_id, anonymous_stream);
    check_equal(peer.results[0].status, 401u);
    check_equal(atomic_load_explicit(&probe.opens, memory_order_acquire), 0);

    check_equal(chttp_h2_server_test_peer_submit_tunnel(&peer, authorized,
                                                        sizeof(authorized) / sizeof(authorized[0]),
                                                        &authorized_stream),
                SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_receive(&peer, socket_value, 2u), SALTS_OK);
    check_equal(peer.results[1].stream_id, authorized_stream);
    check_equal(peer.results[1].status, 200u);
    check(!peer.results[1].closed);
    check_equal(atomic_load_explicit(&probe.opens, memory_order_acquire), 1);

    check_equal(chttp_h2_proto_submit_data(peer.protocol, authorized_stream, masked_close,
                                           sizeof(masked_close), 1),
                0);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 1u), SALTS_OK);

    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_jwt_bearer_validator_destroy(&validator), SALTS_OK);
    chttp_jwt_token_destroy(token);
  }

  it("resets a malformed RFC 8441 stream without failing an HTTP sibling") {
    static const chttp_h2_hpack_header malformed[] = {
        {":method", sizeof(":method") - 1u, "CONNECT", sizeof("CONNECT") - 1u},
        {":protocol", sizeof(":protocol") - 1u, "websocket", sizeof("websocket") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/ws/42", sizeof("/ws/42") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u},
        {"sec-websocket-version", sizeof("sec-websocket-version") - 1u, "13", 2u},
        {"sec-websocket-key", sizeof("sec-websocket-key") - 1u, "forbidden", 9u}};
    static const chttp_h2_hpack_header sibling[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/values/good", sizeof("/values/good") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    chttp_h2_websocket_probe probe;
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    int32_t malformed_stream = 0;
    uint16_t port = 0u;

    atomic_init(&probe.opens, 0);
    atomic_init(&probe.messages, 0);
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(chttp_server_websocket(&server, "/ws/:id", chttp_h2_websocket_open,
                                       chttp_h2_websocket_event, &probe),
                SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_submit_tunnel(
                    &peer, malformed, sizeof(malformed) / sizeof(malformed[0]), &malformed_stream),
                SALTS_OK);
    check_equal(
        chttp_h2_server_test_peer_submit(&peer, sibling, sizeof(sibling) / sizeof(sibling[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 2u), SALTS_OK);
    check_equal(peer.results[0].stream_id, malformed_stream);
    check_equal(peer.results[0].error_code, CHTTP_H2_ERR_PROTOCOL_ERROR);
    check_equal(peer.results[1].error_code, CHTTP_H2_ERR_NO_ERROR);
    check_equal(peer.results[1].status, 200u);
    check_equal(peer.results[1].body, "good");
    check_equal(atomic_load_explicit(&probe.opens, memory_order_acquire), 0);

    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("fails fast on partial or contradictory HTTP/2 server limits") {
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();

    config.enable_http2 = 2;
    check_equal(chttp_server_init(&server, &config), SALTS_EINVAL);
    check_null(server.impl);
    config.enable_http2 = 0;
    check_equal(chttp_server_init(&server, &config), SALTS_EINVAL);
    check_null(server.impl);
    config = chttp_h2_server_test_config();
    config.h2_stream_capacity = 0u;
    check_equal(chttp_server_init(&server, &config), SALTS_EMSGSIZE);
    check_null(server.impl);
    config = chttp_h2_server_test_config();
    config.h2_input_buffer_bytes = 16u * 1024u;
    check_equal(chttp_server_init(&server, &config), SALTS_EMSGSIZE);
    check_null(server.impl);
    config = chttp_h2_server_test_config();
    config.h2_output_buffer_bytes = config.network.max_send_bytes + 1u;
    check_equal(chttp_server_init(&server, &config), SALTS_EMSGSIZE);
    check_null(server.impl);
    config = chttp_h2_server_test_config();
    config.h2_output_buffer_bytes = 16u * 1024u;
    check_equal(chttp_server_init(&server, &config), SALTS_EMSGSIZE);
    check_null(server.impl);
    config = chttp_h2_server_test_config();
    config.network.max_send_bytes = 128u * 1024u;
    config.max_response_header_bytes = config.h2_output_buffer_bytes;
    check_equal(chttp_server_init(&server, &config), SALTS_EMSGSIZE);
    check_null(server.impl);
    config = chttp_h2_server_test_config();
    config.stream_chunk_bytes = config.network.max_send_bytes;
    check_equal(chttp_server_init(&server, &config), SALTS_EMSGSIZE);
    check_null(server.impl);
    config = chttp_h2_server_test_config();
    config.max_buffered_response_body_bytes = config.max_response_body_bytes + 1u;
    check_equal(chttp_server_init(&server, &config), SALTS_EINVAL);
    check_null(server.impl);
  }

  it("serves an h2c prior-knowledge request through middleware and routing") {
    const chttp_header headers[] = {{"x-request", "h2c"}};
    chttp_h2_server_test_probe probe = {0};
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_use(&server, chttp_h2_server_test_middleware, &probe), SALTS_OK);
    check_equal(chttp_server_get(&server, "/items/:id", chttp_h2_server_test_handler, &probe),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/items/42?source=test",
                              .headers = headers,
                              .header_count = 1u,
                              .timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.http_major, 2u);
    check_equal(response.http_minor, 0u);
    check_equal(response.status_code, 200u);
    check_equal(chttp_response_header(&response, "x-transport"), "h2c");
    check_equal(response.body, "pong", 4u);
    check_equal(probe.middleware_calls, 1u);
    check_equal(probe.handler_calls, 1u);

    chttp_response_destroy(&response);
    check_equal(chttp_client_destroy(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("uses the same streaming source and sink contract over HTTP/2") {
    static const unsigned char payload[] = "h2-stream";
    chttp_h2_stream_probe probe = {
        .source_data = payload, .source_size = sizeof(payload) - 1u, .source_chunk = 3u};
    const chttp_body_source source = {.read = chttp_h2_stream_read,
                                      .user = &probe,
                                      .content_length = sizeof(payload) - 1u,
                                      .content_length_known = 1};
    const chttp_body_sink sink = {.write = chttp_h2_stream_write, .user = &probe};
    chttp_h2_server_stream_probe server_probe = {0};
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    client_config.stream_chunk_bytes = 3u;
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    {
      const chttp_server_route_options route = {.method = CHTTP_METHOD_POST,
                                                .path = "/echo",
                                                .handler = chttp_h2_server_stream_handler,
                                                .user = &server_probe,
                                                .body_open = chttp_h2_server_stream_open,
                                                .body_close = chttp_h2_server_stream_close};
      check_equal(chttp_server_route_with(&server, &route), SALTS_OK);
    }
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/echo",
                              .body_source = &source,
                              .body_sink = &sink,
                              .timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_post(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_null(response.body);
    check_equal(response.body_size, sizeof(payload) - 1u);
    check_equal(probe.source_offset, sizeof(payload) - 1u);
    check_equal(probe.source_calls, (size_t)4u);
    check_equal(probe.sink_size, sizeof(payload) - 1u);
    check_equal(probe.sink_data, payload, sizeof(payload) - 1u);
    check_greater(probe.sink_calls, (size_t)0u);
    check_equal(server_probe.opens, (size_t)1u);
    check_greater(server_probe.writes, (size_t)0u);
    check_equal(server_probe.closes, (size_t)1u);
    check_equal(server_probe.close_status, SALTS_OK);
    check_equal(server_probe.size, sizeof(payload) - 1u);
    check_equal(server_probe.data, payload, sizeof(payload) - 1u);
    check_equal(server_probe.response_offset, sizeof(payload) - 1u);
    check_greater(server_probe.response_reads, (size_t)1u);

    chttp_response_destroy(&response);
    check_equal(chttp_client_destroy(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("authenticates regular HTTP2 before streaming body admission") {
    static const unsigned char key[] = "0123456789abcdef0123456789abcdef";
    static const unsigned char payload[] = "jwt-h2-body";
    const chttp_jwt_claims claims = {.subject = "alice", .expires_at = INT64_C(3000000000)};
    const chttp_jwt_bearer_validator_options validator_options = {
        .size = sizeof(validator_options), .key = key, .key_size = sizeof(key) - 1u};
    chttp_h2_stream_probe client_probe = {
        .source_data = payload, .source_size = sizeof(payload) - 1u, .source_chunk = 3u};
    const chttp_body_source source = {.read = chttp_h2_stream_read,
                                      .user = &client_probe,
                                      .content_length = sizeof(payload) - 1u,
                                      .content_length_known = 1};
    chttp_h2_server_stream_probe server_probe = {0};
    chttp_jwt_bearer_validator validator = {0};
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    chttp_header authorization = {0};
    char authorization_storage[512];
    char *token = NULL;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    client_config.stream_chunk_bytes = 3u;
    check_equal(chttp_jwt_hs256_token_create(&claims, key, sizeof(key) - 1u, &token), SALTS_OK);
    check_equal(chttp_jwt_bearer_header(token, authorization_storage, sizeof(authorization_storage),
                                        &authorization),
                SALTS_OK);
    check_equal(chttp_jwt_bearer_validator_init(&validator, &validator_options), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    {
      const chttp_server_route_options route = {.method = CHTTP_METHOD_POST,
                                                .path = "/jwt-echo",
                                                .handler = chttp_h2_server_jwt_stream_handler,
                                                .user = &server_probe,
                                                .body_open = chttp_h2_server_jwt_stream_open,
                                                .body_close = chttp_h2_server_stream_close};
      check_equal(chttp_server_route_with_jwt_bearer(&server, &route, &validator), SALTS_OK);
    }
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/jwt-echo",
                              .body_source = &source,
                              .timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_post(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, 401u);
    check_equal(server_probe.opens, (size_t)0u);
    check_equal(server_probe.writes, (size_t)0u);
    check_equal(server_probe.handler_calls, (size_t)0u);
    chttp_response_destroy(&response);

    client_probe.source_offset = 0u;
    client_probe.source_calls = 0u;
    response = (chttp_response){0};
    error = (chttp_error){0};
    options.headers = &authorization;
    options.header_count = 1u;
    check_equal(chttp_post(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_equal(response.body, "ok", 2u);
    check_equal(server_probe.opens, (size_t)1u);
    check_greater(server_probe.writes, (size_t)0u);
    check_equal(server_probe.closes, (size_t)1u);
    check_equal(server_probe.close_status, SALTS_OK);
    check_equal(server_probe.handler_calls, (size_t)1u);
    check_equal(server_probe.size, sizeof(payload) - 1u);
    check_equal(server_probe.data, payload, sizeof(payload) - 1u);

    chttp_response_destroy(&response);
    check_equal(chttp_client_destroy(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_jwt_bearer_validator_destroy(&validator), SALTS_OK);
    chttp_jwt_token_destroy(token);
  }

  it("resets a failing response sink without failing an HTTP/2 sibling") {
    chttp_server server = {0};
    chttp_async_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_h2_stream_completion failed = {0};
    chttp_h2_stream_completion good = {0};
    const chttp_body_sink failing_sink = {.write = chttp_h2_stream_fail_write};
    chttp_request failed_request = {0};
    chttp_request good_request = {0};
    chttp_request_options options;
    chttp_server_stats stats = {0};
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    uint64_t deadline;

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_async_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/values/bad",
                                      .method = CHTTP_METHOD_GET,
                                      .body_sink = &failing_sink,
                                      .on_complete = chttp_h2_stream_complete,
                                      .user = &failed,
                                      .protocol = CHTTP_HTTP_2};
    check_equal(chttp_async_client_submit(&client, &options, &failed_request), SALTS_OK);
    options.target = "/values/good";
    options.body_sink = NULL;
    options.user = &good;
    check_equal(chttp_async_client_submit(&client, &options, &good_request), SALTS_OK);

    deadline = salts_monotonic_ms() + CHTTP_H2_SERVER_TEST_TIMEOUT_MS;
    while ((failed.calls == 0u || good.calls == 0u) && salts_monotonic_ms() < deadline) {
      size_t completions = 0u;
      check_equal(chttp_async_client_poll(&client, 20u, &completions), SALTS_OK);
    }
    check_equal(failed.calls, (size_t)1u);
    check_equal(failed.status, SALTS_EIO);
    check_equal(good.calls, (size_t)1u);
    check_equal(good.status, SALTS_OK);
    check_equal(good.response_status, 200u);
    check_equal(good.body, "good", 4u);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)1u);

    check_equal(chttp_async_client_stop(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("resets a failing server response source without failing an HTTP/2 sibling") {
    chttp_server server = {0};
    chttp_async_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_h2_server_test_completion failed = {0};
    chttp_h2_server_test_completion sibling = {0};
    chttp_request failed_request = {0};
    chttp_request sibling_request = {0};
    chttp_request_options options;
    chttp_server_stats stats = {0};
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t polls = 0u;

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/fail-response", chttp_h2_server_failing_response_handler, NULL),
        SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_async_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/fail-response",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_server_test_complete,
                                      .user = &failed,
                                      .protocol = CHTTP_HTTP_2};
    check_equal(chttp_async_client_submit(&client, &options, &failed_request), SALTS_OK);
    options.target = "/values/sibling";
    options.user = &sibling;
    check_equal(chttp_async_client_submit(&client, &options, &sibling_request), SALTS_OK);
    while ((failed.calls == 0u || sibling.calls == 0u) && polls++ < 40u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);

    check_equal(failed.calls, (size_t)1u);
    check_equal(failed.status, SALTS_ECANCELED);
    check_equal(sibling.calls, (size_t)1u);
    check_equal(sibling.status, SALTS_OK);
    check_equal(sibling.response_status, 200u);
    check_equal(sibling.body, "sibling");
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)1u);

    check_equal(chttp_async_client_stop(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("uploads and downloads files over one HTTP/2 connection") {
    static const unsigned char payload[] = "h2-file-payload";
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    chttp_server_stats stats = {0};
    char uri[64];
    char authority[64];
    char *upload_path = tt_make_temp_file("chttp-h2-upload", ".bin");
    char *download_path = tt_make_temp_file("chttp-h2-download", ".bin");
    char *downloaded = NULL;
    size_t downloaded_size = 0u;
    uint16_t port = 0u;

    check_not_null(upload_path);
    check_not_null(download_path);
    check_equal(tt_write_file(upload_path, payload, sizeof(payload) - 1u), 0);
    check_equal(tt_write_file(download_path, "old", sizeof("old") - 1u), 0);
    client_config.stream_chunk_bytes = 4u;
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_put(&server, "/file", chttp_h2_server_test_body_handler, NULL),
                SALTS_OK);
    check_equal(chttp_server_get(&server, "/file", chttp_h2_server_test_file_handler, upload_path),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/file",
                              .timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_put_file(&client, &options, upload_path, NULL, NULL, &response, &error),
                SALTS_OK);
    check_equal(response.status_code, 200u);
    check_equal(response.body, payload, sizeof(payload) - 1u);
    chttp_response_destroy(&response);
    check_equal(
        chttp_download_file(&client, &options, download_path, NULL, NULL, &response, &error),
        SALTS_OK);
    check_equal(response.status_code, 200u);
    check_null(response.body);
    check_equal(response.body_size, sizeof(payload) - 1u);
    downloaded = tt_read_file(download_path, &downloaded_size);
    check_not_null(downloaded);
    check_equal(downloaded_size, sizeof(payload) - 1u);
    check_equal(downloaded, payload, sizeof(payload) - 1u);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)1u);

    free(downloaded);
    chttp_response_destroy(&response);
    check_equal(chttp_client_destroy(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(tt_remove_file(upload_path), 0);
    check_equal(tt_remove_file(download_path), 0);
    free(upload_path);
    free(download_path);
  }

  it("keeps cleartext HTTP/1.1 working when HTTP/2 is enabled") {
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_get(&server, "/version", chttp_h2_server_test_version_handler, NULL),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/version",
                              .timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_1_1};

    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.http_major, 1u);
    check_equal(chttp_response_header(&response, "x-protocol"), "h1");
    check_equal(response.body, "h1", 2u);
    chttp_response_destroy(&response);

    check_equal(chttp_client_destroy(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("continues an HTTP/2 response after the peer replenishes flow-control credit") {
    enum { LARGE_BODY_BYTES = 96 * 1024 };
    static unsigned char large_body[LARGE_BODY_BYTES];
    const chttp_h2_server_test_payload payload = {large_body, sizeof(large_body)};
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t index;

    for (index = 0u; index < sizeof(large_body); ++index)
      large_body[index] = (unsigned char)(index % 251u);
    server_config.network.max_send_bytes = 128u * 1024u;
    server_config.max_response_body_bytes = sizeof(large_body);
    client_config.max_response_body_bytes = sizeof(large_body);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/large", chttp_h2_server_test_payload_handler, (void *)&payload),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/large",
                              .timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_equal(response.body_size, sizeof(large_body));
    check_equal(response.body, large_body, sizeof(large_body));
    chttp_response_destroy(&response);

    check_equal(chttp_client_destroy(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("drains an admitted flow-controlled response before stop completes") {
    enum { LARGE_BODY_BYTES = 96 * 1024 };
    static unsigned char large_body[LARGE_BODY_BYTES];
    static const chttp_h2_hpack_header headers[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/large", sizeof("/large") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u},
        {"content-length", sizeof("content-length") - 1u, "0", 1u}};
    atomic_int handler_called;
    chttp_h2_server_test_payload payload = {large_body, sizeof(large_body), &handler_called};
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_stop stop = {&server, SALTS_EBUSY};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    salts_thread_t stop_thread = NULL;
    uint64_t deadline;
    uint16_t port = 0u;
    size_t index;
    int pump_status;

    atomic_init(&handler_called, 0);
    for (index = 0u; index < sizeof(large_body); ++index)
      large_body[index] = (unsigned char)(index % 251u);
    config.network.max_send_bytes = 128u * 1024u;
    config.max_response_body_bytes = sizeof(large_body);
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(chttp_server_get(&server, "/large", chttp_h2_server_test_payload_handler, &payload),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(
        chttp_h2_server_test_peer_submit(&peer, headers, sizeof(headers) / sizeof(headers[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    deadline = salts_monotonic_ms() + CHTTP_H2_SERVER_TEST_TIMEOUT_MS;
    while (atomic_load_explicit(&handler_called, memory_order_acquire) == 0 &&
           salts_monotonic_ms() < deadline)
      salts_thread_yield();
    check_equal(atomic_load_explicit(&handler_called, memory_order_acquire), 1);

    check_equal(salts_thread_create(&stop_thread, chttp_h2_server_test_stop_thread, &stop),
                SALTS_OK);
    pump_status = chttp_h2_server_test_peer_pump(&peer, socket_value, 1u);
    check_equal(pump_status, SALTS_OK);
    check_equal(salts_thread_join(&stop_thread), SALTS_OK);
    salts_thread_destroy(&stop_thread);
    check_equal(chttp_h2_server_test_peer_read_to_close(&peer, socket_value), SALTS_OK);
    check_equal(stop.status, SALTS_OK);
    check_equal(peer.results[0].error_code, CHTTP_H2_ERR_NO_ERROR);
    check_equal(peer.results[0].status, 200u);
    check_equal(peer.results[0].received_body_size, sizeof(large_body));
    check_equal(peer.results[0].body, large_body, sizeof(peer.results[0].body) - 1u);
    check_equal(peer.goaway_count, 1u);
    check_equal(peer.goaway_error, CHTTP_H2_ERR_NO_ERROR);

    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("accepts pseudo-headers in any valid order") {
    static const chttp_h2_hpack_header headers[] = {
        {":path", sizeof(":path") - 1u, "/values/ordered", sizeof("/values/ordered") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {"content-length", sizeof("content-length") - 1u, "0", 1u}};
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    uint16_t port = 0u;

    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(
        chttp_h2_server_test_peer_submit(&peer, headers, sizeof(headers) / sizeof(headers[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 1u), SALTS_OK);
    check_equal(peer.results[0].error_code, CHTTP_H2_ERR_NO_ERROR);
    check_equal(peer.results[0].status, 200u);
    check_equal(peer.results[0].body, "ordered");

    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("isolates a malformed pseudo-header stream from a valid sibling") {
    static const chttp_h2_hpack_header uppercase[] = {
        {":Method", sizeof(":Method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/values/uppercase", sizeof("/values/uppercase") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    static const chttp_h2_hpack_header malformed[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/values/bad", sizeof("/values/bad") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u},
        {"content-length", sizeof("content-length") - 1u, "0", 1u}};
    static const chttp_h2_hpack_header sibling[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/values/good", sizeof("/values/good") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u},
        {"content-length", sizeof("content-length") - 1u, "0", 1u}};
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    uint16_t port = 0u;

    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_submit(&peer, uppercase,
                                                 sizeof(uppercase) / sizeof(uppercase[0])),
                SALTS_OK);
    check_equal(chttp_h2_server_test_peer_submit(&peer, malformed,
                                                 sizeof(malformed) / sizeof(malformed[0])),
                SALTS_OK);
    check_equal(
        chttp_h2_server_test_peer_submit(&peer, sibling, sizeof(sibling) / sizeof(sibling[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 3u), SALTS_OK);
    check_equal(peer.results[0].error_code, CHTTP_H2_ERR_PROTOCOL_ERROR);
    check_equal(peer.results[1].error_code, CHTTP_H2_ERR_PROTOCOL_ERROR);
    check_equal(peer.results[2].error_code, CHTTP_H2_ERR_NO_ERROR);
    check_equal(peer.results[2].status, 200u);
    check_equal(peer.results[2].body, "good");

    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("rejects non-empty request trailers without failing a sibling") {
    static const chttp_h2_hpack_header initial[] = {
        {":method", sizeof(":method") - 1u, "POST", sizeof("POST") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/echo", sizeof("/echo") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u},
        {"content-length", sizeof("content-length") - 1u, "1", 1u}};
    static const chttp_h2_hpack_header trailers[] = {{"authorization", sizeof("authorization") - 1u,
                                                      "trailer-secret",
                                                      sizeof("trailer-secret") - 1u}};
    static const chttp_h2_hpack_header sibling[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/values/good", sizeof("/values/good") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    chttp_h2_server_test_source source = {0};
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    int32_t stream_id = 0;
    uint16_t port = 0u;

    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(chttp_server_post(&server, "/echo", chttp_h2_server_test_body_handler, NULL),
                SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_submit_open(
                    &peer, initial, sizeof(initial) / sizeof(initial[0]), &source, &stream_id),
                SALTS_OK);
    check_equal(chttp_h2_server_test_peer_limit_stream_window(&peer, 1u), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send_once(&peer, socket_value), SALTS_OK);
    check_equal(source.calls, 1u);
    check_equal(chttp_h2_server_test_send_header_block(
                    socket_value, stream_id, trailers, sizeof(trailers) / sizeof(trailers[0]),
                    CHTTP_H2_FLAG_END_HEADERS | CHTTP_H2_FLAG_END_STREAM),
                SALTS_OK);
    check_equal(chttp_h2_server_test_peer_receive(&peer, socket_value, 1u), SALTS_OK);
    check_equal(peer.results[0].error_code, CHTTP_H2_ERR_PROTOCOL_ERROR);

    check_equal(
        chttp_h2_server_test_peer_submit(&peer, sibling, sizeof(sibling) / sizeof(sibling[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 2u), SALTS_OK);
    check_equal(peer.results[1].error_code, CHTTP_H2_ERR_NO_ERROR);
    check_equal(peer.results[1].status, 200u);
    check_equal(peer.results[1].body, "good");

    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("isolates a content-length mismatch from a valid sibling") {
    static const chttp_h2_hpack_header malformed[] = {
        {":method", sizeof(":method") - 1u, "POST", sizeof("POST") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/echo", sizeof("/echo") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u},
        {"content-length", sizeof("content-length") - 1u, "1", 1u}};
    static const chttp_h2_hpack_header sibling[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/values/good", sizeof("/values/good") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u}};
    static const char body[] = "xx";
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    uint16_t port = 0u;

    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(chttp_server_post(&server, "/echo", chttp_h2_server_test_body_handler, NULL),
                SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_submit_body(&peer, malformed,
                                                      sizeof(malformed) / sizeof(malformed[0]),
                                                      body, sizeof(body) - 1u),
                SALTS_OK);
    check_equal(chttp_h2_server_test_peer_send(&peer, socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_receive(&peer, socket_value, 1u), SALTS_OK);
    check_equal(peer.results[0].error_code, CHTTP_H2_ERR_PROTOCOL_ERROR);

    check_equal(
        chttp_h2_server_test_peer_submit(&peer, sibling, sizeof(sibling) / sizeof(sibling[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 2u), SALTS_OK);
    check_equal(peer.results[1].error_code, CHTTP_H2_ERR_NO_ERROR);
    check_equal(peer.results[1].status, 200u);
    check_equal(peer.results[1].body, "good");

    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("sends GOAWAY before closing an idle HTTP/2 connection on stop") {
    static const chttp_h2_hpack_header headers[] = {
        {":method", sizeof(":method") - 1u, "GET", sizeof("GET") - 1u},
        {":scheme", sizeof(":scheme") - 1u, "http", sizeof("http") - 1u},
        {":path", sizeof(":path") - 1u, "/values/stop", sizeof("/values/stop") - 1u},
        {":authority", sizeof(":authority") - 1u, "localhost", sizeof("localhost") - 1u},
        {"content-length", sizeof("content-length") - 1u, "0", 1u}};
    chttp_server server = {0};
    chttp_server_config config = chttp_h2_server_test_config();
    chttp_h2_server_test_peer peer = {0};
    chttp_h2_server_test_socket socket_value = CHTTP_H2_SERVER_TEST_INVALID_SOCKET;
    uint16_t port = 0u;

    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_socket_connect(port, &socket_value), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_init(&peer), SALTS_OK);
    check_equal(
        chttp_h2_server_test_peer_submit(&peer, headers, sizeof(headers) / sizeof(headers[0])),
        SALTS_OK);
    check_equal(chttp_h2_server_test_peer_pump(&peer, socket_value, 1u), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_h2_server_test_peer_read_to_close(&peer, socket_value), SALTS_OK);
    check_equal(peer.goaway_count, 1u);
    check_equal(peer.goaway_error, CHTTP_H2_ERR_NO_ERROR);

    chttp_h2_server_test_peer_destroy(&peer);
    chttp_h2_server_test_socket_close(socket_value);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("multiplexes two targets on one accepted connection") {
    chttp_server server = {0};
    chttp_async_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_h2_server_test_completion first = {0};
    chttp_h2_server_test_completion second = {0};
    chttp_request first_request = {0};
    chttp_request second_request = {0};
    chttp_request_options options;
    chttp_server_stats stats = {0};
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t polls = 0u;

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_async_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/values/first",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_server_test_complete,
                                      .user = &first,
                                      .protocol = CHTTP_HTTP_2};
    check_equal(chttp_async_client_submit(&client, &options, &first_request), SALTS_OK);
    options.target = "/values/second";
    options.user = &second;
    check_equal(chttp_async_client_submit(&client, &options, &second_request), SALTS_OK);
    while ((first.calls == 0u || second.calls == 0u) && polls++ < 40u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);

    check_equal(first.calls, 1u);
    check_equal(first.status, SALTS_OK);
    check_equal(first.response_status, 200u);
    check_equal(first.body, "first");
    check_equal(second.calls, 1u);
    check_equal(second.status, SALTS_OK);
    check_equal(second.response_status, 200u);
    check_equal(second.body, "second");
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, 1u);
    check_equal(stats.requests, 2u);
    check_equal(stats.responses, 2u);

    check_equal(chttp_async_client_stop(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("resets a failing server request sink without failing an HTTP/2 sibling") {
    static const char upload[] = "fail-me";
    chttp_h2_server_stream_probe server_probe = {0};
    chttp_server server = {0};
    chttp_async_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_h2_server_test_completion failed = {0};
    chttp_h2_server_test_completion sibling = {0};
    chttp_request failed_request = {0};
    chttp_request sibling_request = {0};
    chttp_request_options options;
    chttp_server_stats stats = {0};
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t polls = 0u;

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    {
      const chttp_server_route_options route = {.method = CHTTP_METHOD_POST,
                                                .path = "/fail-upload",
                                                .handler = chttp_h2_server_stream_handler,
                                                .user = &server_probe,
                                                .body_open = chttp_h2_server_failing_stream_open,
                                                .body_close = chttp_h2_server_stream_close};
      check_equal(chttp_server_route_with(&server, &route), SALTS_OK);
    }
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_async_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/fail-upload",
                                      .method = CHTTP_METHOD_POST,
                                      .body = upload,
                                      .body_size = sizeof(upload) - 1u,
                                      .on_complete = chttp_h2_server_test_complete,
                                      .user = &failed,
                                      .protocol = CHTTP_HTTP_2};
    check_equal(chttp_async_client_submit(&client, &options, &failed_request), SALTS_OK);
    options.target = "/values/sibling";
    options.method = CHTTP_METHOD_GET;
    options.body = NULL;
    options.body_size = 0u;
    options.user = &sibling;
    check_equal(chttp_async_client_submit(&client, &options, &sibling_request), SALTS_OK);
    while ((failed.calls == 0u || sibling.calls == 0u) && polls++ < 40u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);

    check_equal(failed.calls, 1u);
    check_equal(failed.status, SALTS_EPROTO);
    check_equal(sibling.calls, 1u);
    check_equal(sibling.status, SALTS_OK);
    check_equal(sibling.response_status, 200u);
    check_equal(sibling.body, "sibling");
    check_equal(server_probe.opens, (size_t)1u);
    check_equal(server_probe.closes, (size_t)1u);
    check_equal(server_probe.close_status, SALTS_EIO);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)1u);
    check_equal(stats.requests, (uint64_t)2u);

    check_equal(chttp_async_client_stop(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("resets an oversized body stream without failing its sibling") {
    static unsigned char oversized[20u * 1024u];
    chttp_server server = {0};
    chttp_async_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_h2_server_test_completion rejected = {0};
    chttp_h2_server_test_completion sibling = {0};
    chttp_request rejected_request = {0};
    chttp_request sibling_request = {0};
    chttp_request_options options;
    chttp_server_stats stats = {0};
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t polls = 0u;

    server_config.max_request_body_bytes = 4u;
    server_config.h2_stream_capacity = 2u;
    client_config.max_request_body_bytes = sizeof(oversized);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_post(&server, "/echo", chttp_h2_server_test_body_handler, NULL),
                SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/values/:value", chttp_h2_server_test_value_handler, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_async_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/echo",
                                      .method = CHTTP_METHOD_POST,
                                      .body = oversized,
                                      .body_size = sizeof(oversized),
                                      .on_complete = chttp_h2_server_test_complete,
                                      .user = &rejected,
                                      .protocol = CHTTP_HTTP_2};
    check_equal(chttp_async_client_submit(&client, &options, &rejected_request), SALTS_OK);
    options.target = "/values/sibling";
    options.method = CHTTP_METHOD_GET;
    options.body = NULL;
    options.body_size = 0u;
    options.user = &sibling;
    check_equal(chttp_async_client_submit(&client, &options, &sibling_request), SALTS_OK);
    while ((rejected.calls == 0u || sibling.calls == 0u) && polls++ < 40u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);

    check_equal(rejected.calls, 1u);
    check_equal(rejected.status, SALTS_EPROTO);
    check_equal(sibling.calls, 1u);
    check_equal(sibling.status, SALTS_OK);
    check_equal(sibling.response_status, 200u);
    check_equal(sibling.body, "sibling");

    rejected = (chttp_h2_server_test_completion){0};
    options.target = "/echo";
    options.method = CHTTP_METHOD_POST;
    options.body = oversized;
    options.body_size = sizeof(oversized);
    options.user = &rejected;
    check_equal(chttp_async_client_submit(&client, &options, &rejected_request), SALTS_OK);
    polls = 0u;
    while (rejected.calls == 0u && polls++ < 40u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);
    check_equal(rejected.calls, 1u);
    check_equal(rejected.status, SALTS_EPROTO);

    sibling = (chttp_h2_server_test_completion){0};
    options.target = "/values/reused";
    options.method = CHTTP_METHOD_GET;
    options.body = NULL;
    options.body_size = 0u;
    options.user = &sibling;
    check_equal(chttp_async_client_submit(&client, &options, &sibling_request), SALTS_OK);
    polls = 0u;
    while (sibling.calls == 0u && polls++ < 40u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);
    check_equal(sibling.calls, 1u);
    check_equal(sibling.status, SALTS_OK);
    check_equal(sibling.response_status, 200u);
    check_equal(sibling.body, "reused");

    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, 1u);
    check_equal(stats.requests, 4u);
    check_equal(stats.responses, 2u);

    check_equal(chttp_async_client_stop(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("handles HEAD, request bodies, not-found and method-not-allowed responses") {
    static const char payload[] = "request-body";
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_get(&server, "/head", chttp_h2_server_test_head_handler, NULL),
                SALTS_OK);
    check_equal(chttp_server_post(&server, "/echo", chttp_h2_server_test_body_handler, NULL),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/head",
                              .timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_head(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_equal(response.body_size, 0u);
    check_equal(chttp_response_header(&response, "content-length"), "9");
    chttp_response_destroy(&response);
    options.target = "/echo";
    options.body = payload;
    options.body_size = sizeof(payload) - 1u;
    check_equal(chttp_post(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, 200u);
    check_equal(response.body, payload, sizeof(payload) - 1u);
    chttp_response_destroy(&response);
    options.target = "/missing";
    options.body = NULL;
    options.body_size = 0u;
    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, 404u);
    chttp_response_destroy(&response);
    options.target = "/echo";
    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, 405u);
    check_equal(chttp_response_header(&response, "allow"), "POST");
    chttp_response_destroy(&response);

    check_equal(chttp_client_destroy(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("keeps Session data through an HTTP/2 cookie round trip") {
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    chttp_header cookie_header = {"cookie", NULL};
    char cookie[256];
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    const char *set_cookie;
    const char *semicolon;
    size_t cookie_size;

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_get(&server, "/session", chttp_h2_server_test_session_handler, NULL),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_equal(chttp_h2_server_test_endpoint(port, uri, sizeof(uri), authority, sizeof(authority)),
                SALTS_OK);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/session",
                              .timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.body, "created", 7u);
    set_cookie = chttp_response_header(&response, "set-cookie");
    check_not_null(set_cookie);
    semicolon = strchr(set_cookie, ';');
    cookie_size = semicolon == NULL ? strlen(set_cookie) : (size_t)(semicolon - set_cookie);
    check_less(cookie_size, sizeof(cookie));
    memcpy(cookie, set_cookie, cookie_size);
    cookie[cookie_size] = '\0';
    chttp_response_destroy(&response);
    cookie_header.value = cookie;
    options.headers = &cookie_header;
    options.header_count = 1u;
    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.body, "persisted", 9u);
    chttp_response_destroy(&response);

    check_equal(chttp_client_destroy(&client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("selects HTTP/2 and HTTP/1.1 on one TLS server through ALPN") {
    static const char *server_alpn[] = {"h2", "http/1.1"};
    static const char *h2_alpn[] = {"h2"};
    static const char *h1_alpn[] = {"http/1.1"};
    char *cert_path = tt_make_temp_file("chttp-h2-server-cert", ".pem");
    char *key_path = tt_make_temp_file("chttp-h2-server-key", ".pem");
    chttp_server server = {0};
    chttp_client h2_client = {0};
    chttp_client h1_client = {0};
    chttp_tls_profile h2_profile = {0};
    chttp_tls_profile h1_profile = {0};
    chttp_server_config server_config = chttp_h2_server_test_config();
    chttp_client_config client_config = chttp_h2_server_test_client_config();
    cnet_tls_server_config server_tls;
    cnet_tls_client_config h2_tls;
    cnet_tls_client_config h1_tls;
    chttp_options options;
    chttp_response response = {0};
    chttp_error error = {0};
    char uri[64];
    uint16_t port = 0u;

    check_not_null(cert_path);
    check_not_null(key_path);
    check_equal(tt_write_file(cert_path, CHTTP_TLS_TEST_CERTIFICATE,
                              sizeof(CHTTP_TLS_TEST_CERTIFICATE) - 1u),
                0);
    check_equal(tt_write_file(key_path, CHTTP_TLS_TEST_KEY, sizeof(CHTTP_TLS_TEST_KEY) - 1u), 0);
    server_config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    server_config.network.tls_handshake_timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS;
    client_config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    client_config.network.tls_handshake_timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS;
    server_tls = (cnet_tls_server_config){.size = sizeof(server_tls),
                                          .cert_file = cert_path,
                                          .key_file = key_path,
                                          .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
                                          .alpn_protocols = server_alpn,
                                          .alpn_protocol_count = 2u};
    h2_tls = (cnet_tls_client_config){.size = sizeof(h2_tls),
                                      .ca_file = cert_path,
                                      .server_name = "localhost",
                                      .alpn_protocols = h2_alpn,
                                      .alpn_protocol_count = 1u};
    h1_tls = h2_tls;
    h1_tls.alpn_protocols = h1_alpn;
    server_config.tls = &server_tls;

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_get(&server, "/version", chttp_h2_server_test_version_handler, NULL),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(chttp_tls_profile_init(&h2_profile, &h2_tls), SALTS_OK);
    check_equal(chttp_tls_profile_init(&h1_profile, &h1_tls), SALTS_OK);
    check_equal(chttp_client_init(&h2_client, &client_config), SALTS_OK);
    check_equal(chttp_client_init(&h1_client, &client_config), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = "localhost",
                              .target = "/version",
                              .timeout_ms = CHTTP_H2_SERVER_TEST_TIMEOUT_MS,
                              .tls = &h2_profile,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_get(&h2_client, &options, &response, &error), SALTS_OK);
    check_equal(response.http_major, 2u);
    check_equal(chttp_response_header(&response, "x-protocol"), "h2");
    check_equal(response.body, "h2", 2u);
    chttp_response_destroy(&response);
    options.tls = &h1_profile;
    options.protocol = CHTTP_HTTP_1_1;
    check_equal(chttp_get(&h1_client, &options, &response, &error), SALTS_OK);
    check_equal(response.http_major, 1u);
    check_equal(chttp_response_header(&response, "x-protocol"), "h1");
    check_equal(response.body, "h1", 2u);
    chttp_response_destroy(&response);

    check_equal(chttp_tls_profile_destroy(&h2_profile), SALTS_OK);
    check_equal(chttp_tls_profile_destroy(&h1_profile), SALTS_OK);
    check_equal(chttp_client_destroy(&h2_client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_client_destroy(&h1_client, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, CHTTP_H2_SERVER_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(tt_remove_file(cert_path), 0);
    check_equal(tt_remove_file(key_path), 0);
    free(cert_path);
    free(key_path);
  }
}
