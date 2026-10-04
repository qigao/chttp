#include "chttp_server_runtime.h"

#include <http_server/http.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts_buffer.h>

#include <errno.h>
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
typedef SOCKET owner_bench_socket;
  #define OWNER_BENCH_INVALID_SOCKET INVALID_SOCKET
  #define owner_bench_close_socket closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int owner_bench_socket;
  #define OWNER_BENCH_INVALID_SOCKET (-1)
  #define owner_bench_close_socket close
#endif

enum {
  OWNER_BENCH_CONNECTIONS = 8,
  OWNER_BENCH_TIMEOUT_MS = 10000,
  OWNER_BENCH_HEADER_BYTES = 8192,
  OWNER_BENCH_SMALL_BYTES = 1024,
  OWNER_BENCH_RETAINED_BYTES = 64 * 1024,
  OWNER_BENCH_SMALL_REQUESTS = 300,
  OWNER_BENCH_RETAINED_REQUESTS = 60,
  OWNER_BENCH_WARMUP = 20
};

typedef enum owner_bench_workload_kind {
  OWNER_BENCH_SMALL_COPY = 1,
  OWNER_BENCH_RETAINED_64K
} owner_bench_workload_kind;

typedef struct owner_bench_route {
  owner_bench_workload_kind kind;
  const unsigned char *small_body;
  size_t small_size;
  mem_buffer_t *retained_body;
} owner_bench_route;

typedef struct owner_bench_barrier {
  atomic_int ready;
  atomic_int start;
} owner_bench_barrier;

typedef struct owner_bench_client {
  uint16_t port;
  const char *target;
  size_t expected_body_bytes;
  size_t warmup;
  size_t requests;
  uint64_t *latencies;
  owner_bench_barrier *barrier;
  atomic_int status;
} owner_bench_client;

typedef struct owner_bench_pressure {
  size_t owner_leases[4];
  size_t peak_owner_leases[4];
  size_t peak_owner_rings[4];
  size_t cross_owner_handoffs;
} owner_bench_pressure;

static unsigned char OWNER_BENCH_SMALL_BODY[OWNER_BENCH_SMALL_BYTES];
static unsigned char OWNER_BENCH_RETAINED_BODY[OWNER_BENCH_RETAINED_BYTES];

static int owner_bench_u64_compare(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t owner_bench_percentile(const uint64_t *values, size_t count,
                                       unsigned int percent) {
  uint64_t *ordered;
  size_t rank;
  uint64_t result;
  if (values == NULL || count == 0u) return 0u;
  ordered = (uint64_t *)malloc(count * sizeof(*ordered));
  if (ordered == NULL) return 0u;
  memcpy(ordered, values, count * sizeof(*ordered));
  qsort(ordered, count, sizeof(*ordered), owner_bench_u64_compare);
  rank = ((size_t)percent * count + 99u) / 100u;
  if (rank == 0u) rank = 1u;
  if (rank > count) rank = count;
  result = ordered[rank - 1u];
  free(ordered);
  return result;
}

static size_t owner_bench_env_count(const char *name, size_t fallback) {
  const char *value = getenv(name);
  char *end = NULL;
  unsigned long parsed;
  if (value == NULL || value[0] == '\0') return fallback;
  parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0ul) return fallback;
  return (size_t)parsed;
}

static native_io_backend_kind owner_bench_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config owner_bench_network_config(void) {
  return (cnet_client_config){
      .backend = owner_bench_backend(),
      .connection_capacity = 32u,
      .command_capacity = 64u,
      .request_capacity = 32u,
      .completion_batch_capacity = 32u,
      .event_capacity = 64u,
      .max_send_bytes = 256u * 1024u,
      .receive_buffer_bytes = 16u * 1024u,
      .connect_timeout_ms = OWNER_BENCH_TIMEOUT_MS,
      .read_timeout_ms = OWNER_BENCH_TIMEOUT_MS,
      .write_timeout_ms = OWNER_BENCH_TIMEOUT_MS};
}

