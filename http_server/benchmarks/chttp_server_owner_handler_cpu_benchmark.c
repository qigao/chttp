#include "chttp_server_runtime.h"

#include <http_server/http.h>
#include <salts/clock.h>
#include <salts/thread.h>

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
typedef SOCKET owner_cpu_socket;
  #define OWNER_CPU_INVALID_SOCKET INVALID_SOCKET
  #define owner_cpu_close closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int owner_cpu_socket;
  #define OWNER_CPU_INVALID_SOCKET (-1)
  #define owner_cpu_close close
#endif

enum {
  OWNER_CPU_CONNECTIONS = 8,
  OWNER_CPU_DEFAULT_REQUESTS = 30,
  OWNER_CPU_WARMUP = 2,
  OWNER_CPU_TIMEOUT_MS = 10000,
  OWNER_CPU_MAX_OWNERS = 4,
  OWNER_CPU_MAX_REQUESTS = 128
};

typedef struct owner_cpu_route {
  size_t burn_iterations;
} owner_cpu_route;

typedef struct owner_cpu_barrier {
  atomic_int ready;
  atomic_int start;
} owner_cpu_barrier;

typedef struct owner_cpu_client {
  owner_cpu_socket socket_value;
  const char *target;
  size_t requests;
  uint64_t *latencies;
  owner_cpu_barrier *barrier;
  atomic_int status;
} owner_cpu_client;

static atomic_uint_fast64_t OWNER_CPU_SINK;

static uint64_t owner_cpu_burn(size_t iterations) {
  uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
  size_t index;
  for (index = 0u; index < iterations; ++index) {
    state ^= state >> 12u;
    state ^= state << 25u;
    state ^= state >> 27u;
    state *= UINT64_C(2685821657736338717);
  }
  atomic_fetch_xor_explicit(&OWNER_CPU_SINK, state, memory_order_relaxed);
  return state;
}

static size_t owner_cpu_calibrate(uint64_t target_ns, uint64_t *out_measured_ns) {
  size_t iterations = 256u;
  uint64_t elapsed = 0u;
  uint64_t started;
  if (out_measured_ns != NULL) *out_measured_ns = 0u;
  if (target_ns == 0u) return 0u;

  while (iterations <= (SIZE_MAX / 2u)) {
    started = cmeta_hrtime();
    (void)owner_cpu_burn(iterations);
    elapsed = cmeta_hrtime() - started;
    if (elapsed >= UINT64_C(2000000)) break;
    iterations *= 2u;
  }
  if (elapsed == 0u) return 1u;

  {
    long double scaled =
        ((long double)iterations * (long double)target_ns) / (long double)elapsed;
    if (scaled < 1.0L) iterations = 1u;
    else if (scaled > (long double)(SIZE_MAX / 4u)) iterations = SIZE_MAX / 4u;
    else iterations = (size_t)scaled;
  }

  started = cmeta_hrtime();
  (void)owner_cpu_burn(iterations);
  elapsed = cmeta_hrtime() - started;
  if (out_measured_ns != NULL) *out_measured_ns = elapsed;
  return iterations;
}

static int owner_cpu_u64_compare(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t owner_cpu_percentile(const uint64_t *values, size_t count,
                                     unsigned int percent) {
  uint64_t *ordered;
  size_t rank;
  uint64_t result;
  if (values == NULL || count == 0u) return 0u;
  ordered = (uint64_t *)malloc(count * sizeof(*ordered));
  if (ordered == NULL) return 0u;
  memcpy(ordered, values, count * sizeof(*ordered));
  qsort(ordered, count, sizeof(*ordered), owner_cpu_u64_compare);
  rank = ((size_t)percent * count + 99u) / 100u;
  if (rank == 0u) rank = 1u;
  if (rank > count) rank = count;
  result = ordered[rank - 1u];
  free(ordered);
  return result;
}

static size_t owner_cpu_env_count(const char *name, size_t fallback) {
  const char *value = getenv(name);
  char *end = NULL;
  unsigned long parsed;
  if (value == NULL || value[0] == '\0') return fallback;
  parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0ul || parsed > OWNER_CPU_MAX_REQUESTS)
    return fallback;
  return (size_t)parsed;
}

