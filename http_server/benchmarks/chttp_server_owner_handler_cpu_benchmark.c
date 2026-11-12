#include "chttp_test_thread_cpu.h"
#include "../src/chttp_server_engine.h"
#include "chttp_server_runtime.h"

#include <http_server/http.h>
#include <core/clock.h>
#include <threads/threads.h>
#include <threads/thread_pool.h>

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
  OWNER_CPU_MAX_REQUESTS = 128,
  OWNER_CPU_DEFERRED_JOBS = 16,
  OWNER_CPU_DEFERRED_QUEUE_CAPACITY = 16
};

typedef enum owner_cpu_mode {
  OWNER_CPU_MODE_INLINE = 0,
  OWNER_CPU_MODE_DEFERRED = 1
} owner_cpu_mode;

typedef enum owner_cpu_job_stage {
  OWNER_CPU_JOB_ABORTED = 0,
  OWNER_CPU_JOB_WAITING = 1,
  OWNER_CPU_JOB_READY = 2
} owner_cpu_job_stage;

typedef struct owner_cpu_deferred_job {
  atomic_int in_use;
  atomic_int stage;
  size_t burn_iterations;
  chttp_server_deferred deferred;
  atomic_uint *errors;
} owner_cpu_deferred_job;

/* Captured only during warmup; tokens and OS handles stay valid until drain.
 * Main freezes capture before the start barrier, then reads cumulative clocks. */
typedef struct owner_cpu_clocks {
  sync_mutex_t mutex;
  atomic_int capturing;
  size_t count;
  chttp_test_thread_cpu threads[OWNER_CPU_MAX_OWNERS];
} owner_cpu_clocks;

typedef struct owner_cpu_prime {
  chttp_test_thread_cpu cpu;
  atomic_int ready;
  atomic_int *release;
  int status;
} owner_cpu_prime;

static int owner_cpu_capture(owner_cpu_clocks *clocks) {
  int status = SALTS_OK;
  if (!atomic_load_explicit(&clocks->capturing, memory_order_acquire)) return status;
  sync_mutex_lock(&clocks->mutex);
  size_t i = 0u;
  for (; i < clocks->count; ++i)
    if (clocks->threads[i].token == sync_thread_current_token()) break;
  if (i == clocks->count) {
    if (i == OWNER_CPU_MAX_OWNERS) status = SALTS_EMSGSIZE;
    else {
      status = chttp_test_thread_cpu_capture(&clocks->threads[i]);
      if (status == SALTS_OK) ++clocks->count;
    }
  }
  sync_mutex_unlock(&clocks->mutex);
  return status;
}

static int owner_cpu_read(const chttp_test_thread_cpu *clocks, size_t count, uint64_t *out) {
  *out = 0u;
  for (size_t i = 0u; i < count; ++i) {
    uint64_t ns;
    int status = chttp_test_thread_cpu_read(&clocks[i], &ns);
    if (status != SALTS_OK) return status;
    if (ns > UINT64_MAX - *out) return SALTS_ERANGE;
    *out += ns;
  }
  return SALTS_OK;
}

static void owner_cpu_prime_worker(void *arg) {
  owner_cpu_prime *prime = arg;
  prime->status = chttp_test_thread_cpu_capture(&prime->cpu);
  atomic_store_explicit(&prime->ready, 1, memory_order_release);
  while (!atomic_load_explicit(prime->release, memory_order_acquire)) sync_thread_yield();
}

