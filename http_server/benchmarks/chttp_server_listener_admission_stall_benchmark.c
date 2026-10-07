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

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET owner_listener_socket;
  #define OWNER_LISTENER_INVALID_SOCKET INVALID_SOCKET
  #define owner_listener_close closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int owner_listener_socket;
  #define OWNER_LISTENER_INVALID_SOCKET (-1)
  #define owner_listener_close close
#endif

enum {
  OWNER_LISTENER_DEFAULT_SAMPLES = 24,
  OWNER_LISTENER_MAX_SAMPLES = 64,
  OWNER_LISTENER_TIMEOUT_MS = 10000,
  OWNER_LISTENER_MAX_OWNERS = 4
};

typedef struct owner_listener_stall_route {
  size_t burn_iterations;
  atomic_uint entered;
  atomic_uint armed;
  atomic_int abort_requested;
} owner_listener_stall_route;

typedef struct owner_listener_stall_worker {
  owner_listener_socket socket_value;
  size_t samples;
  atomic_uint start_sequence;
  atomic_uint done_sequence;
  atomic_int abort_requested;
  atomic_int status;
} owner_listener_stall_worker;

static atomic_uint_fast64_t OWNER_LISTENER_CPU_SINK;

static uint64_t owner_listener_cpu_burn(size_t iterations) {
  uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
  size_t index;
  for (index = 0u; index < iterations; ++index) {
    state ^= state >> 12u;
    state ^= state << 25u;
    state ^= state >> 27u;
    state *= UINT64_C(2685821657736338717);
  }
  atomic_fetch_xor_explicit(&OWNER_LISTENER_CPU_SINK, state, memory_order_relaxed);
  return state;
}

static size_t owner_listener_calibrate(uint64_t target_ns,
                                       uint64_t *out_measured_ns) {
  size_t iterations = 256u;
  uint64_t elapsed = 0u;
  uint64_t started;
  if (out_measured_ns != NULL) *out_measured_ns = 0u;
  if (target_ns == 0u) return 0u;

  while (iterations <= SIZE_MAX / 2u) {
    started = cmeta_hrtime();
    (void)owner_listener_cpu_burn(iterations);
    elapsed = cmeta_hrtime() - started;
    if (elapsed >= UINT64_C(2000000)) break;
    iterations *= 2u;
  }
  if (elapsed == 0u) return 1u;

  {
    long double scaled =
        ((long double)iterations * (long double)target_ns) /
        (long double)elapsed;
    if (scaled < 1.0L) iterations = 1u;
    else if (scaled > (long double)(SIZE_MAX / 4u))
      iterations = SIZE_MAX / 4u;
    else
      iterations = (size_t)scaled;
  }

  started = cmeta_hrtime();
  (void)owner_listener_cpu_burn(iterations);
  elapsed = cmeta_hrtime() - started;
  if (out_measured_ns != NULL) *out_measured_ns = elapsed;
  return iterations;
}

static int owner_listener_u64_compare(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t owner_listener_percentile(const uint64_t *values, size_t count,
                                          unsigned int percent) {
  uint64_t ordered[OWNER_LISTENER_MAX_SAMPLES];
  size_t rank;
  if (values == NULL || count == 0u || count > OWNER_LISTENER_MAX_SAMPLES)
    return 0u;
  memcpy(ordered, values, count * sizeof(*ordered));
  qsort(ordered, count, sizeof(*ordered), owner_listener_u64_compare);
  rank = ((size_t)percent * count + 99u) / 100u;
  if (rank == 0u) rank = 1u;
  if (rank > count) rank = count;
  return ordered[rank - 1u];
}

static size_t owner_listener_env_samples(void) {
  const char *value = getenv("CHTTP_LISTENER_ADMISSION_SAMPLES");
  char *end = NULL;
  unsigned long parsed;
  if (value == NULL || value[0] == '\0') return OWNER_LISTENER_DEFAULT_SAMPLES;
  parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0ul ||
      parsed > OWNER_LISTENER_MAX_SAMPLES)
    return OWNER_LISTENER_DEFAULT_SAMPLES;
  return (size_t)parsed;
}