static native_io_backend_kind owner_cpu_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_server_config owner_cpu_server_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 64u,
      .network = {
          .backend = owner_cpu_backend(),
          .connection_capacity = 16u,
          .command_capacity = 64u,
          .request_capacity = 32u,
          .completion_batch_capacity = 32u,
          .event_capacity = 64u,
          .max_send_bytes = 64u * 1024u,
          .receive_buffer_bytes = 16u * 1024u,
          .connect_timeout_ms = OWNER_CPU_TIMEOUT_MS,
          .read_timeout_ms = OWNER_CPU_TIMEOUT_MS,
          .write_timeout_ms = OWNER_CPU_TIMEOUT_MS},
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
      .max_response_body_bytes = 1024u,
      .poll_slice_ms = 1u,
      .max_buffered_response_body_bytes = 1024u,
      .buffer_capacity_bytes = 2u * 1024u * 1024u};
}

static int owner_cpu_handler(void *user,
                             const chttp_server_request_view *request,
                             chttp_server_response *response) {
  static const char body[] = "ok";
  owner_cpu_route *route = (owner_cpu_route *)user;
  (void)request;
  if (route == NULL) return SALTS_EINVAL;
  if (route->burn_iterations != 0u)
    (void)owner_cpu_burn(route->burn_iterations);
  return chttp_server_reply(response, 200u, "text/plain", body, sizeof(body) - 1u);
}

static int owner_cpu_socket_timeout(owner_cpu_socket socket_value) {
#if defined(_WIN32)
  DWORD timeout = OWNER_CPU_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                    (const char *)&timeout, sizeof(timeout)) == 0 &&
                 setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                            (const char *)&timeout, sizeof(timeout)) == 0
             ? 0
             : -1;
#else
  struct timeval timeout = {
      .tv_sec = OWNER_CPU_TIMEOUT_MS / 1000,
      .tv_usec = (OWNER_CPU_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                    &timeout, sizeof(timeout)) == 0 &&
                 setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                            &timeout, sizeof(timeout)) == 0
             ? 0
             : -1;
#endif
}

static int owner_cpu_connect(uint16_t port, owner_cpu_socket *out_socket) {
  struct sockaddr_in address;
  owner_cpu_socket socket_value;
  int one = 1;
  if (out_socket == NULL) return -1;
  *out_socket = OWNER_CPU_INVALID_SOCKET;
  socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_value == OWNER_CPU_INVALID_SOCKET) return -1;
  if (owner_cpu_socket_timeout(socket_value) != 0) goto fail;
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
              (int)sizeof(address)) != 0)
    goto fail;
  *out_socket = socket_value;
  return 0;
fail:
  owner_cpu_close(socket_value);
  return -1;
}