typedef struct owner_cpu_route {
  size_t burn_iterations;
  owner_cpu_clocks *clocks;
  owner_cpu_mode mode;
  threadpool_t *pool;
  owner_cpu_deferred_job *jobs;
  size_t job_count;
  atomic_uint *deferred_errors;
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
  uint64_t cpu_ns;
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
    started = hrtime();
    (void)owner_cpu_burn(iterations);
    elapsed = hrtime() - started;
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

  started = hrtime();
  (void)owner_cpu_burn(iterations);
  elapsed = hrtime() - started;
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
    return 0u;
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

static void owner_cpu_deferred_job_release(owner_cpu_deferred_job *job) {
  if (job == NULL) return;
  job->deferred = (chttp_server_deferred){0};
  job->burn_iterations = 0u;
  job->errors = NULL;
  atomic_store_explicit(
      &job->stage, OWNER_CPU_JOB_ABORTED, memory_order_release);
  atomic_store_explicit(&job->in_use, 0, memory_order_release);
}

static void owner_cpu_deferred_job_run(void *user) {
  static const char body[] = "ok";
  owner_cpu_deferred_job *job = (owner_cpu_deferred_job *)user;
  chttp_server_deferred_response reply = {
      .size = sizeof(reply),
      .status_code = 200u,
      .content_type = "text/plain",
      .body = body,
      .body_size = sizeof(body) - 1u};
  int status;
  int stage;

  if (job == NULL) return;
  do {
    stage = atomic_load_explicit(&job->stage, memory_order_acquire);
    if (stage == OWNER_CPU_JOB_WAITING) sync_thread_yield();
  } while (stage == OWNER_CPU_JOB_WAITING);

  if (stage != OWNER_CPU_JOB_READY) {
    owner_cpu_deferred_job_release(job);
    return;
  }

  if (job->burn_iterations != 0u)
    (void)owner_cpu_burn(job->burn_iterations);
  status = chttp_server_deferred_reply(&job->deferred, &reply);
  if (status != SALTS_OK && job->errors != NULL) {
    atomic_fetch_add_explicit(job->errors, 1u, memory_order_relaxed);
    if (job->deferred.impl != NULL &&
        chttp_server_deferred_cancel(&job->deferred) != SALTS_OK)
      atomic_fetch_add_explicit(job->errors, 1u, memory_order_relaxed);
  }
  owner_cpu_deferred_job_release(job);
}

static owner_cpu_deferred_job *owner_cpu_deferred_job_claim(
    owner_cpu_route *route) {
  size_t index;
  if (route == NULL || route->jobs == NULL) return NULL;
  for (index = 0u; index < route->job_count; ++index) {
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &route->jobs[index].in_use, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
      route->jobs[index].deferred =
          (chttp_server_deferred)CHTTP_SERVER_DEFERRED_INIT;
      route->jobs[index].burn_iterations = 0u;
      route->jobs[index].errors = NULL;
      atomic_store_explicit(
          &route->jobs[index].stage,
          OWNER_CPU_JOB_WAITING,
          memory_order_release);
      return &route->jobs[index];
    }
  }
  return NULL;
}

