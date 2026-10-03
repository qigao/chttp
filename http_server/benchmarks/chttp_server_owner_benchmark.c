#include <http_server/http.h>

#include <salts/clock.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET bench_socket;
  #define BENCH_INVALID_SOCKET INVALID_SOCKET
  #define bench_close_socket closesocket
#else
  #include <arpa/inet.h>
  #include <errno.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int bench_socket;
  #define BENCH_INVALID_SOCKET (-1)
  #define bench_close_socket close
#endif

enum {
  BENCH_CONNECTIONS = 8,
  BENCH_REQUESTS_DEFAULT = 200,
  BENCH_WARMUP_DEFAULT = 20,
  BENCH_TIMEOUT_MS = 5000,
  BENCH_RESPONSE_BYTES = 4096
};

typedef struct bench_barrier {
  atomic_int ready;
  atomic_int start;
} bench_barrier;

typedef struct bench_client {
  uint16_t port;
  size_t warmup;
  size_t requests;
  uint64_t *latency;
  bench_barrier *barrier;
  int status;
} bench_client;

static int bench_u64_compare(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t bench_percentile(const uint64_t *values, size_t count,
                                 unsigned int percent) {
  uint64_t *ordered;
  size_t rank;
  uint64_t result;
  if (values == NULL || count == 0u) return 0u;
  ordered = (uint64_t *)malloc(count * sizeof(*ordered));
  if (ordered == NULL) return 0u;
  memcpy(ordered, values, count * sizeof(*ordered));
  qsort(ordered, count, sizeof(*ordered), bench_u64_compare);
  rank = ((size_t)percent * count + 99u) / 100u;
  if (rank == 0u) rank = 1u;
  if (rank > count) rank = count;
  result = ordered[rank - 1u];
  free(ordered);
  return result;
}

static size_t bench_env_count(const char *name, size_t fallback) {
  const char *value = getenv(name);
  char *end = NULL;
  unsigned long parsed;
  if (value == NULL || value[0] == '\0') return fallback;
  parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0ul) return fallback;
  return (size_t)parsed;
}

static native_io_backend_kind bench_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config bench_network_config(void) {
  return (cnet_client_config){
      .backend = bench_backend(),
      .connection_capacity = 32u,
      .command_capacity = 64u,
      .request_capacity = 32u,
      .completion_batch_capacity = 32u,
      .event_capacity = 64u,
      .max_send_bytes = 64u * 1024u,
      .receive_buffer_bytes = 4096u,
      .connect_timeout_ms = BENCH_TIMEOUT_MS,
      .read_timeout_ms = BENCH_TIMEOUT_MS,
      .write_timeout_ms = BENCH_TIMEOUT_MS};
}

static chttp_server_config bench_server_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 64u,
      .network = bench_network_config(),
      .route_capacity = 4u,
      .middleware_capacity = 1u,
      .max_route_middleware_count = 1u,
      .max_route_param_count = 1u,
      .max_route_param_bytes = 32u,
      .max_target_bytes = 128u,
      .max_header_count = 16u,
      .max_header_bytes = 1024u,
      .max_request_body_bytes = 1024u,
      .max_response_header_count = 16u,
      .max_response_header_bytes = 1024u,
      .max_response_body_bytes = 64u * 1024u,
      .session_capacity = 0u,
      .poll_slice_ms = 1u};
}

static int bench_handler(void *user, const chttp_server_request_view *request,
                         chttp_server_response *response) {
  static const char body[] = "ok";
  (void)user;
  (void)request;
  return chttp_server_reply(response, 200u, "text/plain",
                            body, sizeof(body) - 1u);
}

static int bench_socket_timeout(bench_socket socket_value) {
#if defined(_WIN32)
  DWORD timeout = BENCH_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                    (const char *)&timeout, sizeof(timeout)) == 0 &&
                 setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                            (const char *)&timeout, sizeof(timeout)) == 0
             ? 0
             : -1;
#else
  struct timeval timeout = {
      .tv_sec = BENCH_TIMEOUT_MS / 1000,
      .tv_usec = (BENCH_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                    &timeout, sizeof(timeout)) == 0 &&
                 setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                            &timeout, sizeof(timeout)) == 0
             ? 0
             : -1;
#endif
}

static int bench_connect(uint16_t port, bench_socket *out_socket) {
  struct sockaddr_in address;
  bench_socket socket_value;
  int one = 1;
  if (out_socket == NULL) return -1;
  *out_socket = BENCH_INVALID_SOCKET;
  socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_value == BENCH_INVALID_SOCKET) return -1;
  if (bench_socket_timeout(socket_value) != 0) {
    bench_close_socket(socket_value);
    return -1;
  }
  (void)setsockopt(socket_value, IPPROTO_TCP, TCP_NODELAY,
#if defined(_WIN32)
                   (const char *)&one,
#else
                   &one,
#endif
                   sizeof(one));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (connect(socket_value, (const struct sockaddr *)&address,
              (int)sizeof(address)) != 0) {
    bench_close_socket(socket_value);
    return -1;
  }
  *out_socket = socket_value;
  return 0;
}

