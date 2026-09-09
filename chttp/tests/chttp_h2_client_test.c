#include "chttp_h2_proto.h"
#include "chttp_tls_test_material.h"
#include "tinytest.h"

#include <chttp/chttp.h>
#include <salts/clock.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET chttp_h2_test_socket;
  #define CHTTP_H2_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int chttp_h2_test_socket;
  #define CHTTP_H2_TEST_INVALID_SOCKET (-1)
#endif

enum { CHTTP_H2_TEST_TIMEOUT_MS = 5000 };
enum { CHTTP_H2_TEST_DEADLINE_MS = 20 };

typedef struct chttp_h2_test_server {
  chttp_h2_test_socket listener;
  chttp_h2_proto *protocol;
  char path[64];
  char paths[4][64];
  size_t expected_requests;
  size_t goaway_responses;
  size_t request_count;
  size_t accepted_connections;
  int empty_responses;
  int oversize_first_response;
  int invalid_content_length;
  int invalid_header_name;
  int invalid_first_header;
  int invalid_trailers;
  int parse_until_close;
  int received_goaway;
  int status;
} chttp_h2_test_server;

typedef struct chttp_h2_test_completion {
  size_t count;
  int statuses[4];
  unsigned int response_statuses[4];
  char bodies[4][16];
} chttp_h2_test_completion;

typedef struct chttp_h2_tls_server {
  cnet_client network;
  cnet_listener listener;
  cnet_tls_server tls;
  cnet_connection connection;
  chttp_h2_proto *protocol;
  int status;
  int connected;
  int response_submitted;
  int send_active;
  int terminal;
} chttp_h2_tls_server;

typedef struct chttp_h2_pool_server {
  chttp_h2_test_socket listener;
  atomic_int first_settings_acked;
  char paths[2][64];
  size_t request_count;
  size_t accepted_connections;
  int status;
} chttp_h2_pool_server;

typedef struct chttp_h2_pool_peer {
  chttp_h2_pool_server *server;
  chttp_h2_test_socket socket_value;
  chttp_h2_proto *protocol;
  int32_t stream_id;
  size_t index;
} chttp_h2_pool_peer;

static native_io_backend_kind chttp_h2_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_client_config chttp_h2_test_config(void) {
  const chttp_client_config config = {.network = {.backend = chttp_h2_test_backend(),
                                                  .connection_capacity = 4u,
                                                  .command_capacity = 16u,
                                                  .request_capacity = 8u,
                                                  .completion_batch_capacity = 4u,
                                                  .event_capacity = 16u,
                                                  .max_send_bytes = 64u * 1024u,
                                                  .receive_buffer_bytes = 4096u,
                                                  .connect_timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS,
                                                  .read_timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS,
                                                  .write_timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS},
                                      .request_capacity = 4u,
                                      .max_start_line_bytes = 256u,
                                      .max_header_count = 16u,
                                      .max_header_bytes = 4096u,
                                      .max_request_body_bytes = 4096u,
                                      .max_response_body_bytes = 4096u,
                                      .max_informational_responses = 2u,
                                      .h2_input_buffer_bytes = 64u * 1024u,
                                      .h2_hpack_dynamic_table_bytes = 2048u,
                                      .h2_max_settings_count = 16u};
  return config;
}

static int chttp_h2_test_source_read(void *user, void *buffer, size_t capacity, size_t *out_size) {
  (void)user;
  (void)buffer;
  (void)capacity;
  if (out_size == NULL) return SALTS_EINVAL;
  *out_size = 0u;
  return SALTS_OK;
}

static void chttp_h2_test_close_socket(chttp_h2_test_socket socket_value) {
  if (socket_value == CHTTP_H2_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int chttp_h2_test_listener(chttp_h2_test_socket *out_listener, uint16_t *out_port) {
  struct sockaddr_in address;
#if defined(_WIN32)
  int length = (int)sizeof(address);
#else
  socklen_t length = (socklen_t)sizeof(address);
#endif
  if (out_listener == NULL || out_port == NULL) return SALTS_EINVAL;
  *out_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (*out_listener == CHTTP_H2_TEST_INVALID_SOCKET) return SALTS_EIO;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(*out_listener, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      getsockname(*out_listener, (struct sockaddr *)&address, &length) != 0 ||
      listen(*out_listener, 2) != 0) {
    chttp_h2_test_close_socket(*out_listener);
    *out_listener = CHTTP_H2_TEST_INVALID_SOCKET;
    return SALTS_EIO;
  }
  *out_port = ntohs(address.sin_port);
  return SALTS_OK;
}

static chttp_h2_test_socket chttp_h2_test_connect_port(uint16_t port) {
  struct sockaddr_in address;
  chttp_h2_test_socket socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_value == CHTTP_H2_TEST_INVALID_SOCKET) return socket_value;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (connect(socket_value, (const struct sockaddr *)&address, (int)sizeof(address)) != 0) {
    chttp_h2_test_close_socket(socket_value);
    return CHTTP_H2_TEST_INVALID_SOCKET;
  }
  return socket_value;
}

static int chttp_h2_test_set_timeout(chttp_h2_test_socket socket_value) {
#if defined(_WIN32)
  const DWORD timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                    (int)sizeof(timeout_ms)) == 0
             ? SALTS_OK
             : SALTS_EIO;
#else
  const struct timeval timeout = {CHTTP_H2_TEST_TIMEOUT_MS / 1000,
                                  (CHTTP_H2_TEST_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout)) ==
                 0
             ? SALTS_OK
             : SALTS_EIO;
#endif
}

static int chttp_h2_test_send_all(chttp_h2_test_socket socket_value, const void *data,
                                  size_t size) {
  size_t offset = 0u;
  while (offset < size) {
    const int sent = send(socket_value, (const char *)data + offset, (int)(size - offset), 0);
    if (sent <= 0) return SALTS_EIO;
    offset += (size_t)sent;
  }
  return SALTS_OK;
}

static int chttp_h2_test_on_begin(void *user, int32_t stream_id) {
  chttp_h2_test_server *server = (chttp_h2_test_server *)user;
  (void)stream_id;
  server->path[0] = '\0';
  return 0;
}

static int chttp_h2_test_on_header(void *user, int32_t stream_id, const char *name,
                                   size_t name_size, const char *value, size_t value_size) {
  chttp_h2_test_server *server = (chttp_h2_test_server *)user;
  (void)stream_id;
  if (name_size == sizeof(":path") - 1u && memcmp(name, ":path", name_size) == 0 &&
      value_size < sizeof(server->path)) {
    memcpy(server->path, value, value_size);
    server->path[value_size] = '\0';
  }
  return 0;
}