static chttp_server_config owner_bench_server_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 64u,
      .network = owner_bench_network_config(),
      .route_capacity = 4u,
      .middleware_capacity = 1u,
      .max_route_middleware_count = 1u,
      .max_route_param_count = 1u,
      .max_route_param_bytes = 32u,
      .max_target_bytes = 128u,
      .max_header_count = 16u,
      .max_header_bytes = 2048u,
      .max_request_body_bytes = 1024u,
      .max_response_header_count = 16u,
      .max_response_header_bytes = 2048u,
      .max_response_body_bytes = 128u * 1024u,
      .session_capacity = 0u,
      .poll_slice_ms = 1u,
      .max_buffered_response_body_bytes = 128u * 1024u};
}

static int owner_bench_handler(void *user,
                               const chttp_server_request_view *request,
                               chttp_server_response *response) {
  owner_bench_route *route = (owner_bench_route *)user;
  (void)request;
  if (route == NULL) return SALTS_EINVAL;
  if (route->kind == OWNER_BENCH_RETAINED_64K)
    return chttp_server_reply_buffer(response, 200u, "application/octet-stream",
                                     route->retained_body);
  return chttp_server_reply(response, 200u, "application/octet-stream",
                            route->small_body, route->small_size);
}

static int owner_bench_socket_timeout(owner_bench_socket socket_value) {
#if defined(_WIN32)
  DWORD timeout = OWNER_BENCH_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                    (const char *)&timeout, sizeof(timeout)) == 0 &&
                 setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                            (const char *)&timeout, sizeof(timeout)) == 0
             ? 0
             : -1;
#else
  struct timeval timeout = {
      .tv_sec = OWNER_BENCH_TIMEOUT_MS / 1000,
      .tv_usec = (OWNER_BENCH_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                    &timeout, sizeof(timeout)) == 0 &&
                 setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                            &timeout, sizeof(timeout)) == 0
             ? 0
             : -1;
#endif
}

static int owner_bench_connect(uint16_t port,
                               owner_bench_socket *out_socket) {
  struct sockaddr_in address;
  owner_bench_socket socket_value;
  int one = 1;
  if (out_socket == NULL) return -1;
  *out_socket = OWNER_BENCH_INVALID_SOCKET;
  socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_value == OWNER_BENCH_INVALID_SOCKET) return -1;
  if (owner_bench_socket_timeout(socket_value) != 0) {
    owner_bench_close_socket(socket_value);
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
    owner_bench_close_socket(socket_value);
    return -1;
  }
  *out_socket = socket_value;
  return 0;
}

static int owner_bench_send_all(owner_bench_socket socket_value,
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

static int owner_bench_recv_some(owner_bench_socket socket_value,
                                 unsigned char *data, size_t capacity,
                                 size_t *out_size) {
  int received;
  if (out_size == NULL || data == NULL || capacity == 0u) return -1;
  *out_size = 0u;
  do {
    received = recv(socket_value, (char *)data, (int)capacity, 0);
#if !defined(_WIN32)
  } while (received < 0 && errno == EINTR);
#else
  } while (false);
#endif
  if (received <= 0) return -1;
  *out_size = (size_t)received;
  return 0;
}

static size_t owner_bench_content_length(const char *headers) {
  const char *cursor = headers;
  static const char name[] = "Content-Length:";
  while (cursor != NULL && *cursor != '\0') {
    const char *line_end = strstr(cursor, "\r\n");
    if (line_end == NULL) break;
    if ((size_t)(line_end - cursor) >= sizeof(name) - 1u &&
        memcmp(cursor, name, sizeof(name) - 1u) == 0) {
      const char *value = cursor + sizeof(name) - 1u;
      while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
      return (size_t)strtoull(value, NULL, 10);
    }
    cursor = line_end + 2u;
  }
  return SIZE_MAX;
}