static int bench_send_all(bench_socket socket_value,
                          const unsigned char *data, size_t size) {
  size_t offset = 0u;
  while (offset < size) {
    int sent = send(socket_value, (const char *)data + offset,
                    (int)(size - offset), 0);
    if (sent <= 0) {
#if !defined(_WIN32)
      if (sent < 0 && errno == EINTR) continue;
#endif
      return -1;
    }
    offset += (size_t)sent;
  }
  return 0;
}

static size_t bench_content_length(const char *buffer, size_t header_size) {
  static const char prefix[] = "Content-Length:";
  const char *cursor = buffer;
  const char *end = buffer + header_size;
  while (cursor < end) {
    const char *line_end = strstr(cursor, "\r\n");
    if (line_end == NULL || line_end > end) break;
    if ((size_t)(line_end - cursor) >= sizeof(prefix) - 1u &&
        memcmp(cursor, prefix, sizeof(prefix) - 1u) == 0) {
      const char *value = cursor + sizeof(prefix) - 1u;
      while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
      return (size_t)strtoul(value, NULL, 10);
    }
    cursor = line_end + 2;
  }
  return SIZE_MAX;
}

static int bench_receive_response(bench_socket socket_value) {
  char buffer[BENCH_RESPONSE_BYTES + 1u];
  size_t used = 0u;
  size_t header_size = 0u;
  size_t content_length = SIZE_MAX;
  for (;;) {
    if (used == BENCH_RESPONSE_BYTES) return -1;
    {
      int received = recv(socket_value, buffer + used,
                          (int)(BENCH_RESPONSE_BYTES - used), 0);
      if (received <= 0) {
#if !defined(_WIN32)
        if (received < 0 && errno == EINTR) continue;
#endif
        return -1;
      }
      used += (size_t)received;
    }
    if (header_size == 0u) {
      const char *end;
      buffer[used] = '\0';
      end = strstr(buffer, "\r\n\r\n");
      if (end != NULL) {
        header_size = (size_t)(end - buffer) + 4u;
        content_length = bench_content_length(buffer, header_size);
        if (content_length == SIZE_MAX) return -1;
      }
    }
    if (header_size != 0u && used >= header_size + content_length)
      return 0;
  }
}

static int bench_one_request(bench_socket socket_value, uint64_t *out_ns) {
  static const unsigned char request[] =
      "GET /bench HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: keep-alive\r\n\r\n";
  uint64_t started;
  if (out_ns == NULL) return -1;
  started = salts_hrtime();
  if (bench_send_all(socket_value, request, sizeof(request) - 1u) != 0)
    return -1;
  if (bench_receive_response(socket_value) != 0) return -1;
  *out_ns = salts_hrtime() - started;
  return 0;
}

static void bench_client_main(void *user) {
  bench_client *client = (bench_client *)user;
  bench_socket socket_value = BENCH_INVALID_SOCKET;
  size_t index;
  uint64_t ignored = 0u;
  bool announced = false;
  client->status = -1;
  if (bench_connect(client->port, &socket_value) != 0) goto cleanup;
  for (index = 0u; index < client->warmup; ++index)
    if (bench_one_request(socket_value, &ignored) != 0) goto cleanup;
  atomic_fetch_add_explicit(&client->barrier->ready, 1, memory_order_acq_rel);
  announced = true;
  while (atomic_load_explicit(&client->barrier->start,
                              memory_order_acquire) == 0)
    salts_thread_yield();
  for (index = 0u; index < client->requests; ++index)
    if (bench_one_request(socket_value, &client->latency[index]) != 0)
      goto cleanup;
  client->status = 0;

cleanup:
  if (!announced)
    atomic_fetch_add_explicit(&client->barrier->ready, 1, memory_order_acq_rel);
  if (socket_value != BENCH_INVALID_SOCKET)
    bench_close_socket(socket_value);
}