static int chttp_h2_test_on_end(void *user, int32_t stream_id, int end_stream) {
  static const chttp_h2_hpack_header response_headers[] = {
      {":status", sizeof(":status") - 1u, "200", sizeof("200") - 1u},
      {"content-length", sizeof("content-length") - 1u, "2", sizeof("2") - 1u},
      {"x-protocol", sizeof("x-protocol") - 1u, "h2", sizeof("h2") - 1u}};
  static const chttp_h2_hpack_header empty_response_headers[] = {
      {":status", sizeof(":status") - 1u, "200", sizeof("200") - 1u},
      {"content-length", sizeof("content-length") - 1u, "0", sizeof("0") - 1u},
      {"x-protocol", sizeof("x-protocol") - 1u, "h2", sizeof("h2") - 1u}};
  static const chttp_h2_hpack_header invalid_length_headers[] = {
      {":status", sizeof(":status") - 1u, "200", sizeof("200") - 1u},
      {"content-length", sizeof("content-length") - 1u, "3", sizeof("3") - 1u},
      {"x-protocol", sizeof("x-protocol") - 1u, "h2", sizeof("h2") - 1u}};
  static const chttp_h2_hpack_header invalid_name_headers[] = {
      {":status", sizeof(":status") - 1u, "200", sizeof("200") - 1u},
      {"content-length", sizeof("content-length") - 1u, "2", sizeof("2") - 1u},
      {"bad:name", sizeof("bad:name") - 1u, "h2", sizeof("h2") - 1u}};
  static const uint8_t body[] = {'o', 'k'};
  static const chttp_h2_hpack_header trailer_headers[] = {
      {"x-trailer", sizeof("x-trailer") - 1u, "late", sizeof("late") - 1u}};
  chttp_h2_test_server *server = (chttp_h2_test_server *)user;
  (void)end_stream;
  const size_t request_index = server->request_count;
  const int empty_response =
      server->empty_responses || (server->oversize_first_response && request_index != 0u);
  const int invalid_header_response =
      server->invalid_header_name || (server->invalid_first_header && request_index == 0u);
  if (request_index < sizeof(server->paths) / sizeof(server->paths[0]))
    memcpy(server->paths[request_index], server->path, sizeof(server->path));
  ++server->request_count;
  if (server->invalid_trailers) {
    if (chttp_h2_proto_submit_headers(server->protocol, stream_id, response_headers,
                                      sizeof(response_headers) / sizeof(response_headers[0]),
                                      0) != 0 ||
        chttp_h2_proto_submit_headers(server->protocol, stream_id, trailer_headers,
                                      sizeof(trailer_headers) / sizeof(trailer_headers[0]),
                                      0) != 0 ||
        chttp_h2_proto_submit_data(server->protocol, stream_id, body, sizeof(body), 1) != 0)
      return -1;
    return 0;
  }
  if (chttp_h2_proto_submit_response(
          server->protocol, stream_id,
          empty_response
              ? empty_response_headers
              : (server->invalid_content_length
                     ? invalid_length_headers
                     : (invalid_header_response ? invalid_name_headers : response_headers)),
          sizeof(response_headers) / sizeof(response_headers[0]), empty_response ? NULL : body,
          empty_response ? 0u : sizeof(body)) != 0)
    return -1;
  if (server->goaway_responses != 0u) {
    --server->goaway_responses;
    if (chttp_h2_proto_submit_goaway(server->protocol, (uint32_t)stream_id,
                                     CHTTP_H2_ERR_NO_ERROR) != 0)
      return -1;
  }
  return 0;
}

static void chttp_h2_test_on_goaway(void *user, uint32_t last_stream_id, uint32_t error_code) {
  chttp_h2_test_server *server = (chttp_h2_test_server *)user;
  (void)last_stream_id;
  (void)error_code;
  ++server->received_goaway;
}

static void chttp_h2_test_flush(chttp_h2_test_server *server, chttp_h2_test_socket accepted) {
  while (server->status == SALTS_OK && chttp_h2_proto_want_write(server->protocol)) {
    const uint8_t *wire = NULL;
    const ptrdiff_t wire_size = chttp_h2_proto_send(server->protocol, &wire);
    if (wire_size <= 0 || chttp_h2_test_send_all(accepted, wire, (size_t)wire_size) != SALTS_OK)
      server->status = SALTS_EIO;
  }
}

static int chttp_h2_pool_on_begin(void *user, int32_t stream_id) {
  chttp_h2_pool_peer *peer = (chttp_h2_pool_peer *)user;
  (void)stream_id;
  if (peer == NULL || peer->server == NULL || peer->index >= 2u) return -1;
  peer->server->paths[peer->index][0] = '\0';
  return 0;
}

static int chttp_h2_pool_on_header(void *user, int32_t stream_id, const char *name,
                                   size_t name_size, const char *value, size_t value_size) {
  chttp_h2_pool_peer *peer = (chttp_h2_pool_peer *)user;
  (void)stream_id;
  if (peer == NULL || peer->server == NULL || peer->index >= 2u) return -1;
  if (name_size == sizeof(":path") - 1u && memcmp(name, ":path", name_size) == 0 &&
      value_size < sizeof(peer->server->paths[peer->index])) {
    memcpy(peer->server->paths[peer->index], value, value_size);
    peer->server->paths[peer->index][value_size] = '\0';
  }
  return 0;
}

static int chttp_h2_pool_on_end(void *user, int32_t stream_id, int end_stream) {
  chttp_h2_pool_peer *peer = (chttp_h2_pool_peer *)user;
  (void)end_stream;
  if (peer == NULL || peer->server == NULL || peer->stream_id != 0) return -1;
  peer->stream_id = stream_id;
  ++peer->server->request_count;
  return 0;
}

static int chttp_h2_pool_flush(chttp_h2_pool_peer *peer) {
  while (chttp_h2_proto_want_write(peer->protocol)) {
    const uint8_t *wire = NULL;
    const ptrdiff_t wire_size = chttp_h2_proto_send(peer->protocol, &wire);
    if (wire_size <= 0 ||
        chttp_h2_test_send_all(peer->socket_value, wire, (size_t)wire_size) != SALTS_OK)
      return SALTS_EIO;
  }
  return SALTS_OK;
}

static int chttp_h2_pool_peer_open(chttp_h2_pool_server *server, chttp_h2_pool_peer *peer,
                                   size_t index) {
  chttp_h2_proto_callbacks callbacks = {0};
  peer->server = server;
  peer->index = index;
  peer->socket_value = accept(server->listener, NULL, NULL);
  if (peer->socket_value == CHTTP_H2_TEST_INVALID_SOCKET) return SALTS_EIO;
  ++server->accepted_connections;
  if (chttp_h2_test_set_timeout(peer->socket_value) != SALTS_OK) return SALTS_EIO;
  callbacks.user_data = peer;
  callbacks.on_begin_headers = chttp_h2_pool_on_begin;
  callbacks.on_header = chttp_h2_pool_on_header;
  callbacks.on_end_headers = chttp_h2_pool_on_end;
  peer->protocol = chttp_h2_proto_create(CHTTP_H2_PROTO_SERVER, NULL, &callbacks);
  if (peer->protocol == NULL) return SALTS_ENOMEM;
  if (chttp_h2_proto_set_local_settings(peer->protocol, 4096u, 0u, 1u, 65535u, 16384u,
                                        64u * 1024u) != 0)
    return SALTS_EPROTO;
  return SALTS_OK;
}