static native_io_backend_kind owner_listener_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_server_config owner_listener_server_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 64u,
      .network = {
          .backend = owner_listener_backend(),
          .connection_capacity = 16u,
          .command_capacity = 64u,
          .request_capacity = 32u,
          .completion_batch_capacity = 32u,
          .event_capacity = 64u,
          .max_send_bytes = 64u * 1024u,
          .receive_buffer_bytes = 16u * 1024u,
          .connect_timeout_ms = OWNER_LISTENER_TIMEOUT_MS,
          .read_timeout_ms = OWNER_LISTENER_TIMEOUT_MS,
          .write_timeout_ms = OWNER_LISTENER_TIMEOUT_MS},
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

static int owner_listener_probe_handler(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  static const char body[] = "ok";
  (void)user;
  (void)request;
  return chttp_server_reply(
      response, 200u, "text/plain", body, sizeof(body) - 1u);
}

static int owner_listener_stall_handler(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  static const char body[] = "ok";
  owner_listener_stall_route *route =
      (owner_listener_stall_route *)user;
  unsigned int sequence;
  (void)request;
  if (route == NULL) return SALTS_EINVAL;

  sequence =
      atomic_fetch_add_explicit(&route->entered, 1u, memory_order_acq_rel) + 1u;
  while (atomic_load_explicit(&route->armed, memory_order_acquire) < sequence) {
    if (atomic_load_explicit(
            &route->abort_requested, memory_order_acquire) != 0)
      return SALTS_ECANCELED;
    cmeta_thread_yield();
  }

  if (route->burn_iterations != 0u)
    (void)owner_listener_cpu_burn(route->burn_iterations);
  return chttp_server_reply(
      response, 200u, "text/plain", body, sizeof(body) - 1u);
}

static int owner_listener_socket_timeout(owner_listener_socket socket_value) {
#if defined(_WIN32)
  DWORD timeout = OWNER_LISTENER_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                    (const char *)&timeout, sizeof(timeout)) == 0 &&
                 setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                            (const char *)&timeout, sizeof(timeout)) == 0
             ? 0
             : -1;
#else
  struct timeval timeout = {
      .tv_sec = OWNER_LISTENER_TIMEOUT_MS / 1000,
      .tv_usec = (OWNER_LISTENER_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                    &timeout, sizeof(timeout)) == 0 &&
                 setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                            &timeout, sizeof(timeout)) == 0
             ? 0
             : -1;
#endif
}

static void owner_listener_shutdown_socket(owner_listener_socket socket_value) {
  if (socket_value == OWNER_LISTENER_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)shutdown(socket_value, SD_BOTH);
#else
  (void)shutdown(socket_value, SHUT_RDWR);
#endif
}

static int owner_listener_connect(uint16_t port,
                                  owner_listener_socket *out_socket,
                                  uint64_t *out_connect_ns) {
  struct sockaddr_in address;
  owner_listener_socket socket_value;
  uint64_t started;
  int one = 1;
  if (out_socket == NULL) return -1;
  *out_socket = OWNER_LISTENER_INVALID_SOCKET;
  if (out_connect_ns != NULL) *out_connect_ns = 0u;

  socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_value == OWNER_LISTENER_INVALID_SOCKET) return -1;
  if (owner_listener_socket_timeout(socket_value) != 0) goto fail;
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
  started = cmeta_hrtime();
  if (connect(socket_value, (const struct sockaddr *)&address,
              (int)sizeof(address)) != 0)
    goto fail;
  if (out_connect_ns != NULL) *out_connect_ns = cmeta_hrtime() - started;
  *out_socket = socket_value;
  return 0;

fail:
  owner_listener_close(socket_value);
  return -1;
}

static int owner_listener_send_all(owner_listener_socket socket_value,
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

static int owner_listener_send_target(owner_listener_socket socket_value,
                                      const char *target) {
  unsigned char request[256];
  int request_size;
  if (target == NULL) return -1;
  request_size = snprintf(
      (char *)request, sizeof(request),
      "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n",
      target);
  if (request_size <= 0 || (size_t)request_size >= sizeof(request)) return -1;
  return owner_listener_send_all(
      socket_value, request, (size_t)request_size);
}

static int owner_listener_receive_ok(owner_listener_socket socket_value) {
  unsigned char response[4096];
  size_t used = 0u;
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
    if (strstr((const char *)response, "\r\n\r\nok") != NULL) return 0;
  }
  return -1;
}