static int owner_bench_receive_response(owner_bench_socket socket_value,
                                        size_t expected_body_bytes) {
  unsigned char headers[OWNER_BENCH_HEADER_BYTES + 1u];
  unsigned char body_chunk[16u * 1024u];
  size_t used = 0u;
  size_t body_already = 0u;
  size_t content_length = SIZE_MAX;
  for (;;) {
    size_t got = 0u;
    if (used == OWNER_BENCH_HEADER_BYTES) return -1;
    if (owner_bench_recv_some(socket_value, headers + used,
                              OWNER_BENCH_HEADER_BYTES - used, &got) != 0)
      return -1;
    used += got;
    headers[used] = '\0';
    {
      unsigned char *end =
          (unsigned char *)strstr((const char *)headers, "\r\n\r\n");
      if (end != NULL) {
        const size_t header_bytes = (size_t)(end - headers) + 4u;
        content_length = owner_bench_content_length((const char *)headers);
        if (content_length == SIZE_MAX ||
            content_length != expected_body_bytes)
          return -1;
        if (used > header_bytes) body_already = used - header_bytes;
        break;
      }
    }
  }
  while (body_already < content_length) {
    size_t got = 0u;
    const size_t remaining = content_length - body_already;
    const size_t capacity =
        remaining < sizeof(body_chunk) ? remaining : sizeof(body_chunk);
    if (owner_bench_recv_some(socket_value, body_chunk, capacity, &got) != 0)
      return -1;
    body_already += got;
  }
  return body_already == content_length ? 0 : -1;
}

static int owner_bench_one_request(owner_bench_socket socket_value,
                                   const char *target,
                                   size_t expected_body_bytes,
                                   uint64_t *out_ns) {
  unsigned char request[256];
  int request_size;
  uint64_t started;
  if (target == NULL || out_ns == NULL) return -1;
  request_size = snprintf(
      (char *)request, sizeof(request),
      "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n",
      target);
  if (request_size <= 0 || (size_t)request_size >= sizeof(request))
    return -1;
  started = salts_hrtime();
  if (owner_bench_send_all(socket_value, request, (size_t)request_size) != 0)
    return -1;
  if (owner_bench_receive_response(socket_value, expected_body_bytes) != 0)
    return -1;
  *out_ns = salts_hrtime() - started;
  return 0;
}

static void owner_bench_client_main(void *user) {
  owner_bench_client *client = (owner_bench_client *)user;
  owner_bench_socket socket_value = OWNER_BENCH_INVALID_SOCKET;
  size_t index;
  uint64_t ignored = 0u;
  bool announced = false;
  atomic_store_explicit(&client->status, SALTS_EIO, memory_order_release);
  if (owner_bench_connect(client->port, &socket_value) != 0) goto cleanup;
  for (index = 0u; index < client->warmup; ++index)
    if (owner_bench_one_request(socket_value, client->target,
                                client->expected_body_bytes, &ignored) != 0)
      goto cleanup;
  atomic_store_explicit(&client->status, SALTS_OK, memory_order_release);
  atomic_fetch_add_explicit(&client->barrier->ready, 1, memory_order_acq_rel);
  announced = true;
  while (atomic_load_explicit(&client->barrier->start,
                              memory_order_acquire) == 0)
    salts_thread_yield();
  for (index = 0u; index < client->requests; ++index) {
    if (owner_bench_one_request(socket_value, client->target,
                                client->expected_body_bytes,
                                &client->latencies[index]) != 0) {
      atomic_store_explicit(&client->status, SALTS_EIO, memory_order_release);
      goto cleanup;
    }
  }

cleanup:
  if (!announced)
    atomic_fetch_add_explicit(&client->barrier->ready, 1, memory_order_acq_rel);
  if (socket_value != OWNER_BENCH_INVALID_SOCKET)
    owner_bench_close_socket(socket_value);
}