static int chttp_h2_pool_read_until(chttp_h2_pool_peer *peer, int wait_for_settings_ack) {
  unsigned char input[4096];
  while (peer->stream_id == 0 ||
         (wait_for_settings_ack && !chttp_h2_proto_settings_acked(peer->protocol))) {
    const int received = recv(peer->socket_value, (char *)input, (int)sizeof(input), 0);
    if (received <= 0 || chttp_h2_proto_recv(peer->protocol, input, (size_t)received) != received)
      return SALTS_EPROTO;
    if (chttp_h2_pool_flush(peer) != SALTS_OK) return SALTS_EIO;
  }
  return SALTS_OK;
}

static int chttp_h2_pool_respond(chttp_h2_pool_peer *peer) {
  static const chttp_h2_hpack_header response_headers[] = {
      {":status", sizeof(":status") - 1u, "200", sizeof("200") - 1u},
      {"content-length", sizeof("content-length") - 1u, "2", sizeof("2") - 1u}};
  static const uint8_t body[] = {'o', 'k'};
  if (chttp_h2_proto_submit_response(peer->protocol, peer->stream_id, response_headers,
                                     sizeof(response_headers) / sizeof(response_headers[0]), body,
                                     sizeof(body)) != 0)
    return SALTS_EPROTO;
  return chttp_h2_pool_flush(peer);
}

static void chttp_h2_pool_server_run(void *user) {
  chttp_h2_pool_server *server = (chttp_h2_pool_server *)user;
  chttp_h2_pool_peer peers[2] = {{0}};
  size_t index;
  if (server == NULL) return;
  peers[0].socket_value = CHTTP_H2_TEST_INVALID_SOCKET;
  peers[1].socket_value = CHTTP_H2_TEST_INVALID_SOCKET;
  server->status = chttp_h2_pool_peer_open(server, &peers[0], 0u);
  if (server->status == SALTS_OK) server->status = chttp_h2_pool_read_until(&peers[0], 1);
  if (server->status == SALTS_OK)
    atomic_store_explicit(&server->first_settings_acked, 1, memory_order_release);
  if (server->status == SALTS_OK) server->status = chttp_h2_pool_peer_open(server, &peers[1], 1u);
  if (server->status == SALTS_OK) server->status = chttp_h2_pool_read_until(&peers[1], 0);
  if (server->status == SALTS_OK) server->status = chttp_h2_pool_respond(&peers[1]);
  if (server->status == SALTS_OK) server->status = chttp_h2_pool_respond(&peers[0]);
  for (index = 0u; index < 2u; ++index) {
    chttp_h2_proto_destroy(peers[index].protocol);
    chttp_h2_test_close_socket(peers[index].socket_value);
  }
}

static void chttp_h2_test_serve_one(chttp_h2_test_server *server, size_t target_request_count,
                                    int wait_for_peer_close) {
  chttp_h2_proto_callbacks callbacks = {0};
  chttp_h2_test_socket accepted = CHTTP_H2_TEST_INVALID_SOCKET;
  unsigned char input[4096];
  accepted = accept(server->listener, NULL, NULL);
  if (accepted == CHTTP_H2_TEST_INVALID_SOCKET) {
    server->status = SALTS_EIO;
    return;
  }
  ++server->accepted_connections;
  server->status = chttp_h2_test_set_timeout(accepted);
  callbacks.user_data = server;
  callbacks.on_begin_headers = chttp_h2_test_on_begin;
  callbacks.on_header = chttp_h2_test_on_header;
  callbacks.on_end_headers = chttp_h2_test_on_end;
  callbacks.on_goaway = chttp_h2_test_on_goaway;
  server->protocol = chttp_h2_proto_create(CHTTP_H2_PROTO_SERVER, NULL, &callbacks);
  if (server->status == SALTS_OK && server->protocol == NULL) server->status = SALTS_ENOMEM;
  while (server->status == SALTS_OK && server->request_count < target_request_count) {
    const int received = recv(accepted, (char *)input, (int)sizeof(input), 0);
    if (received <= 0 ||
        chttp_h2_proto_recv(server->protocol, input, (size_t)received) != received) {
      server->status = SALTS_EPROTO;
      break;
    }
    chttp_h2_test_flush(server, accepted);
  }
  chttp_h2_test_flush(server, accepted);
  while (server->status == SALTS_OK && wait_for_peer_close) {
    const int received = recv(accepted, (char *)input, (int)sizeof(input), 0);
    if (received == 0) break;
    if (received < 0) {
      server->status = SALTS_EPROTO;
      break;
    }
    if (server->parse_until_close &&
        chttp_h2_proto_recv(server->protocol, input, (size_t)received) != received) {
      server->status = SALTS_EPROTO;
      break;
    }
    if (server->parse_until_close) chttp_h2_test_flush(server, accepted);
  }
  chttp_h2_proto_destroy(server->protocol);
  server->protocol = NULL;
  chttp_h2_test_close_socket(accepted);
}

static void chttp_h2_test_serve(void *user) {
  chttp_h2_test_server *server = (chttp_h2_test_server *)user;
  if (server == NULL) return;
  chttp_h2_test_serve_one(server, server->expected_requests, 0);
}

static void chttp_h2_test_serve_goaway(void *user) {
  chttp_h2_test_server *server = (chttp_h2_test_server *)user;
  if (server == NULL) return;
  chttp_h2_test_serve_one(server, 1u, 1);
  if (server->status == SALTS_OK) chttp_h2_test_serve_one(server, 2u, 0);
}

static void chttp_h2_test_serve_until_client_goaway(void *user) {
  chttp_h2_test_server *server = (chttp_h2_test_server *)user;
  if (server == NULL) return;
  chttp_h2_test_serve_one(server, 1u, 1);
}