static int owner_cpu_handler(void *user,
                             const chttp_server_request_view *request,
                             chttp_server_response *response) {
  static const char body[] = "ok";
  static const char unavailable[] = "Service Unavailable";
  owner_cpu_route *route = (owner_cpu_route *)user;
  owner_cpu_deferred_job *job;
  int status;
  (void)request;

  if (route == NULL) return SALTS_EINVAL;
  status = owner_cpu_capture(route->clocks);
  if (status != SALTS_OK) return status;
  if (route->mode == OWNER_CPU_MODE_INLINE) {
    if (route->burn_iterations != 0u)
      (void)owner_cpu_burn(route->burn_iterations);
    return chttp_server_reply(
        response, 200u, "text/plain", body, sizeof(body) - 1u);
  }

  if (route->pool == NULL || route->jobs == NULL ||
      route->job_count == 0u || route->deferred_errors == NULL)
    return SALTS_EINVAL;

  job = owner_cpu_deferred_job_claim(route);
  if (job == NULL) {
    atomic_fetch_add_explicit(
        route->deferred_errors, 1u, memory_order_relaxed);
    return chttp_server_reply(
        response, 503u, "text/plain",
        unavailable, sizeof(unavailable) - 1u);
  }

  job->burn_iterations = route->burn_iterations;
  job->errors = route->deferred_errors;

  /*
   * Canonical application-worker handoff:
   * reserve bounded worker capacity before transferring the HTTP request,
   * then publish READY only after response_defer succeeds. The accepted worker
   * may start immediately but cannot touch the deferred handle while WAITING.
   *
   * Use the same application-owned bounded threadpool surface as the repository
   * deferred Web example. CHttp owns no hidden worker pool.
   */
  status = threadpool_try_submit(
      route->pool, owner_cpu_deferred_job_run, job);
  if (status != SALTS_OK) {
    atomic_fetch_add_explicit(
        route->deferred_errors, 1u, memory_order_relaxed);
    owner_cpu_deferred_job_release(job);
    return chttp_server_reply(
        response, 503u, "text/plain",
        unavailable, sizeof(unavailable) - 1u);
  }

  status = chttp_server_response_defer(response, &job->deferred);
  if (status != SALTS_OK) {
    atomic_store_explicit(
        &job->stage, OWNER_CPU_JOB_ABORTED, memory_order_release);
    return status;
  }

  atomic_store_explicit(
      &job->stage, OWNER_CPU_JOB_READY, memory_order_release);
  return SALTS_OK;
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
  started = hrtime();
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
      *out_ns = hrtime() - started;
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
  const uint64_t deadline = monotonic_ms() + OWNER_CPU_TIMEOUT_MS;
  do {
    owner_cpu_snapshot(impl, owner_count, leases);
    if (owner_cpu_sum(leases, owner_count) == expected) return 0;
    sync_thread_yield();
  } while (monotonic_ms() < deadline);
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
  owner_cpu_client *client = user;
  chttp_test_thread_cpu cpu = {0};
  uint64_t before = 0u, after = 0u;
  int status = chttp_test_thread_cpu_capture(&cpu);
  atomic_fetch_add_explicit(&client->barrier->ready, 1, memory_order_acq_rel);
  while (!atomic_load_explicit(&client->barrier->start, memory_order_acquire)) sync_thread_yield();
  if (status == SALTS_OK) status = chttp_test_thread_cpu_read(&cpu, &before);
  for (size_t i = 0u; i < client->requests && status == SALTS_OK; ++i)
    if (owner_cpu_request(client->socket_value, client->target, &client->latencies[i]) != 0)
      status = SALTS_EIO;
  if (status == SALTS_OK) status = chttp_test_thread_cpu_read(&cpu, &after);
  if (status == SALTS_OK && after < before) status = SALTS_ERANGE;
  client->cpu_ns = status == SALTS_OK ? after - before : 0u;
  int released = chttp_test_thread_cpu_release(&cpu);
  if (status == SALTS_OK) status = released;
  atomic_store_explicit(&client->status, status, memory_order_release);
}