static void owner_bench_sample_pressure(chttp_server_impl *impl,
                                        owner_bench_pressure *pressure,
                                        size_t owner_count) {
  size_t index;
  if (impl == NULL || pressure == NULL) return;
  for (index = 0u; index < owner_count && index < 4u; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(impl, index);
    size_t leases;
    size_t ring;
    if (owner == NULL) continue;
    leases = chttp_server_owner_lease_count(owner);
    salts_mutex_lock(&owner->admission_mutex);
    ring = owner->admission_count;
    salts_mutex_unlock(&owner->admission_mutex);
    pressure->owner_leases[index] = leases;
    if (leases > pressure->peak_owner_leases[index])
      pressure->peak_owner_leases[index] = leases;
    if (ring > pressure->peak_owner_rings[index])
      pressure->peak_owner_rings[index] = ring;
  }
}

static int owner_bench_run(owner_bench_workload_kind kind,
                           const char *name, const char *target,
                           size_t payload_bytes, size_t owners,
                           size_t requests_per_connection, size_t warmup,
                           owner_bench_route *route) {
  chttp_server server = {0};
  chttp_server_config config = owner_bench_server_config();
  chttp_server_execution_options execution =
      (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
  chttp_server_stats stats = {0};
  chttp_server_impl *impl;
  owner_bench_barrier barrier;
  owner_bench_client clients[OWNER_BENCH_CONNECTIONS];
  salts_thread_t threads[OWNER_BENCH_CONNECTIONS] = {0};
  bool thread_started[OWNER_BENCH_CONNECTIONS] = {false};
  owner_bench_pressure pressure = {0};
  uint64_t *latencies = NULL;
  uint16_t port = 0u;
  uint64_t started_ns;
  uint64_t wall_ns;
  clock_t cpu_started;
  clock_t cpu_elapsed;
  size_t total_ops;
  size_t index;
  int result = 1;

  if (owners == 0u || owners > 4u ||
      owners > config.network.connection_capacity)
    return 1;
  if (requests_per_connection > SIZE_MAX / OWNER_BENCH_CONNECTIONS)
    return 1;
  total_ops = requests_per_connection * OWNER_BENCH_CONNECTIONS;
  latencies = (uint64_t *)calloc(total_ops, sizeof(*latencies));
  if (latencies == NULL) return 1;
  atomic_init(&barrier.ready, 0);
  atomic_init(&barrier.start, 0);
  memset(clients, 0, sizeof(clients));

  if (chttp_server_init(&server, &config) != SALTS_OK) goto cleanup;
  execution.owner_count = owners;
  if (chttp_server_set_execution_options(&server, &execution) != SALTS_OK)
    goto cleanup;
  if (chttp_server_get(&server, target, owner_bench_handler, route) != SALTS_OK)
    goto cleanup;
  if (chttp_server_start(&server) != SALTS_OK) goto cleanup;
  if (chttp_server_port(&server, &port) != SALTS_OK || port == 0u)
    goto cleanup;
  impl = (chttp_server_impl *)server.impl;

  for (index = 0u; index < OWNER_BENCH_CONNECTIONS; ++index) {
    clients[index] = (owner_bench_client){
        .port = port,
        .target = target,
        .expected_body_bytes = payload_bytes,
        .warmup = warmup,
        .requests = requests_per_connection,
        .latencies = latencies + index * requests_per_connection,
        .barrier = &barrier};
    atomic_init(&clients[index].status, SALTS_EIO);
    if (salts_thread_create(&threads[index], owner_bench_client_main,
                            &clients[index]) != SALTS_OK)
      goto cleanup_threads;
    thread_started[index] = true;
  }

  while (atomic_load_explicit(&barrier.ready, memory_order_acquire) !=
         OWNER_BENCH_CONNECTIONS) {
    owner_bench_sample_pressure(impl, &pressure, owners);
    salts_thread_yield();
  }
  owner_bench_sample_pressure(impl, &pressure, owners);
  for (index = 0u; index < OWNER_BENCH_CONNECTIONS; ++index)
    if (atomic_load_explicit(&clients[index].status,
                             memory_order_acquire) != SALTS_OK)
      goto cleanup_threads;

  pressure.cross_owner_handoffs = 0u;
  for (index = 1u; index < owners; ++index)
    pressure.cross_owner_handoffs += pressure.owner_leases[index];

  cpu_started = clock();
  started_ns = salts_hrtime();
  atomic_store_explicit(&barrier.start, 1, memory_order_release);

  for (index = 0u; index < OWNER_BENCH_CONNECTIONS; ++index) {
    if (salts_thread_join(&threads[index]) != SALTS_OK)
      goto cleanup_threads;
    salts_thread_destroy(&threads[index]);
    thread_started[index] = false;
    if (atomic_load_explicit(&clients[index].status,
                             memory_order_acquire) != SALTS_OK)
      goto cleanup_threads;
  }

  wall_ns = salts_hrtime() - started_ns;
  cpu_elapsed = clock() - cpu_started;
  if (chttp_server_get_stats(&server, &stats) != SALTS_OK) goto cleanup;
  if (stats.rejected_connections != 0u) goto cleanup;

  printf(
      "{\"kind\":\"measurement\","
      "\"benchmark\":\"chttp_server_owner_scaling\","
      "\"protocol\":\"h1\","
      "\"transport\":\"tcp\","
      "\"workload\":\"%s\","
      "\"payload_bytes\":%zu,"
      "\"retained\":%s,"
      "\"owners\":%zu,"
      "\"connections\":%u,"
      "\"requests_per_connection\":%zu,"
      "\"operations\":%zu,"
      "\"samples\":%zu,"
      "\"wall_ns\":%llu,"
      "\"ops_per_second\":%.3f,"
      "\"cpu_ns_per_op\":%.3f,"
      "\"p50_ns\":%llu,"
      "\"p95_ns\":%llu,"
      "\"p99_ns\":%llu,"
      "\"accepted_connections\":%llu,"
      "\"rejected_connections\":%llu,"
      "\"owner0_leases\":%zu,"
      "\"owner1_leases\":%zu,"
      "\"owner2_leases\":%zu,"
      "\"owner3_leases\":%zu,"
      "\"peak_owner0_ring\":%zu,"
      "\"peak_owner1_ring\":%zu,"
      "\"peak_owner2_ring\":%zu,"
      "\"peak_owner3_ring\":%zu,"
      "\"cross_owner_admission_handoffs\":%zu,"
      "\"cross_owner_data_plane_hops\":0,"
      "\"errors\":0}\n",
      name, payload_bytes,
      kind == OWNER_BENCH_RETAINED_64K ? "true" : "false",
      owners, OWNER_BENCH_CONNECTIONS, requests_per_connection, total_ops,
      total_ops, (unsigned long long)wall_ns,
      wall_ns != 0u ? (double)total_ops * 1.0e9 / (double)wall_ns : 0.0,
      total_ops != 0u
          ? ((double)cpu_elapsed * 1.0e9 / (double)CLOCKS_PER_SEC) /
                (double)total_ops
          : 0.0,
      (unsigned long long)owner_bench_percentile(latencies, total_ops, 50u),
      (unsigned long long)owner_bench_percentile(latencies, total_ops, 95u),
      (unsigned long long)owner_bench_percentile(latencies, total_ops, 99u),
      (unsigned long long)stats.accepted_connections,
      (unsigned long long)stats.rejected_connections,
      pressure.owner_leases[0], pressure.owner_leases[1],
      pressure.owner_leases[2], pressure.owner_leases[3],
      pressure.peak_owner_rings[0], pressure.peak_owner_rings[1],
      pressure.peak_owner_rings[2], pressure.peak_owner_rings[3],
      pressure.cross_owner_handoffs);
  fflush(stdout);
  result = 0;

cleanup_threads:
  atomic_store_explicit(&barrier.start, 1, memory_order_release);
  for (index = 0u; index < OWNER_BENCH_CONNECTIONS; ++index) {
    if (thread_started[index]) {
      (void)salts_thread_join(&threads[index]);
      salts_thread_destroy(&threads[index]);
      thread_started[index] = false;
    }
  }

cleanup:
  if (server.impl != NULL) {
    (void)chttp_server_stop(&server, OWNER_BENCH_TIMEOUT_MS);
    (void)chttp_server_destroy(&server);
  }
  free(latencies);
  return result;
}

int main(void) {
  static const size_t owners[] = {1u, 2u, 4u};
  const size_t small_requests =
      owner_bench_env_count("CHTTP_OWNER_BENCH_SMALL_REQUESTS",
                            OWNER_BENCH_SMALL_REQUESTS);
  const size_t retained_requests =
      owner_bench_env_count("CHTTP_OWNER_BENCH_RETAINED_REQUESTS",
                            OWNER_BENCH_RETAINED_REQUESTS);
  const size_t warmup =
      owner_bench_env_count("CHTTP_OWNER_BENCH_WARMUP",
                            OWNER_BENCH_WARMUP);
  owner_bench_route small_route;
  owner_bench_route retained_route;
  mem_buffer_t *retained = NULL;
  size_t index;
  int result = 0;

#if defined(_WIN32)
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
#endif

  memset(OWNER_BENCH_SMALL_BODY, 's', sizeof(OWNER_BENCH_SMALL_BODY));
  memset(OWNER_BENCH_RETAINED_BODY, 'r', sizeof(OWNER_BENCH_RETAINED_BODY));
  retained = mem_wrap_external(OWNER_BENCH_RETAINED_BODY,
                               sizeof(OWNER_BENCH_RETAINED_BODY),
                               NULL, NULL);
  if (retained == NULL) {
    result = 3;
    goto cleanup;
  }
  mem_set_used(retained, sizeof(OWNER_BENCH_RETAINED_BODY));

  small_route = (owner_bench_route){
      .kind = OWNER_BENCH_SMALL_COPY,
      .small_body = OWNER_BENCH_SMALL_BODY,
      .small_size = sizeof(OWNER_BENCH_SMALL_BODY)};
  retained_route = (owner_bench_route){
      .kind = OWNER_BENCH_RETAINED_64K,
      .retained_body = retained};

  printf(
      "{\"kind\":\"environment\","
      "\"benchmark\":\"chttp_server_owner_scaling\","
      "\"commit\":\"%s\","
      "\"backend\":\"%s\","
      "\"connections\":%u,"
      "\"small_requests\":%zu,"
      "\"retained_requests\":%zu,"
      "\"warmup\":%zu,"
      "\"note\":\"same-run 1/2/4 owner evidence; absolute hosted-runner values are not cross-machine rankings\"}\n",
      getenv("GITHUB_SHA") != NULL ? getenv("GITHUB_SHA") : "unknown",
#if defined(_WIN32)
      "iocp",
#elif defined(__linux__)
      "epoll",
#else
      "kqueue",
#endif
      OWNER_BENCH_CONNECTIONS, small_requests, retained_requests, warmup);
  fflush(stdout);

  for (index = 0u; index < sizeof(owners) / sizeof(owners[0]); ++index) {
    if (owner_bench_run(OWNER_BENCH_SMALL_COPY, "1k-copy", "/copy-1k",
                        sizeof(OWNER_BENCH_SMALL_BODY), owners[index],
                        small_requests, warmup, &small_route) != 0) {
      fprintf(stderr, "owner benchmark failed workload=1k-copy owners=%zu\n",
              owners[index]);
      result = 4;
      goto cleanup;
    }
  }
  for (index = 0u; index < sizeof(owners) / sizeof(owners[0]); ++index) {
    if (owner_bench_run(OWNER_BENCH_RETAINED_64K, "64k-retained",
                        "/retained-64k",
                        sizeof(OWNER_BENCH_RETAINED_BODY), owners[index],
                        retained_requests, warmup, &retained_route) != 0) {
      fprintf(stderr, "owner benchmark failed workload=64k-retained owners=%zu\n",
              owners[index]);
      result = 5;
      goto cleanup;
    }
  }

cleanup:
  mem_buffer_release(retained);
#if defined(_WIN32)
  WSACleanup();
#endif
  return result;
}