static void chttp_h1_test_serve_keep_alive_until_close(void *user) {
  static const char response[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
  chttp_h2_test_server *server = (chttp_h2_test_server *)user;
  chttp_h2_test_socket accepted = CHTTP_H2_TEST_INVALID_SOCKET;
  char input[4096];
  size_t used = 0u;
  if (server == NULL) return;
  input[0] = '\0';
  accepted = accept(server->listener, NULL, NULL);
  if (accepted == CHTTP_H2_TEST_INVALID_SOCKET) {
    server->status = SALTS_EIO;
    return;
  }
  ++server->accepted_connections;
  server->status = chttp_h2_test_set_timeout(accepted);
  while (server->status == SALTS_OK && strstr(input, "\r\n\r\n") == NULL) {
    const int received = recv(accepted, input + used, (int)(sizeof(input) - used - 1u), 0);
    if (received <= 0) {
      server->status = SALTS_EPROTO;
      break;
    }
    used += (size_t)received;
    input[used] = '\0';
    if (used == sizeof(input) - 1u && strstr(input, "\r\n\r\n") == NULL)
      server->status = SALTS_EMSGSIZE;
  }
  if (server->status == SALTS_OK)
    server->status =
        chttp_h2_test_send_all(accepted, (const uint8_t *)response, sizeof(response) - 1u);
  while (server->status == SALTS_OK) {
    const int received = recv(accepted, input, (int)sizeof(input), 0);
    if (received == 0) break;
    if (received < 0) server->status = SALTS_EPROTO;
  }
  chttp_h2_test_close_socket(accepted);
}

static void chttp_h2_test_serve_after_timeout(void *user) {
  chttp_h2_test_server *server = (chttp_h2_test_server *)user;
  chttp_h2_test_socket accepted = CHTTP_H2_TEST_INVALID_SOCKET;
  unsigned char input[4096];
  if (server == NULL) return;
  accepted = accept(server->listener, NULL, NULL);
  if (accepted == CHTTP_H2_TEST_INVALID_SOCKET) {
    server->status = SALTS_EIO;
    return;
  }
  ++server->accepted_connections;
  server->status = chttp_h2_test_set_timeout(accepted);
  while (server->status == SALTS_OK) {
    const int received = recv(accepted, (char *)input, (int)sizeof(input), 0);
    if (received <= 0) break;
  }
  chttp_h2_test_close_socket(accepted);
  if (server->status == SALTS_OK) chttp_h2_test_serve_one(server, 1u, 0);
}

static int chttp_h2_tls_server_flush(chttp_h2_tls_server *server) {
  const uint8_t *wire = NULL;
  ptrdiff_t wire_size;
  int status;
  if (server->send_active || !chttp_h2_proto_want_write(server->protocol)) return SALTS_OK;
  wire_size = chttp_h2_proto_send(server->protocol, &wire);
  if (wire_size <= 0) return SALTS_EPROTO;
  status = cnet_send(&server->network, server->connection, wire, (size_t)wire_size);
  if (status == SALTS_OK) server->send_active = 1;
  return status;
}

static int chttp_h2_tls_server_on_end(void *user, int32_t stream_id, int end_stream) {
  static const chttp_h2_hpack_header response_headers[] = {
      {":status", sizeof(":status") - 1u, "200", sizeof("200") - 1u},
      {"content-length", sizeof("content-length") - 1u, "3", sizeof("3") - 1u},
      {"x-transport", sizeof("x-transport") - 1u, "tls-h2", sizeof("tls-h2") - 1u}};
  static const uint8_t body[] = {'t', 'l', 's'};
  chttp_h2_tls_server *server = (chttp_h2_tls_server *)user;
  (void)end_stream;
  if (chttp_h2_proto_submit_response(server->protocol, stream_id, response_headers,
                                     sizeof(response_headers) / sizeof(response_headers[0]), body,
                                     sizeof(body)) != 0)
    return -1;
  server->response_submitted = 1;
  return 0;
}

static void chttp_h2_tls_server_state(void *user, cnet_connection connection,
                                      cnet_connection_state state, const cnet_error *error) {
  chttp_h2_tls_server *server = (chttp_h2_tls_server *)user;
  if (server->connection.slot != connection.slot ||
      server->connection.generation != connection.generation)
    return;
  if (state == CNET_CONNECTION_CONNECTED) {
    server->connected = 1;
    server->status = cnet_receive(&server->network, connection, 1u);
  } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    if (state == CNET_CONNECTION_FAILED && server->status == SALTS_OK)
      server->status = error != NULL ? error->status : SALTS_EIO;
    server->terminal = 1;
  }
}

static void chttp_h2_tls_server_receive(void *user, cnet_connection connection,
                                        const cnet_receive_view *view) {
  chttp_h2_tls_server *server = (chttp_h2_tls_server *)user;
  ptrdiff_t consumed;
  if (server->connection.slot != connection.slot ||
      server->connection.generation != connection.generation || view == NULL ||
      view->kind != CNET_MESSAGE_BYTES) {
    server->status = SALTS_EPROTO;
    return;
  }
  consumed = chttp_h2_proto_recv(server->protocol, (const uint8_t *)view->data, view->size);
  if (consumed < 0 || (size_t)consumed != view->size) {
    server->status = SALTS_EPROTO;
    (void)cnet_close(&server->network, connection);
    return;
  }
  server->status = chttp_h2_tls_server_flush(server);
  if (server->status == SALTS_OK && !server->response_submitted)
    server->status = cnet_receive(&server->network, connection, 1u);
}

static void chttp_h2_tls_server_send(void *user, cnet_connection connection, size_t size) {
  chttp_h2_tls_server *server = (chttp_h2_tls_server *)user;
  (void)size;
  if (server->connection.slot != connection.slot ||
      server->connection.generation != connection.generation)
    return;
  server->send_active = 0;
  server->status = chttp_h2_tls_server_flush(server);
  if (server->status == SALTS_OK && server->response_submitted &&
      !chttp_h2_proto_want_write(server->protocol))
    server->status = cnet_close(&server->network, connection);
}

static void chttp_h2_tls_server_run(void *user) {
  chttp_h2_tls_server *server = (chttp_h2_tls_server *)user;
  chttp_h2_proto_callbacks protocol_callbacks = {0};
  cnet_observer observer;
  uint64_t deadline;
  int ready = 0;
  if (server == NULL) return;
  protocol_callbacks.user_data = server;
  protocol_callbacks.on_end_headers = chttp_h2_tls_server_on_end;
  server->protocol = chttp_h2_proto_create(CHTTP_H2_PROTO_SERVER, NULL, &protocol_callbacks);
  if (server->protocol == NULL) {
    server->status = SALTS_ENOMEM;
    return;
  }
  server->status = cnet_listener_wait(&server->listener, CHTTP_H2_TEST_TIMEOUT_MS, &ready);
  observer = (cnet_observer){.on_state = chttp_h2_tls_server_state,
                             .on_receive = chttp_h2_tls_server_receive,
                             .user = server,
                             .on_send = chttp_h2_tls_server_send};
  if (server->status == SALTS_OK && !ready) server->status = SALTS_ETIMEDOUT;
  if (server->status == SALTS_OK)
    server->status = cnet_listener_accept_tls(&server->listener, &server->network, &server->tls,
                                              &observer, &server->connection);
  deadline = salts_monotonic_ms() + CHTTP_H2_TEST_TIMEOUT_MS;
  while (server->status == SALTS_OK && !server->terminal && salts_monotonic_ms() < deadline) {
    size_t events = 0u;
    server->status = cnet_client_poll(&server->network, 5u, &events);
  }
  if (server->status == SALTS_OK && !server->terminal) server->status = SALTS_ETIMEDOUT;
  (void)cnet_listener_close(&server->listener);
  (void)cnet_listener_destroy(&server->listener);
  (void)cnet_client_stop(&server->network, CHTTP_H2_TEST_TIMEOUT_MS);
  (void)cnet_client_destroy(&server->network);
  (void)cnet_tls_server_destroy(&server->tls);
  chttp_h2_proto_destroy(server->protocol);
  server->protocol = NULL;
}