static int owner_cpu_send_all(owner_cpu_socket socket_value,
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

static int owner_cpu_request(owner_cpu_socket socket_value, const char *target,
                             uint64_t *out_ns) {
  unsigned char request[256];
  unsigned char response[4096];
  size_t used = 0u;
  int request_size;
  uint64_t started;
  if (target == NULL || out_ns == NULL) return -1;
  request_size = snprintf(
      (char *)request, sizeof(request),
      "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n",
      target);
  if (request_size <= 0 || (size_t)request_size >= sizeof(request)) return -1;
  started = cmeta_hrtime();
  if (owner_cpu_send_all(socket_value, request, (size_t)request_size) != 0) return -1;
  while (used < sizeof(response) - 1u) {
    int got = recv(socket_value, (char *)response + used,
                   (int)(sizeof(response) - 1u - used), 0);
    if (got <= 0) {
#if !defined(_WIN32)
      if (got < 0 && errno == EINTR) continue;
#endif
      return -1;
    }
    used += (size_t)got;
    response[used] = '\0';
    if (strstr((const char *)response, "\r\n\r\nok") != NULL) {
      *out_ns = cmeta_hrtime() - started;
      return 0;
    }
  }
  return -1;
}

static void owner_cpu_snapshot(chttp_server_impl *impl, size_t owner_count,
                               size_t leases[OWNER_CPU_MAX_OWNERS]) {
  size_t index;
  memset(leases, 0, OWNER_CPU_MAX_OWNERS * sizeof(*leases));
  for (index = 0u; index < owner_count; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(impl, index);
    if (owner != NULL) leases[index] = chttp_server_owner_lease_count(owner);
  }
}

static size_t owner_cpu_sum(const size_t values[OWNER_CPU_MAX_OWNERS],
                            size_t owner_count) {
  size_t index;
  size_t total = 0u;
  for (index = 0u; index < owner_count; ++index) total += values[index];
  return total;
}

static int owner_cpu_wait_total(chttp_server_impl *impl, size_t owner_count,
                                size_t expected,
                                size_t leases[OWNER_CPU_MAX_OWNERS]) {
  const uint64_t deadline = cmeta_monotonic_ms() + OWNER_CPU_TIMEOUT_MS;
  do {
    owner_cpu_snapshot(impl, owner_count, leases);
    if (owner_cpu_sum(leases, owner_count) == expected) return 0;
    cmeta_thread_yield();
  } while (cmeta_monotonic_ms() < deadline);
  return -1;
}

static int owner_cpu_delta_owner(
    const size_t before[OWNER_CPU_MAX_OWNERS],
    const size_t after[OWNER_CPU_MAX_OWNERS], size_t owner_count,
    size_t *out_owner) {
  size_t index;
  size_t changed = SIZE_MAX;
  if (out_owner == NULL) return -1;
  for (index = 0u; index < owner_count; ++index) {
    if (after[index] == before[index]) continue;
    if (after[index] != before[index] + 1u || changed != SIZE_MAX) return -1;
    changed = index;
  }
  if (changed == SIZE_MAX) return -1;
  *out_owner = changed;
  return 0;
}

static void owner_cpu_client_main(void *user) {
  owner_cpu_client *client = (owner_cpu_client *)user;
  size_t index;
  atomic_store_explicit(&client->status, SALTS_OK, memory_order_release);
  atomic_fetch_add_explicit(&client->barrier->ready, 1, memory_order_acq_rel);
  while (atomic_load_explicit(&client->barrier->start, memory_order_acquire) == 0)
    cmeta_thread_yield();
  for (index = 0u; index < client->requests; ++index) {
    if (owner_cpu_request(client->socket_value, client->target,
                          &client->latencies[index]) != 0) {
      atomic_store_explicit(&client->status, SALTS_EIO, memory_order_release);
      return;
    }
  }
}

static int owner_cpu_run(size_t owner_count, uint64_t target_work_ns,
                         size_t requests_per_connection) {
  chttp_server server = {0};
  chttp_server_config config = owner_cpu_server_config();
  chttp_server_execution_options execution =
      (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
  chttp_server_stats stats = {0};
  chttp_server_impl *impl = NULL;
  owner_cpu_route fast_route = {0};
  owner_cpu_route slow_route = {0};
  owner_cpu_socket sockets[OWNER_CPU_CONNECTIONS];
  size_t leases[OWNER_CPU_MAX_OWNERS] = {0};
  size_t before[OWNER_CPU_MAX_OWNERS] = {0};
  owner_cpu_client clients[OWNER_CPU_CONNECTIONS];
  cmeta_thread_t threads[OWNER_CPU_CONNECTIONS] = {0};
  bool thread_started[OWNER_CPU_CONNECTIONS] = {false};
  owner_cpu_barrier barrier;
  uint64_t latencies[OWNER_CPU_CONNECTIONS][OWNER_CPU_MAX_REQUESTS] = {{0}};
  uint64_t fast_latencies[(OWNER_CPU_CONNECTIONS / 2) * OWNER_CPU_MAX_REQUESTS];
  uint64_t slow_latencies[(OWNER_CPU_CONNECTIONS / 2) * OWNER_CPU_MAX_REQUESTS];
  uint64_t calibrated_work_ns = 0u;
  uint16_t port = 0u;
  uint64_t started_ns;
  uint64_t wall_ns;
  clock_t cpu_started;
  clock_t cpu_elapsed;
  size_t fast_count = 0u;
  size_t slow_count = 0u;
  unsigned int fast_owner_mask = 0u;
  unsigned int slow_owner_mask = 0u;
  size_t index;
  int result = 1;

  if (owner_count == 0u || owner_count > OWNER_CPU_MAX_OWNERS ||
      requests_per_connection == 0u ||
      requests_per_connection > OWNER_CPU_MAX_REQUESTS)
    return 1;
  for (index = 0u; index < OWNER_CPU_CONNECTIONS; ++index)
    sockets[index] = OWNER_CPU_INVALID_SOCKET;
  memset(clients, 0, sizeof(clients));
  atomic_init(&barrier.ready, 0);
  atomic_init(&barrier.start, 0);

  slow_route.burn_iterations =
      owner_cpu_calibrate(target_work_ns, &calibrated_work_ns);

  if (chttp_server_init(&server, &config) != SALTS_OK) goto cleanup;
  execution.owner_count = owner_count;
  if (chttp_server_set_execution_options(&server, &execution) != SALTS_OK) goto cleanup;
  if (chttp_server_get(&server, "/fast", owner_cpu_handler, &fast_route) != SALTS_OK)
    goto cleanup;
  if (chttp_server_get(&server, "/slow", owner_cpu_handler, &slow_route) != SALTS_OK)
    goto cleanup;
  if (chttp_server_start(&server) != SALTS_OK) goto cleanup;
  if (chttp_server_port(&server, &port) != SALTS_OK || port == 0u) goto cleanup;
  impl = (chttp_server_impl *)server.impl;

  for (index = 0u; index < OWNER_CPU_CONNECTIONS; ++index) {
    uint64_t ignored;
    const bool slow = (index % 2u) == 0u;
    const char *target = slow ? "/slow" : "/fast";
    size_t assigned;
    size_t warmup;

    owner_cpu_snapshot(impl, owner_count, before);
    if (owner_cpu_connect(port, &sockets[index]) != 0) goto cleanup;
    for (warmup = 0u; warmup < OWNER_CPU_WARMUP; ++warmup)
      if (owner_cpu_request(sockets[index], target, &ignored) != 0) goto cleanup;
    if (owner_cpu_wait_total(impl, owner_count, index + 1u, leases) != 0) goto cleanup;
    if (owner_cpu_delta_owner(before, leases, owner_count, &assigned) != 0) goto cleanup;
    if (slow)
      slow_owner_mask |= 1u << assigned;
    else
      fast_owner_mask |= 1u << assigned;
  }

  if (owner_count > 1u && (slow_owner_mask & fast_owner_mask) != 0u) goto cleanup;

  for (index = 0u; index < OWNER_CPU_CONNECTIONS; ++index) {
    clients[index] = (owner_cpu_client){
        .socket_value = sockets[index],
        .target = (index % 2u) == 0u ? "/slow" : "/fast",
        .requests = requests_per_connection,
        .latencies = latencies[index],
        .barrier = &barrier};
    atomic_init(&clients[index].status, SALTS_EIO);
    if (cmeta_thread_create(&threads[index], owner_cpu_client_main, &clients[index]) != SALTS_OK)
      goto cleanup_threads;
    thread_started[index] = true;
  }

  while (atomic_load_explicit(&barrier.ready, memory_order_acquire) !=
         OWNER_CPU_CONNECTIONS)
    cmeta_thread_yield();

  cpu_started = clock();
  started_ns = cmeta_hrtime();
  atomic_store_explicit(&barrier.start, 1, memory_order_release);

  for (index = 0u; index < OWNER_CPU_CONNECTIONS; ++index) {
    size_t sample;
    if (cmeta_thread_join(&threads[index]) != SALTS_OK) goto cleanup_threads;
    cmeta_thread_destroy(&threads[index]);
    thread_started[index] = false;
    if (atomic_load_explicit(&clients[index].status, memory_order_acquire) != SALTS_OK)
      goto cleanup_threads;
    for (sample = 0u; sample < requests_per_connection; ++sample) {
      if ((index % 2u) == 0u)
        slow_latencies[slow_count++] = latencies[index][sample];
      else
        fast_latencies[fast_count++] = latencies[index][sample];
    }
  }

  wall_ns = cmeta_hrtime() - started_ns;
  cpu_elapsed = clock() - cpu_started;
  if (chttp_server_get_stats(&server, &stats) != SALTS_OK ||
      stats.rejected_connections != 0u)
    goto cleanup;

  printf(
      "{\"kind\":\"measurement\","
      "\"benchmark\":\"chttp_server_owner_handler_cpu\","
      "\"owners\":%zu,"
      "\"connections\":%u,"
      "\"requests_per_connection\":%zu,"
      "\"target_work_ns\":%llu,"
      "\"calibrated_work_ns\":%llu,"
      "\"burn_iterations\":%zu,"
      "\"fast_owner_mask\":%u,"
      "\"slow_owner_mask\":%u,"
      "\"owner0_leases\":%zu,"
      "\"owner1_leases\":%zu,"
      "\"owner2_leases\":%zu,"
      "\"owner3_leases\":%zu,"
      "\"fast_samples\":%zu,"
      "\"slow_samples\":%zu,"
      "\"ops_per_second\":%.3f,"
      "\"cpu_ns_per_op\":%.3f,"
      "\"fast_p50_ns\":%llu,"
      "\"fast_p95_ns\":%llu,"
      "\"fast_p99_ns\":%llu,"
      "\"slow_p50_ns\":%llu,"
      "\"slow_p95_ns\":%llu,"
      "\"slow_p99_ns\":%llu,"
      "\"accepted_connections\":%llu,"
      "\"rejected_connections\":%llu,"
      "\"cross_owner_data_plane_hops\":0,"
      "\"errors\":0}\n",
      owner_count, OWNER_CPU_CONNECTIONS, requests_per_connection,
      (unsigned long long)target_work_ns,
      (unsigned long long)calibrated_work_ns,
      slow_route.burn_iterations, fast_owner_mask, slow_owner_mask,
      leases[0], leases[1], leases[2], leases[3],
      fast_count, slow_count,
      wall_ns != 0u
          ? (double)(fast_count + slow_count) * 1.0e9 / (double)wall_ns
          : 0.0,
      (fast_count + slow_count) != 0u
          ? ((double)cpu_elapsed * 1.0e9 / (double)CLOCKS_PER_SEC) /
                (double)(fast_count + slow_count)
          : 0.0,
      (unsigned long long)owner_cpu_percentile(fast_latencies, fast_count, 50u),
      (unsigned long long)owner_cpu_percentile(fast_latencies, fast_count, 95u),
      (unsigned long long)owner_cpu_percentile(fast_latencies, fast_count, 99u),
      (unsigned long long)owner_cpu_percentile(slow_latencies, slow_count, 50u),
      (unsigned long long)owner_cpu_percentile(slow_latencies, slow_count, 95u),
      (unsigned long long)owner_cpu_percentile(slow_latencies, slow_count, 99u),
      (unsigned long long)stats.accepted_connections,
      (unsigned long long)stats.rejected_connections);
  fflush(stdout);
  result = 0;

cleanup_threads:
  atomic_store_explicit(&barrier.start, 1, memory_order_release);
  for (index = 0u; index < OWNER_CPU_CONNECTIONS; ++index) {
    if (thread_started[index]) {
      (void)cmeta_thread_join(&threads[index]);
      cmeta_thread_destroy(&threads[index]);
      thread_started[index] = false;
    }
  }

cleanup:
  for (index = 0u; index < OWNER_CPU_CONNECTIONS; ++index) {
    if (sockets[index] != OWNER_CPU_INVALID_SOCKET) {
      owner_cpu_close(sockets[index]);
      sockets[index] = OWNER_CPU_INVALID_SOCKET;
    }
  }
  if (server.impl != NULL) {
    (void)chttp_server_stop(&server, OWNER_CPU_TIMEOUT_MS);
    (void)chttp_server_destroy(&server);
  }
  return result;
}

int main(void) {
  static const size_t owners[] = {1u, 2u, 4u};
  static const uint64_t work_ns[] = {
      0u, UINT64_C(50000), UINT64_C(250000),
      UINT64_C(1000000), UINT64_C(5000000)};
  const size_t requests =
      owner_cpu_env_count("CHTTP_OWNER_CPU_REQUESTS", OWNER_CPU_DEFAULT_REQUESTS);
  size_t work_index;
  size_t owner_index;
  int result = 0;

#if defined(_WIN32)
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
#endif
  atomic_init(&OWNER_CPU_SINK, 0u);

  printf(
      "{\"kind\":\"environment\","
      "\"benchmark\":\"chttp_server_owner_handler_cpu\","
      "\"commit\":\"%s\","
      "\"connections\":%u,"
      "\"requests_per_connection\":%zu,"
      "\"pattern\":\"sequential admission alternates slow/fast connections; owners=2/4 isolates route classes by owner\"}\n",
      getenv("GITHUB_SHA") != NULL ? getenv("GITHUB_SHA") : "unknown",
      OWNER_CPU_CONNECTIONS, requests);

  for (work_index = 0u; work_index < sizeof(work_ns) / sizeof(work_ns[0]); ++work_index) {
    for (owner_index = 0u; owner_index < sizeof(owners) / sizeof(owners[0]); ++owner_index) {
      if (owner_cpu_run(owners[owner_index], work_ns[work_index], requests) != 0) {
        fprintf(stderr,
                "owner handler CPU benchmark failed owners=%zu target_work_ns=%llu\n",
                owners[owner_index], (unsigned long long)work_ns[work_index]);
        result = 3;
        goto cleanup;
      }
    }
  }

cleanup:
#if defined(_WIN32)
  WSACleanup();
#endif
  return result;
}