static int bench_run(size_t owners, size_t requests_per_connection,
                     size_t warmup) {
  chttp_server server = {0};
  chttp_server_config config = bench_server_config();
  chttp_server_execution_options execution =
      (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
  bench_barrier barrier;
  bench_client clients[BENCH_CONNECTIONS];
  salts_thread_t threads[BENCH_CONNECTIONS] = {0};
  uint64_t *latency = NULL;
  uint16_t port = 0u;
  uint64_t started_ns;
  uint64_t wall_ns;
  clock_t cpu_started;
  clock_t cpu_elapsed;
  size_t total_ops;
  size_t index;
  int status = 1;

  if (owners == 0u || owners > BENCH_CONNECTIONS) return 1;
  if (requests_per_connection > SIZE_MAX / BENCH_CONNECTIONS) return 1;
  total_ops = requests_per_connection * BENCH_CONNECTIONS;
  latency = (uint64_t *)calloc(total_ops, sizeof(*latency));
  if (latency == NULL) return 1;
  atomic_init(&barrier.ready, 0);
  atomic_init(&barrier.start, 0);
  memset(clients, 0, sizeof(clients));

  if (chttp_server_init(&server, &config) != SALTS_OK) goto cleanup;
  execution.owner_count = owners;
  if (chttp_server_set_execution_options(&server, &execution) != SALTS_OK)
    goto cleanup;
  if (chttp_server_get(&server, "/bench", bench_handler, NULL) != SALTS_OK)
    goto cleanup;
  if (chttp_server_start(&server) != SALTS_OK) goto cleanup;
  if (chttp_server_port(&server, &port) != SALTS_OK || port == 0u)
    goto cleanup;

  for (index = 0u; index < BENCH_CONNECTIONS; ++index) {
    clients[index] = (bench_client){
        .port = port,
        .warmup = warmup,
        .requests = requests_per_connection,
        .latency = latency + index * requests_per_connection,
        .barrier = &barrier,
        .status = -1};
    if (salts_thread_create(&threads[index], bench_client_main,
                            &clients[index]) != SALTS_OK)
      goto cleanup_threads;
  }
  while (atomic_load_explicit(&barrier.ready, memory_order_acquire) !=
         BENCH_CONNECTIONS)
    salts_thread_yield();

  cpu_started = clock();
  started_ns = salts_hrtime();
  atomic_store_explicit(&barrier.start, 1, memory_order_release);
  for (index = 0u; index < BENCH_CONNECTIONS; ++index) {
    if (salts_thread_join(&threads[index]) != SALTS_OK) goto cleanup_threads;
    salts_thread_destroy(&threads[index]);
    threads[index] = (salts_thread_t){0};
    if (clients[index].status != 0) goto cleanup_threads;
  }
  wall_ns = salts_hrtime() - started_ns;
  cpu_elapsed = clock() - cpu_started;

  printf(
      "{\"kind\":\"measurement\","
      "\"benchmark\":\"chttp_server_owner_scaling\","
      "\"owners\":%zu,\"connections\":%u,"
      "\"requests_per_connection\":%zu,\"operations\":%zu,"
      "\"wall_ns\":%llu,\"ops_per_second\":%.3f,"
      "\"cpu_ns_per_op\":%.3f,"
      "\"p50_ns\":%llu,\"p95_ns\":%llu,\"p99_ns\":%llu,"
      "\"errors\":0}\n",
      owners, BENCH_CONNECTIONS, requests_per_connection, total_ops,
      (unsigned long long)wall_ns,
      wall_ns != 0u ? (double)total_ops * 1.0e9 / (double)wall_ns : 0.0,
      total_ops != 0u
          ? ((double)cpu_elapsed * 1.0e9 / (double)CLOCKS_PER_SEC) /
                (double)total_ops
          : 0.0,
      (unsigned long long)bench_percentile(latency, total_ops, 50u),
      (unsigned long long)bench_percentile(latency, total_ops, 95u),
      (unsigned long long)bench_percentile(latency, total_ops, 99u));
  fflush(stdout);
  status = 0;

cleanup_threads:
  atomic_store_explicit(&barrier.start, 1, memory_order_release);
  for (index = 0u; index < BENCH_CONNECTIONS; ++index) {
    if (threads[index] != (salts_thread_t){0}) {
      (void)salts_thread_join(&threads[index]);
      salts_thread_destroy(&threads[index]);
    }
  }

cleanup:
  if (server.impl != NULL) {
    (void)chttp_server_stop(&server, BENCH_TIMEOUT_MS);
    (void)chttp_server_destroy(&server);
  }
  free(latency);
  return status;
}

int main(void) {
  static const size_t owners[] = {1u, 2u, 4u};
  const size_t requests =
      bench_env_count("CHTTP_OWNER_BENCH_REQUESTS", BENCH_REQUESTS_DEFAULT);
  const size_t warmup =
      bench_env_count("CHTTP_OWNER_BENCH_WARMUP", BENCH_WARMUP_DEFAULT);
  size_t index;
#if defined(_WIN32)
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
#endif

  printf(
      "{\"kind\":\"environment\","
      "\"benchmark\":\"chttp_server_owner_scaling\","
      "\"commit\":\"%s\",\"requests\":%zu,\"warmup\":%zu,"
      "\"note\":\"same-run 1/2/4-owner topology control; hosted-runner "
      "absolute values are not cross-machine rankings\"}\n",
      getenv("GITHUB_SHA") != NULL ? getenv("GITHUB_SHA") : "unknown",
      requests, warmup);
  fflush(stdout);

  for (index = 0u; index < sizeof(owners) / sizeof(owners[0]); ++index)
    if (bench_run(owners[index], requests, warmup) != 0) {
#if defined(_WIN32)
      WSACleanup();
#endif
      return 3;
    }

#if defined(_WIN32)
  WSACleanup();
#endif
  return 0;
}