static void chttp_h2_test_on_complete(void *user, chttp_request request,
                                      const chttp_response_view *response,
                                      const chttp_error *error) {
  chttp_h2_test_completion *completion = (chttp_h2_test_completion *)user;
  const size_t index = completion->count;
  (void)request;
  if (index >= sizeof(completion->statuses) / sizeof(completion->statuses[0])) return;
  completion->statuses[index] = error == NULL ? SALTS_OK : error->status;
  if (response != NULL) {
    const size_t body_size = response->body_size < sizeof(completion->bodies[index]) - 1u
                                 ? response->body_size
                                 : sizeof(completion->bodies[index]) - 1u;
    completion->response_statuses[index] = response->status_code;
    if (body_size != 0u) memcpy(completion->bodies[index], response->body, body_size);
    completion->bodies[index][body_size] = '\0';
  }
  ++completion->count;
}

spec("CHTTP HTTP/2 client") {
  it("performs a blocking h2c prior-knowledge request without caller polling") {
    chttp_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    salts_thread_t thread = NULL;
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(chttp_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 1u;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve, &server), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/h2",
                              .timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.http_major, 2u);
    check_equal(response.http_minor, 0u);
    check_equal(response.status_code, 200u);
    check_equal(chttp_response_header(&response, "x-protocol"), "h2");
    check_equal(response.body, "ok", 2u);
    check_equal(response.body_size, 2u);
    chttp_response_destroy(&response);
    check_equal(chttp_client_destroy(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    check_equal(server.request_count, 1u);
    check_equal(server.path, "/h2");
    check_equal(server.accepted_connections, 1u);
    chttp_h2_test_close_socket(listener);
  }

  it("flushes GOAWAY before a blocking client closes its HTTP/2 session") {
    chttp_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    salts_thread_t thread = NULL;
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(chttp_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 1u;
    server.parse_until_close = 1;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve_until_client_goaway, &server),
                SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/stop",
                              .timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    chttp_response_destroy(&response);
    check_equal(chttp_client_destroy(&client, 0u), SALTS_ETIMEDOUT);
    check_equal(chttp_client_destroy(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    check_equal(server.received_goaway, 1);
    chttp_h2_test_close_socket(listener);
  }

  it("multiplexes concurrent targets from one authority on one connection") {
    chttp_async_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    chttp_h2_test_completion completion = {0};
    salts_thread_t thread = NULL;
    chttp_request first = {0};
    chttp_request second = {0};
    chttp_request_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t poll_count = 0u;
    config.network.connection_capacity = 1u;

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 2u;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve, &server), SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/first",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_test_on_complete,
                                      .user = &completion,
                                      .protocol = CHTTP_HTTP_2};

    check_equal(chttp_async_client_submit(&client, &options, &first), SALTS_OK);
    options.target = "/second";
    check_equal(chttp_async_client_submit(&client, &options, &second), SALTS_OK);
    check_equal(completion.count, 0u);
    while (completion.count < 2u && poll_count++ < 20u) {
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);
    }

    check_equal(completion.count, 2u);
    check_equal(completion.statuses[0], SALTS_OK);
    check_equal(completion.statuses[1], SALTS_OK);
    check_equal(completion.response_statuses[0], 200u);
    check_equal(completion.response_statuses[1], 200u);
    check_equal(completion.bodies[0], "ok");
    check_equal(completion.bodies[1], "ok");
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    check_equal(server.request_count, 2u);
    check_equal(server.paths[0], "/first");
    check_equal(server.paths[1], "/second");
    check_equal(server.accepted_connections, 1u);
    chttp_h2_test_close_socket(listener);
  }

  it("evicts an idle HTTP/2 origin when the physical pool is full") {
    chttp_async_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    chttp_h2_test_completion first_completion = {0};
    chttp_h2_test_completion second_completion = {0};
    salts_thread_t thread = NULL;
    chttp_request first = {0};
    chttp_request second = {0};
    chttp_request_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t poll_count = 0u;
    int admission_status;
    config.network.connection_capacity = 1u;

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 1u;
    server.parse_until_close = 1;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve_until_client_goaway, &server),
                SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/first-origin",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_test_on_complete,
                                      .user = &first_completion,
                                      .protocol = CHTTP_HTTP_2};

    check_equal(chttp_async_client_submit(&client, &options, &first), SALTS_OK);
    while (first_completion.count == 0u && poll_count++ < 20u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);
    check_equal(first_completion.count, 1u);
    check_equal(first_completion.statuses[0], SALTS_OK);

    options.authority = "alternate.example";
    options.target = "/second-origin";
    options.user = &second_completion;
    admission_status = chttp_async_client_submit(&client, &options, &second);
    check_equal(admission_status, SALTS_ENOBUFS);
    poll_count = 0u;
    while (admission_status == SALTS_ENOBUFS && poll_count++ < 20u) {
      check_equal(chttp_async_client_poll(&client, 50u, &completions), SALTS_OK);
      admission_status = chttp_async_client_submit(&client, &options, &second);
    }
    check_equal(admission_status, SALTS_OK);
    if (admission_status == SALTS_OK) {
      check_equal(chttp_async_request_cancel(&client, second), SALTS_OK);
      poll_count = 0u;
      while (second_completion.count == 0u && poll_count++ < 20u)
        check_equal(chttp_async_client_poll(&client, 50u, &completions), SALTS_OK);
      check_equal(second_completion.count, 1u);
      check_equal(second_completion.statuses[0], SALTS_ECANCELED);
    }
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    check_equal(server.received_goaway, 1);
    check_equal(server.accepted_connections, 1u);
    chttp_h2_test_close_socket(listener);
  }

  it("evicts an idle HTTP/2 session before admitting HTTP/1") {
    chttp_async_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    chttp_h2_test_completion first_completion = {0};
    chttp_h2_test_completion second_completion = {0};
    salts_thread_t thread = NULL;
    chttp_request first = {0};
    chttp_request second = {0};
    chttp_request_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t poll_count = 0u;
    int admission_status;
    config.network.connection_capacity = 1u;

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 1u;
    server.parse_until_close = 1;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve_until_client_goaway, &server),
                SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/h2-first",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_test_on_complete,
                                      .user = &first_completion,
                                      .protocol = CHTTP_HTTP_2};

    check_equal(chttp_async_client_submit(&client, &options, &first), SALTS_OK);
    while (first_completion.count == 0u && poll_count++ < 20u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);
    check_equal(first_completion.count, 1u);
    check_equal(first_completion.statuses[0], SALTS_OK);

    options.target = "/h1-second";
    options.user = &second_completion;
    options.protocol = CHTTP_HTTP_1_1;
    admission_status = chttp_async_client_submit(&client, &options, &second);
    check_equal(admission_status, SALTS_ENOBUFS);
    poll_count = 0u;
    while (admission_status == SALTS_ENOBUFS && poll_count++ < 20u) {
      check_equal(chttp_async_client_poll(&client, 50u, &completions), SALTS_OK);
      admission_status = chttp_async_client_submit(&client, &options, &second);
    }
    check_equal(admission_status, SALTS_OK);
    if (admission_status == SALTS_OK) {
      check_equal(chttp_async_request_cancel(&client, second), SALTS_OK);
      poll_count = 0u;
      while (second_completion.count == 0u && poll_count++ < 20u)
        check_equal(chttp_async_client_poll(&client, 50u, &completions), SALTS_OK);
      check_equal(second_completion.count, 1u);
      check_equal(second_completion.statuses[0], SALTS_ECANCELED);
    }
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    check_equal(server.received_goaway, 1);
    check_equal(server.accepted_connections, 1u);
    chttp_h2_test_close_socket(listener);
  }

  it("evicts an idle HTTP/1 connection before admitting HTTP/2") {
    chttp_async_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    chttp_h2_test_completion first_completion = {0};
    chttp_h2_test_completion second_completion = {0};
    salts_thread_t thread = NULL;
    chttp_request first = {0};
    chttp_request second = {0};
    chttp_request_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t poll_count = 0u;
    int admission_status;
    config.network.connection_capacity = 1u;

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    check_equal(salts_thread_create(&thread, chttp_h1_test_serve_keep_alive_until_close, &server),
                SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/h1-first",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_test_on_complete,
                                      .user = &first_completion,
                                      .protocol = CHTTP_HTTP_1_1};

    check_equal(chttp_async_client_submit(&client, &options, &first), SALTS_OK);
    while (first_completion.count == 0u && poll_count++ < 20u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);
    check_equal(first_completion.count, 1u);
    check_equal(first_completion.statuses[0], SALTS_OK);

    options.target = "/h2-second";
    options.user = &second_completion;
    options.protocol = CHTTP_HTTP_2;
    admission_status = chttp_async_client_submit(&client, &options, &second);
    check_equal(admission_status, SALTS_ENOBUFS);
    poll_count = 0u;
    while (admission_status == SALTS_ENOBUFS && poll_count++ < 20u) {
      check_equal(chttp_async_client_poll(&client, 50u, &completions), SALTS_OK);
      admission_status = chttp_async_client_submit(&client, &options, &second);
    }
    check_equal(admission_status, SALTS_OK);
    if (admission_status == SALTS_OK) {
      check_equal(chttp_async_request_cancel(&client, second), SALTS_OK);
      poll_count = 0u;
      while (second_completion.count == 0u && poll_count++ < 20u)
        check_equal(chttp_async_client_poll(&client, 50u, &completions), SALTS_OK);
      check_equal(second_completion.count, 1u);
      check_equal(second_completion.statuses[0], SALTS_ECANCELED);
    }
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    check_equal(server.accepted_connections, 1u);
    chttp_h2_test_close_socket(listener);
  }

  it("resets only the oversized response stream and preserves its sibling") {
    chttp_async_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    chttp_h2_test_completion first_completion = {0};
    chttp_h2_test_completion second_completion = {0};
    salts_thread_t thread = NULL;
    chttp_request first = {0};
    chttp_request second = {0};
    chttp_request_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t poll_count = 0u;
    config.network.connection_capacity = 1u;
    config.max_response_body_bytes = 1u;

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 2u;
    server.oversize_first_response = 1;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve, &server), SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/oversized",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_test_on_complete,
                                      .user = &first_completion,
                                      .protocol = CHTTP_HTTP_2};

    check_equal(chttp_async_client_submit(&client, &options, &first), SALTS_OK);
    options.target = "/sibling-empty";
    options.user = &second_completion;
    check_equal(chttp_async_client_submit(&client, &options, &second), SALTS_OK);
    while ((first_completion.count == 0u || second_completion.count == 0u) && poll_count++ < 20u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);

    check_equal(first_completion.count, 1u);
    check_equal(first_completion.statuses[0], SALTS_EMSGSIZE);
    check_equal(second_completion.count, 1u);
    check_equal(second_completion.statuses[0], SALTS_OK);
    check_equal(second_completion.response_statuses[0], 200u);
    check_equal(second_completion.bodies[0], "");
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    check_equal(server.request_count, 2u);
    check_equal(server.accepted_connections, 1u);
    chttp_h2_test_close_socket(listener);
  }

  it("resets only a semantically invalid response stream and preserves its sibling") {
    chttp_async_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    chttp_h2_test_completion first_completion = {0};
    chttp_h2_test_completion second_completion = {0};
    salts_thread_t thread = NULL;
    chttp_request first = {0};
    chttp_request second = {0};
    chttp_request_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;
    size_t poll_count = 0u;
    config.network.connection_capacity = 1u;

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 2u;
    server.invalid_first_header = 1;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve, &server), SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/invalid-response",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_test_on_complete,
                                      .user = &first_completion,
                                      .protocol = CHTTP_HTTP_2};

    check_equal(chttp_async_client_submit(&client, &options, &first), SALTS_OK);
    options.target = "/valid-sibling";
    options.user = &second_completion;
    check_equal(chttp_async_client_submit(&client, &options, &second), SALTS_OK);
    while ((first_completion.count == 0u || second_completion.count == 0u) && poll_count++ < 20u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);

    check_equal(first_completion.count, 1u);
    check_equal(first_completion.statuses[0], SALTS_EPROTO);
    check_equal(second_completion.count, 1u);
    check_equal(second_completion.statuses[0], SALTS_OK);
    check_equal(second_completion.response_statuses[0], 200u);
    check_equal(second_completion.bodies[0], "ok");
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    check_equal(server.request_count, 2u);
    check_equal(server.accepted_connections, 1u);
    chttp_h2_test_close_socket(listener);
  }

  it("completes stream cancellation later without closing the client") {
    chttp_async_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_completion completion = {0};
    chttp_request request = {0};
    chttp_request_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    size_t completions = 0u;

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/cancel",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_test_on_complete,
                                      .user = &completion,
                                      .protocol = CHTTP_HTTP_2};

    check_equal(chttp_async_client_submit(&client, &options, &request), SALTS_OK);
    check_equal(chttp_async_request_cancel(&client, request), SALTS_OK);
    check_equal(completion.count, 0u);
    check_equal(chttp_async_client_poll(&client, 0u, &completions), SALTS_OK);
    check_equal(completions, 1u);
    check_equal(completion.count, 1u);
    check_equal(completion.statuses[0], SALTS_ECANCELED);
    check_equal(chttp_async_request_cancel(&client, request), SALTS_ENOENT);
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    chttp_h2_test_close_socket(listener);
  }

  it("opens another pooled session when the peer stream limit is full") {
    chttp_h2_pool_server server = {0};
    chttp_h2_test_completion completion = {0};
    chttp_async_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_socket unblocker = CHTTP_H2_TEST_INVALID_SOCKET;
    salts_thread_t thread = NULL;
    chttp_request first = {0};
    chttp_request second = {0};
    chttp_request_options options;
    uint16_t port = 0u;
    char uri[64];
    char authority[64];
    size_t completions = 0u;
    size_t poll_count = 0u;
    int second_status;

    atomic_init(&server.first_settings_acked, 0);
    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    check_equal(salts_thread_create(&thread, chttp_h2_pool_server_run, &server), SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/limited-first",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_test_on_complete,
                                      .user = &completion,
                                      .protocol = CHTTP_HTTP_2};

    check_equal(chttp_async_client_submit(&client, &options, &first), SALTS_OK);
    while (atomic_load_explicit(&server.first_settings_acked, memory_order_acquire) == 0 &&
           poll_count++ < 20u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);
    check_equal(atomic_load_explicit(&server.first_settings_acked, memory_order_acquire), 1);
    options.target = "/limited-second";
    second_status = chttp_async_client_submit(&client, &options, &second);
    check_equal(second_status, SALTS_OK);
    if (second_status == SALTS_OK) {
      while (completion.count < 2u && poll_count++ < 40u)
        check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);
    } else {
      unblocker = chttp_h2_test_connect_port(port);
      chttp_h2_test_close_socket(unblocker);
      unblocker = CHTTP_H2_TEST_INVALID_SOCKET;
    }

    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    if (second_status == SALTS_OK) {
      check_equal(completion.count, 2u);
      check_equal(completion.statuses[0], SALTS_OK);
      check_equal(completion.statuses[1], SALTS_OK);
      check_equal(server.status, SALTS_OK);
      check_equal(server.accepted_connections, 2u);
      check_equal(server.paths[0], "/limited-first");
      check_equal(server.paths[1], "/limited-second");
    }
    chttp_h2_test_close_socket(unblocker);
    chttp_h2_test_close_socket(listener);
  }

  it("keeps a sibling stream alive when a canceled response was already in flight") {
    chttp_h2_test_server server = {0};
    chttp_h2_test_completion completion = {0};
    chttp_async_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    salts_thread_t thread = NULL;
    chttp_request first = {0};
    chttp_request second = {0};
    chttp_request_options options;
    uint16_t port = 0u;
    char uri[64];
    char authority[64];
    size_t completions = 0u;
    size_t poll_count = 0u;

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 2u;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve, &server), SALTS_OK);
    options = (chttp_request_options){.connection_uri = uri,
                                      .authority = authority,
                                      .target = "/cancel-in-flight",
                                      .method = CHTTP_METHOD_GET,
                                      .on_complete = chttp_h2_test_on_complete,
                                      .user = &completion,
                                      .protocol = CHTTP_HTTP_2};

    check_equal(chttp_async_client_submit(&client, &options, &first), SALTS_OK);
    check_equal(chttp_async_request_cancel(&client, first), SALTS_OK);
    options.target = "/sibling";
    check_equal(chttp_async_client_submit(&client, &options, &second), SALTS_OK);
    while (completion.count < 2u && poll_count++ < 20u)
      check_equal(chttp_async_client_poll(&client, 250u, &completions), SALTS_OK);

    check_equal(completion.count, 2u);
    check_equal(completion.statuses[0], SALTS_ECANCELED);
    check_equal(completion.statuses[1], SALTS_OK);
    check_equal(completion.response_statuses[1], 200u);
    check_equal(completion.bodies[1], "ok");
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    check_equal(server.request_count, 2u);
    check_equal(server.accepted_connections, 1u);
    chttp_h2_test_close_socket(listener);
  }

  it("rejects HTTP/2 admission when its explicit parser bound is too small") {
    chttp_async_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_completion completion = {0};
    chttp_request request = {0};
    const chttp_request_options options = {.connection_uri = "tcp://127.0.0.1:1",
                                           .authority = "127.0.0.1:1",
                                           .target = "/invalid-limit",
                                           .method = CHTTP_METHOD_GET,
                                           .on_complete = chttp_h2_test_on_complete,
                                           .user = &completion,
                                           .protocol = CHTTP_HTTP_2};
    config.h2_input_buffer_bytes = 16u * 1024u;

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_async_client_submit(&client, &options, &request), SALTS_EMSGSIZE);
    check_equal(request.slot, 0u);
    check_equal(completion.count, 0u);
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
  }

  it("rejects malformed request fields and generated-header overflow before admission") {
    static const chttp_header invalid_te[] = {{"te", "gzip"}};
    const chttp_body_source invalid_source = {.read = chttp_h2_test_source_read,
                                              .content_length_known = 2};
    chttp_async_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_completion completion = {0};
    chttp_request request = {0};
    chttp_request_options options = {.connection_uri = "tcp://127.0.0.1:1",
                                     .authority = "localhost",
                                     .target = "/request-validation",
                                     .method = CHTTP_METHOD_GET,
                                     .headers = invalid_te,
                                     .header_count = 1u,
                                     .on_complete = chttp_h2_test_on_complete,
                                     .user = &completion,
                                     .protocol = CHTTP_HTTP_2};
    int status;

    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    status = chttp_async_client_submit(&client, &options, &request);
    check_equal(status, SALTS_EINVAL);
    if (status == SALTS_OK) check_equal(chttp_async_request_cancel(&client, request), SALTS_OK);
    options.headers = NULL;
    options.header_count = 0u;
    options.body_source = &invalid_source;
    request = (chttp_request){0};
    status = chttp_async_client_submit(&client, &options, &request);
    check_equal(status, SALTS_EINVAL);
    if (status == SALTS_OK) check_equal(chttp_async_request_cancel(&client, request), SALTS_OK);
    options.body_source = NULL;
    config.max_header_bytes = 128u;
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_async_client_init(&client, &config), SALTS_OK);
    request = (chttp_request){0};
    status = chttp_async_client_submit(&client, &options, &request);
    check_equal(status, SALTS_EMSGSIZE);
    if (status == SALTS_OK) check_equal(chttp_async_request_cancel(&client, request), SALTS_OK);
    check_equal(chttp_async_client_stop(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
  }

  it("recovers a blocking client after an HTTP/2 request deadline") {
    chttp_client client = {0};
    cnet_client socket_guard = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    salts_thread_t thread = NULL;
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;
    int recovery_status;

    check_equal(chttp_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_client_init(&socket_guard, &config.network), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 1u;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve_after_timeout, &server), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/timeout",
                              .timeout_ms = CHTTP_H2_TEST_DEADLINE_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_get(&client, &options, &response, &error), SALTS_ETIMEDOUT);
    check_equal(error.status, SALTS_ETIMEDOUT);
    check_equal(error.stage, "request-deadline");
    options.target = "/after-timeout";
    options.timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS;
    recovery_status = chttp_get(&client, &options, &response, &error);
    check_equal(chttp_client_destroy(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.accepted_connections, 2u);
    check_equal(server.paths[0], "/after-timeout");
    check_equal(server.status, SALTS_OK);
    check_equal(recovery_status, SALTS_OK);
    check_equal(response.status_code, 200u);
    check_equal(response.body, "ok", 2u);
    chttp_response_destroy(&response);
    chttp_h2_test_close_socket(listener);
    check_equal(cnet_client_stop(&socket_guard, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&socket_guard), SALTS_OK);
  }

  it("negotiates exact h2 ALPN and exchanges HTTP/2 over TLS") {
    static const char *h2[] = {"h2"};
    chttp_h2_tls_server server = {0};
    chttp_client client = {0};
    chttp_tls_profile profile = {0};
    chttp_client_config client_config = chttp_h2_test_config();
    cnet_client_config server_network = client_config.network;
    cnet_listener_config listener_config;
    cnet_tls_server_config server_tls;
    cnet_tls_client_config client_tls;
    salts_thread_t thread = NULL;
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    char *cert_path = tt_make_temp_file("chttp-h2-cert", ".pem");
    char *key_path = tt_make_temp_file("chttp-h2-key", ".pem");
    char uri[64];
    uint16_t port = 0u;

    check_not_null(cert_path);
    check_not_null(key_path);
    check_equal(tt_write_file(cert_path, CHTTP_TLS_TEST_CERTIFICATE,
                              sizeof(CHTTP_TLS_TEST_CERTIFICATE) - 1u),
                0);
    check_equal(tt_write_file(key_path, CHTTP_TLS_TEST_KEY, sizeof(CHTTP_TLS_TEST_KEY) - 1u), 0);
    client_config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    client_config.network.tls_handshake_timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS;
    server_network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    server_network.tls_handshake_timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS;
    listener_config = (cnet_listener_config){
        .backend = chttp_h2_test_backend(), .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    server_tls = (cnet_tls_server_config){.size = sizeof(server_tls),
                                          .cert_file = cert_path,
                                          .key_file = key_path,
                                          .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
                                          .alpn_protocols = h2,
                                          .alpn_protocol_count = 1u};
    client_tls = (cnet_tls_client_config){.size = sizeof(client_tls),
                                          .ca_file = cert_path,
                                          .server_name = "localhost",
                                          .alpn_protocols = h2,
                                          .alpn_protocol_count = 1u};

    check_equal(cnet_client_init(&server.network, &server_network), SALTS_OK);
    check_equal(cnet_listener_init(&server.listener, &listener_config), SALTS_OK);
    check_equal(cnet_tls_server_init(&server.tls, &server_tls), SALTS_OK);
    check_equal(cnet_listener_port(&server.listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
    check_equal(chttp_tls_profile_init(&profile, &client_tls), SALTS_OK);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    check_equal(salts_thread_create(&thread, chttp_h2_tls_server_run, &server), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = "localhost",
                              .target = "/tls-h2",
                              .timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS,
                              .tls = &profile,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.http_major, 2u);
    check_equal(response.status_code, 200u);
    check_equal(chttp_response_header(&response, "x-transport"), "tls-h2");
    check_equal(response.body, "tls", 3u);
    chttp_response_destroy(&response);
    check_equal(chttp_tls_profile_destroy(&profile), SALTS_OK);
    check_equal(chttp_client_destroy(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    check_equal(server.connected, 1);
    check_equal(server.response_submitted, 1);
    check_equal(tt_remove_file(cert_path), 0);
    check_equal(tt_remove_file(key_path), 0);
    free(cert_path);
    free(key_path);
  }

  it("rejects a response whose content-length disagrees with DATA") {
    chttp_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    salts_thread_t thread = NULL;
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(chttp_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 1u;
    server.invalid_content_length = 1;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve, &server), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/bad-length",
                              .timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_get(&client, &options, &response, &error), SALTS_EPROTO);
    check_equal(error.status, SALTS_EPROTO);
    check_null(response.body);
    check_equal(chttp_client_destroy(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    chttp_h2_test_close_socket(listener);
  }

  it("rejects a response header name that is not a lowercase token") {
    chttp_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    salts_thread_t thread = NULL;
    chttp_options options;
    chttp_response response = {0};
    chttp_error error = {0};
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(chttp_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 1u;
    server.invalid_header_name = 1;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve, &server), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/bad-header-name",
                              .timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_get(&client, &options, &response, &error), SALTS_EPROTO);
    check_equal(error.status, SALTS_EPROTO);
    check_null(response.body);
    check_equal(chttp_client_destroy(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    chttp_h2_test_close_socket(listener);
  }

  it("replaces a drained GOAWAY session for the next request") {
    chttp_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    salts_thread_t thread = NULL;
    chttp_response response = {0};
    chttp_error error = {0};
    chttp_options options;
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(chttp_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 2u;
    server.goaway_responses = 1u;
    server.empty_responses = 1;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve_goaway, &server), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/before-goaway",
                              .timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    chttp_response_destroy(&response);
    options.target = "/after-goaway";
    check_equal(chttp_get(&client, &options, &response, &error), SALTS_OK);
    check_equal(response.status_code, 200u);
    chttp_response_destroy(&response);
    check_equal(chttp_client_destroy(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    check_equal(server.request_count, 2u);
    check_equal(server.accepted_connections, 2u);
    check_equal(server.paths[0], "/before-goaway");
    check_equal(server.paths[1], "/after-goaway");
    chttp_h2_test_close_socket(listener);
  }

  it("rejects a trailer block that does not end the response stream") {
    chttp_client client = {0};
    chttp_client_config config = chttp_h2_test_config();
    chttp_h2_test_socket listener = CHTTP_H2_TEST_INVALID_SOCKET;
    chttp_h2_test_server server = {0};
    salts_thread_t thread = NULL;
    chttp_options options;
    chttp_response response = {0};
    chttp_error error = {0};
    char uri[64];
    char authority[64];
    uint16_t port = 0u;

    check_equal(chttp_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_h2_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    check_greater(snprintf(authority, sizeof(authority), "127.0.0.1:%u", (unsigned int)port), 0);
    server.listener = listener;
    server.expected_requests = 1u;
    server.invalid_trailers = 1;
    check_equal(salts_thread_create(&thread, chttp_h2_test_serve, &server), SALTS_OK);
    options = (chttp_options){.connection_uri = uri,
                              .authority = authority,
                              .target = "/bad-trailers",
                              .timeout_ms = CHTTP_H2_TEST_TIMEOUT_MS,
                              .protocol = CHTTP_HTTP_2};

    check_equal(chttp_get(&client, &options, &response, &error), SALTS_EPROTO);
    check_equal(error.status, SALTS_EPROTO);
    check_null(response.body);
    check_equal(chttp_client_destroy(&client, CHTTP_H2_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    salts_thread_destroy(&thread);
    check_equal(server.status, SALTS_OK);
    chttp_h2_test_close_socket(listener);
  }
}