static int owner_listener_request(owner_listener_socket socket_value,
                                  const char *target) {
  if (owner_listener_send_target(socket_value, target) != 0) return -1;
  return owner_listener_receive_ok(socket_value);
}

static void owner_listener_snapshot(
    chttp_server_impl *impl, size_t owner_count,
    size_t leases[OWNER_LISTENER_MAX_OWNERS],
    size_t rings[OWNER_LISTENER_MAX_OWNERS]) {
  size_t index;
  memset(leases, 0, OWNER_LISTENER_MAX_OWNERS * sizeof(*leases));
  memset(rings, 0, OWNER_LISTENER_MAX_OWNERS * sizeof(*rings));
  for (index = 0u; index < owner_count &&
                   index < OWNER_LISTENER_MAX_OWNERS; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(impl, index);
    if (owner == NULL) continue;
    leases[index] = chttp_server_owner_lease_count(owner);
    rings[index] = chttp_server_owner_admission_count(owner);
  }
}

static size_t owner_listener_sum(
    const size_t values[OWNER_LISTENER_MAX_OWNERS], size_t owner_count) {
  size_t index;
  size_t total = 0u;
  for (index = 0u; index < owner_count; ++index) total += values[index];
  return total;
}

static int owner_listener_wait_total(
    chttp_server_impl *impl, size_t owner_count, size_t expected,
    size_t leases[OWNER_LISTENER_MAX_OWNERS],
    size_t rings[OWNER_LISTENER_MAX_OWNERS],
    size_t peak_rings[OWNER_LISTENER_MAX_OWNERS]) {
  const uint64_t deadline = cmeta_monotonic_ms() + OWNER_LISTENER_TIMEOUT_MS;
  do {
    size_t index;
    owner_listener_snapshot(impl, owner_count, leases, rings);
    if (peak_rings != NULL) {
      for (index = 0u; index < owner_count; ++index)
        if (rings[index] > peak_rings[index])
          peak_rings[index] = rings[index];
    }
    if (owner_listener_sum(leases, owner_count) == expected) return 0;
    cmeta_thread_yield();
  } while (cmeta_monotonic_ms() < deadline);
  return -1;
}