static int owner_cpu_run(size_t owner_count, uint64_t target_work_ns,
                         size_t requests_per_connection,
                         owner_cpu_mode mode, size_t burn_iterations,
                         uint64_t calibrated_work_ns) {
  chttp_server server = {0};
  chttp_server_config config = owner_cpu_server_config();
  chttp_server_execution_options execution =
      (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
  chttp_server_stats stats = {0};
  chttp_server_impl *impl = NULL;
  owner_cpu_route fast_route = {0};
  owner_cpu_route slow_route = {0};
  threadpool_t *pool = NULL;
  owner_cpu_deferred_job deferred_jobs[OWNER_CPU_DEFERRED_JOBS];
  atomic_uint deferred_errors;
  owner_cpu_socket sockets[OWNER_CPU_CONNECTIONS];
  size_t leases[OWNER_CPU_MAX_OWNERS] = {0};
  size_t before[OWNER_CPU_MAX_OWNERS] = {0};
  owner_cpu_client clients[OWNER_CPU_CONNECTIONS];
  sync_thread_t threads[OWNER_CPU_CONNECTIONS] = {0};
  bool thread_started[OWNER_CPU_CONNECTIONS] = {false};
  owner_cpu_barrier barrier;
  uint64_t latencies[OWNER_CPU_CONNECTIONS][OWNER_CPU_MAX_REQUESTS] = {{0}};
  uint64_t fast_latencies[(OWNER_CPU_CONNECTIONS / 2) * OWNER_CPU_MAX_REQUESTS];
  uint64_t slow_latencies[(OWNER_CPU_CONNECTIONS / 2) * OWNER_CPU_MAX_REQUESTS];
  uint16_t port = 0u;
  uint64_t started_ns;
  uint64_t wall_ns;
  owner_cpu_clocks owner_clocks = {0};
  owner_cpu_prime primes[OWNER_CPU_MAX_OWNERS] = {0};
  chttp_test_thread_cpu worker_clocks[OWNER_CPU_MAX_OWNERS] = {0};
  atomic_int prime_release;
  uint64_t owner_before = 0u, owner_after = 0u, worker_before = 0u, worker_after = 0u;
  uint64_t client_cpu = 0u;
  const size_t worker_count = mode == OWNER_CPU_MODE_DEFERRED ? owner_count : 0u;
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
  memset(deferred_jobs, 0, sizeof(deferred_jobs));
  atomic_init(&barrier.ready, 0);
  atomic_init(&barrier.start, 0);
  atomic_init(&deferred_errors, 0u);
  for (index = 0u; index < OWNER_CPU_DEFERRED_JOBS; ++index) {
    atomic_init(&deferred_jobs[index].in_use, 0);
    atomic_init(&deferred_jobs[index].stage, OWNER_CPU_JOB_ABORTED);
  }

  sync_mutex_init(&owner_clocks.mutex);
  atomic_init(&owner_clocks.capturing, 1);
  atomic_init(&prime_release, 0);
  fast_route.clocks = slow_route.clocks = &owner_clocks;
  for (index = 0u; index < OWNER_CPU_MAX_OWNERS; ++index) {
    atomic_init(&primes[index].ready, 0);
    primes[index].release = &prime_release;
  }
  slow_route.burn_iterations = burn_iterations;
  slow_route.mode = mode;
  if (mode == OWNER_CPU_MODE_DEFERRED) {
    const threadpool_config_t pool_config = {
        .num_threads = owner_count,
        .queue_capacity = OWNER_CPU_DEFERRED_QUEUE_CAPACITY};
    pool = threadpool_create_with_config(&pool_config);
    if (pool == NULL) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
    slow_route.pool = pool;
    slow_route.jobs = deferred_jobs;
    slow_route.job_count = OWNER_CPU_DEFERRED_JOBS;
    slow_route.deferred_errors = &deferred_errors;
    for (index = 0u; index < worker_count; ++index)
      if (threadpool_try_submit(pool, owner_cpu_prime_worker, &primes[index]) != SALTS_OK)
        { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
    const uint64_t prime_deadline = monotonic_ms() + OWNER_CPU_TIMEOUT_MS;
    for (index = 0u; index < worker_count; ++index) {
      while (!atomic_load_explicit(&primes[index].ready, memory_order_acquire)) {
        if (monotonic_ms() >= prime_deadline) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
        sync_thread_yield();
      }
      if (primes[index].status != SALTS_OK) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
      worker_clocks[index] = primes[index].cpu; /* one retained handle; release primes only */
    }
    atomic_store_explicit(&prime_release, 1, memory_order_release);
    if (threadpool_wait_status(pool) != SALTS_OK) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
  }

  if (chttp_server_init(&server, &config) != SALTS_OK) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
  execution.owner_count = owner_count;
  if (chttp_server_set_execution_options(&server, &execution) != SALTS_OK) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
  if (chttp_server_get(&server, "/fast", owner_cpu_handler, &fast_route) != SALTS_OK)
    { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
  if (chttp_server_get(&server, "/slow", owner_cpu_handler, &slow_route) != SALTS_OK)
    { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
  if (chttp_server_start(&server) != SALTS_OK) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
  if (chttp_server_port(&server, &port) != SALTS_OK || port == 0u) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
  impl = (chttp_server_impl *)server.impl;

  for (index = 0u; index < OWNER_CPU_CONNECTIONS; ++index) {
    uint64_t ignored;
    const bool slow = (index % 2u) == 0u;
    const char *target = slow ? "/slow" : "/fast";
    size_t assigned;
    size_t warmup;

    owner_cpu_snapshot(impl, owner_count, before);
    if (owner_cpu_connect(port, &sockets[index]) != 0) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
    for (warmup = 0u; warmup < OWNER_CPU_WARMUP; ++warmup)
      if (owner_cpu_request(sockets[index], target, &ignored) != 0) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
    if (owner_cpu_wait_total(impl, owner_count, index + 1u, leases) != 0) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
    if (owner_cpu_delta_owner(before, leases, owner_count, &assigned) != 0) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
    if (slow)
      slow_owner_mask |= 1u << assigned;
    else
      fast_owner_mask |= 1u << assigned;
  }

  if (owner_count > 1u && (slow_owner_mask & fast_owner_mask) != 0u) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }

  for (index = 0u; index < OWNER_CPU_CONNECTIONS; ++index) {
    clients[index] = (owner_cpu_client){
        .socket_value = sockets[index],
        .target = (index % 2u) == 0u ? "/slow" : "/fast",
        .requests = requests_per_connection,
        .latencies = latencies[index],
        .barrier = &barrier};
    atomic_init(&clients[index].status, SALTS_EIO);
    if (sync_thread_create(&threads[index], owner_cpu_client_main, &clients[index]) != SALTS_OK)
      { fprintf(stderr, "benchmark thread boundary failed at line %d\n", __LINE__); goto cleanup_threads; }
    thread_started[index] = true;
  }

  while (atomic_load_explicit(&barrier.ready, memory_order_acquire) !=
         OWNER_CPU_CONNECTIONS)
    sync_thread_yield();

  sync_mutex_lock(&owner_clocks.mutex);
  atomic_store_explicit(&owner_clocks.capturing, 0, memory_order_release);
  sync_mutex_unlock(&owner_clocks.mutex);
  if (owner_clocks.count != owner_count ||
      owner_cpu_read(owner_clocks.threads, owner_count, &owner_before) != SALTS_OK ||
      owner_cpu_read(worker_clocks, worker_count, &worker_before) != SALTS_OK)
    { fprintf(stderr, "benchmark thread boundary failed at line %d\n", __LINE__); goto cleanup_threads; }
  started_ns = hrtime();
  atomic_store_explicit(&barrier.start, 1, memory_order_release);

  for (index = 0u; index < OWNER_CPU_CONNECTIONS; ++index) {
    size_t sample;
    if (sync_thread_join(&threads[index]) != SALTS_OK) { fprintf(stderr, "benchmark thread boundary failed at line %d\n", __LINE__); goto cleanup_threads; }
    sync_thread_destroy(&threads[index]);
    thread_started[index] = false;
    if (atomic_load_explicit(&clients[index].status, memory_order_acquire) != SALTS_OK)
      { fprintf(stderr, "benchmark thread boundary failed at line %d\n", __LINE__); goto cleanup_threads; }
    if (clients[index].cpu_ns > UINT64_MAX - client_cpu) { fprintf(stderr, "benchmark thread boundary failed at line %d\n", __LINE__); goto cleanup_threads; }
    client_cpu += clients[index].cpu_ns;
    for (sample = 0u; sample < requests_per_connection; ++sample) {
      if ((index % 2u) == 0u)
        slow_latencies[slow_count++] = latencies[index][sample];
      else
        fast_latencies[fast_count++] = latencies[index][sample];
    }
  }

  wall_ns = hrtime() - started_ns;
  if (owner_cpu_read(owner_clocks.threads, owner_count, &owner_after) != SALTS_OK ||
      owner_cpu_read(worker_clocks, worker_count, &worker_after) != SALTS_OK ||
      owner_after < owner_before || worker_after < worker_before) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
  const uint64_t owner_cpu = owner_after - owner_before;
  const uint64_t worker_cpu = worker_after - worker_before;
  if (owner_cpu > UINT64_MAX - worker_cpu ||
      owner_cpu + worker_cpu > UINT64_MAX - client_cpu) { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
  if (atomic_load_explicit(&deferred_errors, memory_order_acquire) != 0u)
    { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }
  if (chttp_server_get_stats(&server, &stats) != SALTS_OK ||
      stats.rejected_connections != 0u)
    { fprintf(stderr, "benchmark boundary failed at line %d\n", __LINE__); goto cleanup; }

  printf(
      "{\"kind\":\"measurement\","
      "\"benchmark\":\"chttp_server_owner_handler_cpu\","
      "\"mode\":\"%s\","
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
      "\"cpu_time_kind\":\"thread-user-kernel\","
      "\"owner_cpu_ns\":%llu,"
      "\"worker_cpu_ns\":%llu,"
      "\"client_cpu_ns\":%llu,"
      "\"cpu_ns_per_op\":%.3f,"
      "\"fast_p50_ns\":%llu,"
      "\"fast_p95_ns\":%llu,"
      "\"fast_p99_ns\":%llu,"
      "\"slow_p50_ns\":%llu,"
      "\"slow_p95_ns\":%llu,"
      "\"slow_p99_ns\":%llu,"
      "\"accepted_connections\":%llu,"
      "\"rejected_connections\":%llu,"
      "\"deferred_errors\":%u,"
      "\"worker_count\":%zu,"
      "\"cross_owner_data_plane_hops\":0,"
      "\"errors\":0}\n",
      mode == OWNER_CPU_MODE_DEFERRED ? "deferred" : "inline",
      owner_count, OWNER_CPU_CONNECTIONS, requests_per_connection,
      (unsigned long long)target_work_ns,
      (unsigned long long)calibrated_work_ns,
      slow_route.burn_iterations, fast_owner_mask, slow_owner_mask,
      leases[0], leases[1], leases[2], leases[3],
      fast_count, slow_count,
      wall_ns != 0u
          ? (double)(fast_count + slow_count) * 1.0e9 / (double)wall_ns
          : 0.0,
      (unsigned long long)owner_cpu, (unsigned long long)worker_cpu,
      (unsigned long long)client_cpu,
      (fast_count + slow_count) != 0u
          ? (double)(owner_cpu + worker_cpu + client_cpu) /
                (double)(fast_count + slow_count)
          : 0.0,
      (unsigned long long)owner_cpu_percentile(fast_latencies, fast_count, 50u),
      (unsigned long long)owner_cpu_percentile(fast_latencies, fast_count, 95u),
      (unsigned long long)owner_cpu_percentile(fast_latencies, fast_count, 99u),
      (unsigned long long)owner_cpu_percentile(slow_latencies, slow_count, 50u),
      (unsigned long long)owner_cpu_percentile(slow_latencies, slow_count, 95u),
      (unsigned long long)owner_cpu_percentile(slow_latencies, slow_count, 99u),
      (unsigned long long)stats.accepted_connections,
      (unsigned long long)stats.rejected_connections,
      atomic_load_explicit(&deferred_errors, memory_order_acquire),
      mode == OWNER_CPU_MODE_DEFERRED ? owner_count : 0u);
  fflush(stdout);
  result = 0;

cleanup_threads:
  atomic_store_explicit(&barrier.start, 1, memory_order_release);
  for (index = 0u; index < OWNER_CPU_CONNECTIONS; ++index) {
    if (thread_started[index]) {
      (void)sync_thread_join(&threads[index]);
      sync_thread_destroy(&threads[index]);
      thread_started[index] = false;
    }
  }

cleanup:
  atomic_store_explicit(&prime_release, 1, memory_order_release);
  for (index = 0u; index < OWNER_CPU_CONNECTIONS; ++index) {
    if (sockets[index] != OWNER_CPU_INVALID_SOCKET) {
      owner_cpu_close(sockets[index]);
      sockets[index] = OWNER_CPU_INVALID_SOCKET;
    }
  }
  if (server.impl != NULL) {
    if (chttp_server_stop(&server, OWNER_CPU_TIMEOUT_MS) != SALTS_OK) result = 1;
  }
  if (pool != NULL) {
    const int shutdown_status = threadpool_shutdown_with_policy(
        pool, SALTS_THREADPOOL_SHUTDOWN_DRAIN);
    const int wait_status =
        shutdown_status == SALTS_OK
            ? threadpool_wait_status(pool)
            : shutdown_status;
    if (result == 0 && wait_status != SALTS_OK) result = 1;
    threadpool_destroy(pool);
    pool = NULL;
  }
  if (server.impl != NULL && chttp_server_destroy(&server) != SALTS_OK) result = 1;
  for (index = 0u; index < owner_clocks.count; ++index)
    if (chttp_test_thread_cpu_release(&owner_clocks.threads[index]) != SALTS_OK) result = 1;
  for (index = 0u; index < OWNER_CPU_MAX_OWNERS; ++index)
    if (chttp_test_thread_cpu_release(&primes[index].cpu) != SALTS_OK) result = 1;
  sync_mutex_destroy(&owner_clocks.mutex);
  return result;
}

int main(void) {
  static const size_t owners[] = {1u, 2u, 4u};
  static const uint64_t work_ns[] = {
      0u, UINT64_C(50000), UINT64_C(250000),
      UINT64_C(1000000), UINT64_C(5000000)};
  const size_t requests =
      owner_cpu_env_count("CHTTP_OWNER_CPU_REQUESTS", OWNER_CPU_DEFAULT_REQUESTS);
  static const owner_cpu_mode modes[] = {
      OWNER_CPU_MODE_INLINE, OWNER_CPU_MODE_DEFERRED};
  size_t calibrated_iterations[sizeof(work_ns) / sizeof(work_ns[0])];
  uint64_t calibrated_ns[sizeof(work_ns) / sizeof(work_ns[0])];
  size_t mode_index;
  size_t work_index;
  size_t owner_index;
  int result = 0;

#if defined(_WIN32)
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
#endif
  if (requests == 0u) return 2;
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

  /* One calibration per budget per repeat: both modes/owner counts execute
   * identical work, independent of scheduling and Turbo changes between cases. */
  for (work_index = 0u; work_index < sizeof(work_ns) / sizeof(work_ns[0]); ++work_index)
    calibrated_iterations[work_index] = owner_cpu_calibrate(work_ns[work_index], &calibrated_ns[work_index]);

  for (mode_index = 0u; mode_index < sizeof(modes) / sizeof(modes[0]); ++mode_index) {
    for (work_index = 0u; work_index < sizeof(work_ns) / sizeof(work_ns[0]); ++work_index) {
      for (owner_index = 0u; owner_index < sizeof(owners) / sizeof(owners[0]); ++owner_index) {
        if (owner_cpu_run(
                owners[owner_index], work_ns[work_index], requests,
                modes[mode_index], calibrated_iterations[work_index],
                calibrated_ns[work_index]) != 0) {
          fprintf(
              stderr,
              "owner handler CPU benchmark failed mode=%s owners=%zu target_work_ns=%llu\n",
              modes[mode_index] == OWNER_CPU_MODE_DEFERRED ? "deferred" : "inline",
              owners[owner_index], (unsigned long long)work_ns[work_index]);
          result = 3;
          goto cleanup;
        }
      }
    }
  }

cleanup:
#if defined(_WIN32)
  WSACleanup();
#endif
  return result;
}