static int owner_listener_delta_owner(
    const size_t before[OWNER_LISTENER_MAX_OWNERS],
    const size_t after[OWNER_LISTENER_MAX_OWNERS], size_t owner_count,
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

static int owner_listener_wait_atomic_at_least(
    const atomic_uint *value, unsigned int expected) {
  const uint64_t deadline = cmeta_monotonic_ms() + OWNER_LISTENER_TIMEOUT_MS;
  do {
    if (atomic_load_explicit(value, memory_order_acquire) >= expected)
      return 0;
    cmeta_thread_yield();
  } while (cmeta_monotonic_ms() < deadline);
  return -1;
}

static void owner_listener_stall_worker_main(void *user) {
  owner_listener_stall_worker *worker =
      (owner_listener_stall_worker *)user;
  unsigned int sequence;
  if (worker == NULL) return;
  atomic_store_explicit(&worker->status, SALTS_OK, memory_order_release);

  for (sequence = 1u; sequence <= worker->samples; ++sequence) {
    while (atomic_load_explicit(
               &worker->start_sequence, memory_order_acquire) < sequence) {
      if (atomic_load_explicit(
              &worker->abort_requested, memory_order_acquire) != 0)
        return;
      cmeta_thread_yield();
    }
    if (owner_listener_request(worker->socket_value, "/stall") != 0) {
      atomic_store_explicit(
          &worker->status, SALTS_EIO, memory_order_release);
      return;
    }
    atomic_store_explicit(
        &worker->done_sequence, sequence, memory_order_release);
  }
}

static int owner_listener_run(size_t owner_count, uint64_t target_work_ns,
                              size_t sample_count) {
  chttp_server server = {0};
  chttp_server_config config = owner_listener_server_config();
  chttp_server_execution_options execution =
      (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
  chttp_server_stats stats = {0};
  owner_listener_stall_route stall_route;
  owner_listener_stall_worker stall_worker;
  cmeta_thread_t stall_thread = {0};
  bool stall_thread_started = false;
  chttp_server_impl *impl = NULL;
  owner_listener_socket stall_socket = OWNER_LISTENER_INVALID_SOCKET;
  uint64_t connect_latencies[OWNER_LISTENER_MAX_SAMPLES] = {0};
  uint64_t admission_latencies[OWNER_LISTENER_MAX_SAMPLES] = {0};
  uint64_t response_latencies[OWNER_LISTENER_MAX_SAMPLES] = {0};
  size_t admissions[OWNER_LISTENER_MAX_OWNERS] = {0};
  size_t peak_rings[OWNER_LISTENER_MAX_OWNERS] = {0};
  size_t before[OWNER_LISTENER_MAX_OWNERS] = {0};
  size_t before_rings[OWNER_LISTENER_MAX_OWNERS] = {0};
  size_t after[OWNER_LISTENER_MAX_OWNERS] = {0};
  size_t after_rings[OWNER_LISTENER_MAX_OWNERS] = {0};
  uint64_t calibrated_work_ns = 0u;
  uint16_t port = 0u;
  size_t index;
  int result = 1;

  if (owner_count == 0u || owner_count > OWNER_LISTENER_MAX_OWNERS ||
      sample_count == 0u || sample_count > OWNER_LISTENER_MAX_SAMPLES)
    return 1;

  memset(&stall_route, 0, sizeof(stall_route));
  atomic_init(&stall_route.entered, 0u);
  atomic_init(&stall_route.armed, 0u);
  atomic_init(&stall_route.abort_requested, 0);
  stall_route.burn_iterations =
      owner_listener_calibrate(target_work_ns, &calibrated_work_ns);

  memset(&stall_worker, 0, sizeof(stall_worker));
  stall_worker.samples = sample_count;
  atomic_init(&stall_worker.start_sequence, 0u);
  atomic_init(&stall_worker.done_sequence, 0u);
  atomic_init(&stall_worker.abort_requested, 0);
  atomic_init(&stall_worker.status, SALTS_EIO);

  if (chttp_server_init(&server, &config) != SALTS_OK) goto cleanup;
  execution.owner_count = owner_count;
  if (chttp_server_set_execution_options(&server, &execution) != SALTS_OK)
    goto cleanup;
  if (chttp_server_get(
          &server, "/probe", owner_listener_probe_handler, NULL) != SALTS_OK)
    goto cleanup;
  if (chttp_server_get(
          &server, "/stall", owner_listener_stall_handler,
          &stall_route) != SALTS_OK)
    goto cleanup;
  if (chttp_server_start(&server) != SALTS_OK) goto cleanup;
  if (chttp_server_port(&server, &port) != SALTS_OK || port == 0u) goto cleanup;
  impl = (chttp_server_impl *)server.impl;

  owner_listener_snapshot(impl, owner_count, before, before_rings);
  if (owner_listener_connect(port, &stall_socket, NULL) != 0 ||
      owner_listener_request(stall_socket, "/probe") != 0)
    goto cleanup;
  if (owner_listener_wait_total(
          impl, owner_count, 1u, after, after_rings, peak_rings) != 0)
    goto cleanup;
  {
    size_t assigned = SIZE_MAX;
    if (owner_listener_delta_owner(before, after, owner_count, &assigned) != 0 ||
        assigned != 0u) {
      fprintf(stderr,
              "primary stall connection did not bind owner0 owners=%zu assigned=%zu\n",
              owner_count, assigned);
      goto cleanup;
    }
  }

  stall_worker.socket_value = stall_socket;
  if (cmeta_thread_create(
          &stall_thread, owner_listener_stall_worker_main,
          &stall_worker) != SALTS_OK)
    goto cleanup;
  stall_thread_started = true;

  for (index = 0u; index < sample_count; ++index) {
    owner_listener_socket probe_socket = OWNER_LISTENER_INVALID_SOCKET;
    const unsigned int sequence = (unsigned int)(index + 1u);
    size_t assigned = SIZE_MAX;
    uint64_t admission_started;
    uint64_t response_started;

    atomic_store_explicit(
        &stall_worker.start_sequence, sequence, memory_order_release);
    if (owner_listener_wait_atomic_at_least(
            &stall_route.entered, sequence) != 0)
      goto cleanup;

    owner_listener_snapshot(impl, owner_count, before, before_rings);
    if (owner_listener_sum(before, owner_count) != 1u) {
      fprintf(stderr,
              "unexpected leases before isolated admission owners=%zu work_ns=%llu sample=%zu leases=%zu\n",
              owner_count, (unsigned long long)target_work_ns, index,
              owner_listener_sum(before, owner_count));
      goto cleanup;
    }

    admission_started = cmeta_hrtime();
    if (owner_listener_connect(
            port, &probe_socket, &connect_latencies[index]) != 0)
      goto cleanup;
    if (owner_listener_send_target(probe_socket, "/probe") != 0) {
      owner_listener_close(probe_socket);
      goto cleanup;
    }

    /*
     * Post-fix contract: listener admission is a separate control plane.
     * The probe lease must be acquired while owner0 is still blocked at the
     * handler gate. Only after this witness succeeds may the benchmark release
     * owner0 and run the calibrated application CPU work.
     */
    if (owner_listener_wait_total(
            impl, owner_count, 2u, after, after_rings, peak_rings) != 0) {
      fprintf(stderr,
              "listener admission remained blocked by owner0 owners=%zu work_ns=%llu sample=%zu\n",
              owner_count, (unsigned long long)target_work_ns, index);
      owner_listener_close(probe_socket);
      goto cleanup;
    }
    admission_latencies[index] = cmeta_hrtime() - admission_started;

    if (owner_listener_delta_owner(
            before, after, owner_count, &assigned) != 0) {
      owner_listener_close(probe_socket);
      goto cleanup;
    }
    ++admissions[assigned];

    response_started = cmeta_hrtime();
    atomic_store_explicit(
        &stall_route.armed, sequence, memory_order_release);

    if (owner_listener_receive_ok(probe_socket) != 0) {
      owner_listener_close(probe_socket);
      goto cleanup;
    }
    response_latencies[index] = cmeta_hrtime() - response_started;

    owner_listener_close(probe_socket);
    if (owner_listener_wait_total(
            impl, owner_count, 1u, after, after_rings, peak_rings) != 0)
      goto cleanup;
    if (owner_listener_wait_atomic_at_least(
            &stall_worker.done_sequence, sequence) != 0)
      goto cleanup;
    if (atomic_load_explicit(
            &stall_worker.status, memory_order_acquire) != SALTS_OK)
      goto cleanup;
  }

  if (cmeta_thread_join(&stall_thread) != SALTS_OK) goto cleanup;
  cmeta_thread_destroy(&stall_thread);
  stall_thread_started = false;

  if (chttp_server_get_stats(&server, &stats) != SALTS_OK ||
      stats.rejected_connections != 0u ||
      stats.accepted_connections != sample_count + 1u)
    goto cleanup;

  printf(
      "{\"kind\":\"measurement\","
      "\"benchmark\":\"chttp_server_listener_admission_stall\","
      "\"owners\":%zu,"
      "\"samples\":%zu,"
      "\"target_work_ns\":%llu,"
      "\"calibrated_work_ns\":%llu,"
      "\"burn_iterations\":%zu,"
      "\"stall_owner\":0,"
      "\"admitted_while_stalled\":true,"
      "\"accepted_connections\":%llu,"
      "\"rejected_connections\":%llu,"
      "\"owner0_admissions\":%zu,"
      "\"owner1_admissions\":%zu,"
      "\"owner2_admissions\":%zu,"
      "\"owner3_admissions\":%zu,"
      "\"peak_owner0_ring\":%zu,"
      "\"peak_owner1_ring\":%zu,"
      "\"peak_owner2_ring\":%zu,"
      "\"peak_owner3_ring\":%zu,"
      "\"connect_p50_ns\":%llu,"
      "\"connect_p95_ns\":%llu,"
      "\"connect_p99_ns\":%llu,"
      "\"admission_p50_ns\":%llu,"
      "\"admission_p95_ns\":%llu,"
      "\"admission_p99_ns\":%llu,"
      "\"response_p50_ns\":%llu,"
      "\"response_p95_ns\":%llu,"
      "\"response_p99_ns\":%llu,"
      "\"cross_owner_data_plane_hops\":0,"
      "\"errors\":0}\n",
      owner_count, sample_count,
      (unsigned long long)target_work_ns,
      (unsigned long long)calibrated_work_ns,
      stall_route.burn_iterations,
      (unsigned long long)stats.accepted_connections,
      (unsigned long long)stats.rejected_connections,
      admissions[0], admissions[1], admissions[2], admissions[3],
      peak_rings[0], peak_rings[1], peak_rings[2], peak_rings[3],
      (unsigned long long)owner_listener_percentile(
          connect_latencies, sample_count, 50u),
      (unsigned long long)owner_listener_percentile(
          connect_latencies, sample_count, 95u),
      (unsigned long long)owner_listener_percentile(
          connect_latencies, sample_count, 99u),
      (unsigned long long)owner_listener_percentile(
          admission_latencies, sample_count, 50u),
      (unsigned long long)owner_listener_percentile(
          admission_latencies, sample_count, 95u),
      (unsigned long long)owner_listener_percentile(
          admission_latencies, sample_count, 99u),
      (unsigned long long)owner_listener_percentile(
          response_latencies, sample_count, 50u),
      (unsigned long long)owner_listener_percentile(
          response_latencies, sample_count, 95u),
      (unsigned long long)owner_listener_percentile(
          response_latencies, sample_count, 99u));
  fflush(stdout);
  result = 0;

cleanup:
  atomic_store_explicit(
      &stall_route.abort_requested, 1, memory_order_release);
  atomic_store_explicit(
      &stall_worker.abort_requested, 1, memory_order_release);
  atomic_store_explicit(
      &stall_route.armed, UINT_MAX, memory_order_release);
  atomic_store_explicit(
      &stall_worker.start_sequence, UINT_MAX, memory_order_release);

  if (result != 0 && stall_socket != OWNER_LISTENER_INVALID_SOCKET)
    owner_listener_shutdown_socket(stall_socket);

  if (stall_thread_started) {
    (void)cmeta_thread_join(&stall_thread);
    cmeta_thread_destroy(&stall_thread);
    stall_thread_started = false;
  }

  if (stall_socket != OWNER_LISTENER_INVALID_SOCKET) {
    owner_listener_close(stall_socket);
    stall_socket = OWNER_LISTENER_INVALID_SOCKET;
  }
  if (server.impl != NULL) {
    (void)chttp_server_stop(&server, OWNER_LISTENER_TIMEOUT_MS);
    (void)chttp_server_destroy(&server);
  }
  return result;
}

int main(void) {
  static const size_t owners[] = {1u, 2u, 4u};
  static const uint64_t work_ns[] = {
      0u, UINT64_C(50000), UINT64_C(250000),
      UINT64_C(1000000), UINT64_C(5000000)};
  const size_t samples = owner_listener_env_samples();
  size_t work_index;
  size_t owner_index;
  int result = 0;

#if defined(_WIN32)
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
#endif
  atomic_init(&OWNER_LISTENER_CPU_SINK, 0u);

  printf(
      "{\"kind\":\"environment\","
      "\"benchmark\":\"chttp_server_listener_admission_stall\","
      "\"commit\":\"%s\","
      "\"samples\":%zu,"
      "\"note\":\"owner0 handler remains gated until probe lease growth proves isolated listener admission; calibrated CPU work begins only after admission\"}\n",
      getenv("GITHUB_SHA") != NULL ? getenv("GITHUB_SHA") : "unknown",
      samples);

  for (work_index = 0u;
       work_index < sizeof(work_ns) / sizeof(work_ns[0]); ++work_index) {
    for (owner_index = 0u;
         owner_index < sizeof(owners) / sizeof(owners[0]); ++owner_index) {
      if (owner_listener_run(
              owners[owner_index], work_ns[work_index], samples) != 0) {
        fprintf(
            stderr,
            "listener admission stall benchmark failed owners=%zu target_work_ns=%llu\n",
            owners[owner_index],
            (unsigned long long)work_ns[work_index]);
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
