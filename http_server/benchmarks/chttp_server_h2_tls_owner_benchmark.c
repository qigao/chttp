#include "chttp_server_runtime.h"
#include "chttp_client_internal.h"
#include "chttp_tls_test_material.h"
#define TINYTEST_NO_MAIN 1
#include "tinytest.h"

#include <http_client/http.h>
#include <http_server/http.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts_buffer.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  OWNER_PROTO_CONNECTIONS = 8,
  OWNER_PROTO_H2_DEPTH = 4,
  OWNER_PROTO_TIMEOUT_MS = 10000,
  OWNER_PROTO_SMALL_BYTES = 1024,
  OWNER_PROTO_BELOW_WINDOW_BYTES = 16383,
  OWNER_PROTO_CROSS_WINDOW_BYTES = 16384,
  OWNER_PROTO_32K_BYTES = 32 * 1024,
  OWNER_PROTO_DEFAULT_WINDOW_BYTES = 65535,
  OWNER_PROTO_RETAINED_BYTES = 64 * 1024,
  OWNER_PROTO_128K_BYTES = 128 * 1024,
  OWNER_PROTO_FLOW_CONNECTION_WINDOW = 1024 * 1024,
  OWNER_PROTO_SMALL_ROUNDS = 80,
  OWNER_PROTO_RETAINED_ROUNDS = 16,
  OWNER_PROTO_WARMUP_ROUNDS = 2
};

typedef struct owner_proto_case {
  const char *name;
  chttp_protocol protocol;
  bool tls;
  bool retained;
  bool server_nodelay;
  bool client_nodelay;
  size_t payload_bytes;
  size_t streams_per_connection;
  uint32_t stream_receive_window;
  uint32_t connection_receive_window;
  const char *benchmark_name;
} owner_proto_case;

typedef struct owner_proto_route {
  const unsigned char *small_body;
  size_t small_size;
  mem_buffer_t *retained_body;
  bool retained;
} owner_proto_route;

typedef struct owner_proto_barrier {
  atomic_int ready;
  atomic_int start;
} owner_proto_barrier;

typedef struct owner_proto_completion {
  uint64_t started_ns;
  uint64_t *latency_out;
  size_t expected_body_bytes;
  unsigned int expected_http_major;
  unsigned char expected_byte;
  const char *error_stage;
  int native_status;
  int done;
  int status;
} owner_proto_completion;

typedef struct owner_proto_worker {
  const owner_proto_case *test_case;
  uint16_t port;
  const char *ca_path;
  size_t rounds;
  size_t warmup_rounds;
  uint64_t *latencies;
  owner_proto_barrier *barrier;
  atomic_int status;
} owner_proto_worker;

typedef struct owner_proto_pressure {
  size_t owner_leases[4];
  size_t peak_owner_leases[4];
  size_t peak_owner_rings[4];
  size_t cross_owner_handoffs;
} owner_proto_pressure;

static unsigned char OWNER_PROTO_SMALL_BODY[OWNER_PROTO_SMALL_BYTES];
static unsigned char OWNER_PROTO_LARGE_COPY_BODY[OWNER_PROTO_128K_BYTES];
static unsigned char OWNER_PROTO_RETAINED_BODY[OWNER_PROTO_128K_BYTES];

static int owner_proto_u64_compare(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t owner_proto_percentile(const uint64_t *values, size_t count,
                                       unsigned int percent) {
  uint64_t *ordered;
  size_t rank;
  uint64_t result;
  if (values == NULL || count == 0u) return 0u;
  ordered = (uint64_t *)malloc(count * sizeof(*ordered));
  if (ordered == NULL) return 0u;
  memcpy(ordered, values, count * sizeof(*ordered));
  qsort(ordered, count, sizeof(*ordered), owner_proto_u64_compare);
  rank = ((size_t)percent * count + 99u) / 100u;
  if (rank == 0u) rank = 1u;
  if (rank > count) rank = count;
  result = ordered[rank - 1u];
  free(ordered);
  return result;
}

static size_t owner_proto_env_count(const char *name, size_t fallback) {
  const char *value = getenv(name);
  char *end = NULL;
  unsigned long parsed;
  if (value == NULL || value[0] == '\0') return fallback;
  parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0ul) return fallback;
  return (size_t)parsed;
}

static native_io_backend_kind owner_proto_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config owner_proto_server_network(bool tls) {
  cnet_client_config config = {
      .backend = owner_proto_backend(),
      .connection_capacity = 32u,
      .command_capacity = 128u,
      .request_capacity = 64u,
      .completion_batch_capacity = 64u,
      .event_capacity = 128u,
      .max_send_bytes = 256u * 1024u,
      .receive_buffer_bytes = 64u * 1024u,
      .connect_timeout_ms = OWNER_PROTO_TIMEOUT_MS,
      .read_timeout_ms = OWNER_PROTO_TIMEOUT_MS,
      .write_timeout_ms = OWNER_PROTO_TIMEOUT_MS};
  if (tls) {
    config.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    config.tls_handshake_timeout_ms = OWNER_PROTO_TIMEOUT_MS;
  }
  return config;
}

static chttp_server_config owner_proto_server_config(
    const owner_proto_case *test_case) {
  chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 64u,
      .network = owner_proto_server_network(test_case->tls),
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
      .max_buffered_response_body_bytes = 128u * 1024u,
      .buffer_capacity_bytes = 32u * 1024u * 1024u};
  if (test_case->protocol == CHTTP_HTTP_2) {
    config.enable_http2 = 1;
    config.h2_stream_capacity = 16u;
    config.h2_input_buffer_bytes = 256u * 1024u;
    config.h2_output_buffer_bytes = 256u * 1024u;
    config.h2_hpack_dynamic_table_bytes = 4096u;
    config.h2_max_settings_count = 16u;
  }
  return config;
}

static chttp_client_config owner_proto_client_config(
    const owner_proto_case *test_case) {
  chttp_client_config config = {
      .network = {
          .backend = owner_proto_backend(),
          .connection_capacity = 1u,
          .command_capacity = 32u,
          .request_capacity = 16u,
          .completion_batch_capacity = 16u,
          .event_capacity = 64u,
          .max_send_bytes = 256u * 1024u,
          .receive_buffer_bytes = 128u * 1024u,
          .connect_timeout_ms = OWNER_PROTO_TIMEOUT_MS,
          .read_timeout_ms = OWNER_PROTO_TIMEOUT_MS,
          .write_timeout_ms = OWNER_PROTO_TIMEOUT_MS},
      .request_capacity = 8u,
      .max_start_line_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 4096u,
      .max_request_body_bytes = 4096u,
      .max_response_body_bytes = 128u * 1024u,
      .max_informational_responses = 2u,
      .h2_input_buffer_bytes = 256u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
  if (test_case->tls) {
    config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    config.network.tls_handshake_timeout_ms = OWNER_PROTO_TIMEOUT_MS;
  }
  return config;
}

static int owner_proto_handler(void *user,
                               const chttp_server_request_view *request,
                               chttp_server_response *response) {
  owner_proto_route *route = (owner_proto_route *)user;
  (void)request;
  if (route == NULL) return SALTS_EINVAL;
  if (route->retained)
    return chttp_server_reply_buffer(response, 200u, "application/octet-stream",
                                     route->retained_body);
  return chttp_server_reply(response, 200u, "application/octet-stream",
                            route->small_body, route->small_size);
}

static void owner_proto_complete(void *user, chttp_request request,
                                 const chttp_response_view *response,
                                 const chttp_error *error) {
  owner_proto_completion *completion = (owner_proto_completion *)user;
  (void)request;
  if (completion == NULL || completion->done) return;
  completion->status = SALTS_OK;
  completion->error_stage = NULL;
  completion->native_status = 0;
  if (error != NULL) {
    completion->status = error->status;
    completion->error_stage = error->stage;
    completion->native_status = error->native_status;
  } else if (response == NULL || response->status_code != 200u ||
             response->http_major != completion->expected_http_major ||
             response->body_size != completion->expected_body_bytes ||
             (completion->expected_body_bytes != 0u &&
              (response->body == NULL ||
               ((const unsigned char *)response->body)[0] !=
                   completion->expected_byte ||
               ((const unsigned char *)response->body)
                       [completion->expected_body_bytes - 1u] !=
                   completion->expected_byte))) {
    completion->status = SALTS_EPROTO;
    completion->error_stage = "response-validate";
  }
  if (completion->latency_out != NULL)
    *completion->latency_out = salts_hrtime() - completion->started_ns;
  completion->done = 1;
}

static int owner_proto_round(chttp_async_client *client,
                             const chttp_request_options *base_options,
                             size_t depth,
                             size_t expected_body_bytes,
                             unsigned char expected_byte,
                             uint64_t *latencies,
                             size_t *latency_index) {
  owner_proto_completion completions[OWNER_PROTO_H2_DEPTH];
  chttp_request requests[OWNER_PROTO_H2_DEPTH];
  size_t complete_count = 0u;
  size_t index;
  uint64_t deadline;

  if (client == NULL || base_options == NULL || depth == 0u ||
      depth > OWNER_PROTO_H2_DEPTH)
    return SALTS_EINVAL;
  memset(completions, 0, sizeof(completions));
  memset(requests, 0, sizeof(requests));

  for (index = 0u; index < depth; ++index) {
    chttp_request_options options = *base_options;
    owner_proto_completion *completion = &completions[index];
    completion->expected_body_bytes = expected_body_bytes;
    completion->expected_http_major =
        base_options->protocol == CHTTP_HTTP_2 ? 2u : 1u;
    completion->expected_byte = expected_byte;
    completion->status = SALTS_EBUSY;
    completion->latency_out =
        latencies != NULL ? &latencies[(*latency_index)++] : NULL;
    completion->started_ns = salts_hrtime();
    options.user = completion;
    {
      const int status =
          chttp_async_client_submit(client, &options, &requests[index]);
      if (status != SALTS_OK) return status;
    }
  }

  deadline = salts_monotonic_ms() + OWNER_PROTO_TIMEOUT_MS;
  while (complete_count < depth) {
    size_t callbacks = 0u;
    int status = chttp_async_client_poll(client, 5u, &callbacks);
    if (status != SALTS_OK) return status;
    complete_count = 0u;
    for (index = 0u; index < depth; ++index)
      if (completions[index].done) ++complete_count;
    if (salts_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
  }
  for (index = 0u; index < depth; ++index) {
    if (completions[index].status != SALTS_OK) {
      fprintf(stderr,
              "owner protocol stream failed index=%zu status=%d native=%d stage=%s expected_bytes=%zu http_major=%u\n",
              index, completions[index].status,
              completions[index].native_status,
              completions[index].error_stage != NULL
                  ? completions[index].error_stage
                  : "none",
              completions[index].expected_body_bytes,
              completions[index].expected_http_major);
      return completions[index].status;
    }
  }
  return SALTS_OK;
}

static void owner_proto_worker_main(void *user) {
  static const char *H1_ALPN[] = {"http/1.1"};
  static const char *H2_ALPN[] = {"h2"};
  owner_proto_worker *worker = (owner_proto_worker *)user;
  chttp_async_client client = {0};
  chttp_tls_profile profile = {0};
  chttp_client_config config;
  cnet_tls_client_config tls_config = {0};
  chttp_request_options options;
  char uri[64];
  size_t latency_index = 0u;
  size_t round;
  bool announced = false;
  int status = SALTS_OK;

  atomic_store_explicit(&worker->status, SALTS_EIO, memory_order_release);
  config = owner_proto_client_config(worker->test_case);

  if (worker->test_case->tls) {
    const char *const *alpn =
        worker->test_case->protocol == CHTTP_HTTP_2 ? H2_ALPN : H1_ALPN;
    tls_config = (cnet_tls_client_config){
        .size = sizeof(tls_config),
        .ca_file = worker->ca_path,
        .server_name = "localhost",
        .alpn_protocols = alpn,
        .alpn_protocol_count = 1u};
    status = chttp_tls_profile_init(&profile, &tls_config);
    if (status != SALTS_OK) goto cleanup;
  }

  status = chttp_async_client_init(&client, &config);
  if (status != SALTS_OK) goto cleanup;
  if (worker->test_case->protocol == CHTTP_HTTP_2 &&
      (worker->test_case->stream_receive_window != 0u ||
       worker->test_case->connection_receive_window != 0u)) {
    status = chttp_async_client_set_h2_receive_window_policy(
        &client, worker->test_case->stream_receive_window,
        worker->test_case->connection_receive_window);
    if (status != SALTS_OK) goto cleanup;
  }
  if (worker->test_case->client_nodelay) {
    cnet_stream_socket_options socket_options =
        (cnet_stream_socket_options)CNET_STREAM_SOCKET_OPTIONS_INIT;
    socket_options.nodelay = 1;
    status = chttp_async_client_set_socket_options(&client, &socket_options);
    if (status != SALTS_OK) goto cleanup;
  }
  if (snprintf(uri, sizeof(uri), "%s://127.0.0.1:%u",
               worker->test_case->tls ? "tls" : "tcp",
               (unsigned int)worker->port) <= 0) {
    status = SALTS_EIO;
    goto cleanup;
  }

  options = (chttp_request_options){
      .connection_uri = uri,
      .authority = "localhost",
      .target = "/bench",
      .method = CHTTP_METHOD_GET,
      .on_complete = owner_proto_complete,
      .tls = worker->test_case->tls ? &profile : NULL,
      .protocol = worker->test_case->protocol};

  for (round = 0u; round < worker->warmup_rounds; ++round) {
    size_t ignored_index = 0u;
    status = owner_proto_round(
        &client, &options, worker->test_case->streams_per_connection,
        worker->test_case->payload_bytes,
        worker->test_case->retained ? (unsigned char)'r' : (unsigned char)'s',
        NULL, &ignored_index);
    if (status != SALTS_OK) goto cleanup;
  }

  atomic_store_explicit(&worker->status, SALTS_OK, memory_order_release);
  atomic_fetch_add_explicit(&worker->barrier->ready, 1, memory_order_acq_rel);
  announced = true;
  while (atomic_load_explicit(&worker->barrier->start,
                              memory_order_acquire) == 0)
    salts_thread_yield();

  for (round = 0u; round < worker->rounds; ++round) {
    status = owner_proto_round(
        &client, &options, worker->test_case->streams_per_connection,
        worker->test_case->payload_bytes,
        worker->test_case->retained ? (unsigned char)'r' : (unsigned char)'s',
        worker->latencies, &latency_index);
    if (status != SALTS_OK) {
      atomic_store_explicit(&worker->status, status, memory_order_release);
      goto cleanup;
    }
  }
  if (latency_index !=
      worker->rounds * worker->test_case->streams_per_connection) {
    atomic_store_explicit(&worker->status, SALTS_EPROTO, memory_order_release);
    goto cleanup;
  }

cleanup:
  if (!announced)
    atomic_fetch_add_explicit(&worker->barrier->ready, 1, memory_order_acq_rel);
  if (client.impl != NULL) {
    const int stop_status =
        chttp_async_client_stop(&client, OWNER_PROTO_TIMEOUT_MS);
    if (status == SALTS_OK && stop_status != SALTS_OK) status = stop_status;
    {
      const int destroy_status = chttp_async_client_destroy(&client);
      if (status == SALTS_OK && destroy_status != SALTS_OK)
        status = destroy_status;
    }
  }
  if (profile.impl != NULL) {
    const int profile_status = chttp_tls_profile_destroy(&profile);
    if (status == SALTS_OK && profile_status != SALTS_OK)
      status = profile_status;
  }
  if (status != SALTS_OK)
    atomic_store_explicit(&worker->status, status, memory_order_release);
}

static void owner_proto_sample_pressure(chttp_server_impl *impl,
                                        owner_proto_pressure *pressure,
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

static int owner_proto_run(const owner_proto_case *test_case,
                           size_t owner_count, size_t rounds,
                           size_t warmup_rounds, owner_proto_route *route,
                           const char *cert_path, const char *key_path,
                           const char *ca_path) {
  static const char *H1_ALPN[] = {"http/1.1"};
  static const char *H2_ALPN[] = {"h2"};
  chttp_server server = {0};
  chttp_server_config config = owner_proto_server_config(test_case);
  chttp_server_execution_options execution =
      (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
  cnet_tls_server_config tls_config = {0};
  chttp_server_stats stats = {0};
  chttp_server_impl *impl;
  owner_proto_barrier barrier;
  owner_proto_worker workers[OWNER_PROTO_CONNECTIONS];
  salts_thread_t threads[OWNER_PROTO_CONNECTIONS] = {0};
  bool thread_started[OWNER_PROTO_CONNECTIONS] = {false};
  owner_proto_pressure pressure = {0};
  uint64_t *latencies = NULL;
  size_t latencies_per_worker =
      rounds * test_case->streams_per_connection;
  size_t total_samples =
      latencies_per_worker * OWNER_PROTO_CONNECTIONS;
  uint16_t port = 0u;
  uint64_t started_ns;
  uint64_t wall_ns;
  clock_t cpu_started;
  clock_t cpu_elapsed;
  size_t index;
  int result = 1;
  int status;

  if (owner_count == 0u || owner_count > 4u ||
      owner_count > config.network.connection_capacity ||
      latencies_per_worker == 0u ||
      total_samples / OWNER_PROTO_CONNECTIONS != latencies_per_worker)
    return 1;
  latencies = (uint64_t *)calloc(total_samples, sizeof(*latencies));
  if (latencies == NULL) return 1;
  atomic_init(&barrier.ready, 0);
  atomic_init(&barrier.start, 0);
  memset(workers, 0, sizeof(workers));

  if (test_case->tls) {
    const char *const *alpn =
        test_case->protocol == CHTTP_HTTP_2 ? H2_ALPN : H1_ALPN;
    tls_config = (cnet_tls_server_config){
        .size = sizeof(tls_config),
        .cert_file = cert_path,
        .key_file = key_path,
        .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
        .alpn_protocols = alpn,
        .alpn_protocol_count = 1u};
    config.tls = &tls_config;
  }

  status = chttp_server_init(&server, &config);
  if (status != SALTS_OK) goto cleanup;
  if (test_case->server_nodelay) {
    chttp_server_socket_options socket_options =
        (chttp_server_socket_options)CHTTP_SERVER_SOCKET_OPTIONS_INIT;
    socket_options.stream.nodelay = 1;
    status = chttp_server_set_socket_options(&server, &socket_options);
    if (status != SALTS_OK) goto cleanup;
  }
  execution.owner_count = owner_count;
  status = chttp_server_set_execution_options(&server, &execution);
  if (status != SALTS_OK) goto cleanup;
  status = chttp_server_get(&server, "/bench", owner_proto_handler, route);
  if (status != SALTS_OK) goto cleanup;
  status = chttp_server_start(&server);
  if (status != SALTS_OK) goto cleanup;
  status = chttp_server_port(&server, &port);
  if (status != SALTS_OK || port == 0u) goto cleanup;
  impl = (chttp_server_impl *)server.impl;

  for (index = 0u; index < OWNER_PROTO_CONNECTIONS; ++index) {
    workers[index] = (owner_proto_worker){
        .test_case = test_case,
        .port = port,
        .ca_path = ca_path,
        .rounds = rounds,
        .warmup_rounds = warmup_rounds,
        .latencies = latencies + index * latencies_per_worker,
        .barrier = &barrier};
    atomic_init(&workers[index].status, SALTS_EIO);
    status = salts_thread_create(&threads[index], owner_proto_worker_main,
                                 &workers[index]);
    if (status != SALTS_OK) goto cleanup_threads;
    thread_started[index] = true;
  }

  while (atomic_load_explicit(&barrier.ready, memory_order_acquire) !=
         OWNER_PROTO_CONNECTIONS) {
    owner_proto_sample_pressure(impl, &pressure, owner_count);
    salts_thread_yield();
  }
  owner_proto_sample_pressure(impl, &pressure, owner_count);
  for (index = 0u; index < OWNER_PROTO_CONNECTIONS; ++index) {
    status = atomic_load_explicit(&workers[index].status, memory_order_acquire);
    if (status != SALTS_OK) {
      fprintf(stderr,
              "owner protocol warmup failed workload=%s owners=%zu worker=%zu status=%d\n",
              test_case->name, owner_count, index, status);
      goto cleanup_threads;
    }
  }

  pressure.cross_owner_handoffs = 0u;
  for (index = 1u; index < owner_count; ++index)
    pressure.cross_owner_handoffs += pressure.owner_leases[index];

  cpu_started = clock();
  started_ns = salts_hrtime();
  atomic_store_explicit(&barrier.start, 1, memory_order_release);

  for (index = 0u; index < OWNER_PROTO_CONNECTIONS; ++index) {
    if (salts_thread_join(&threads[index]) != SALTS_OK) {
      status = SALTS_EIO;
      goto cleanup_threads;
    }
    salts_thread_destroy(&threads[index]);
    thread_started[index] = false;
    status = atomic_load_explicit(&workers[index].status, memory_order_acquire);
    if (status != SALTS_OK) {
      fprintf(stderr,
              "owner protocol measured run failed workload=%s owners=%zu worker=%zu status=%d\n",
              test_case->name, owner_count, index, status);
      goto cleanup_threads;
    }
  }

  wall_ns = salts_hrtime() - started_ns;
  cpu_elapsed = clock() - cpu_started;
  status = chttp_server_get_stats(&server, &stats);
  if (status != SALTS_OK || stats.rejected_connections != 0u) goto cleanup;

  printf(
      "{\"kind\":\"measurement\","
      "\"benchmark\":\"%s\","
      "\"workload\":\"%s\","
      "\"protocol\":\"%s\","
      "\"transport\":\"%s\","
      "\"payload_bytes\":%zu,"
      "\"retained\":%s,"
      "\"owners\":%zu,"
      "\"connections\":%u,"
      "\"streams_per_connection\":%zu,"
      "\"rounds_per_connection\":%zu,"
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
      "\"stream_receive_window\":%u,"
      "\"connection_receive_window\":%u,"
      "\"client_nodelay\":%s,"
      "\"server_nodelay\":%s,"
      "\"cross_owner_data_plane_hops\":0,"
      "\"errors\":0}\n",
      test_case->benchmark_name != NULL
          ? test_case->benchmark_name
          : "chttp_server_owner_protocol_scaling",
      test_case->name,
      test_case->protocol == CHTTP_HTTP_2 ? "h2" : "h1",
      test_case->tls ? "tls" : "tcp",
      test_case->payload_bytes,
      test_case->retained ? "true" : "false",
      owner_count, OWNER_PROTO_CONNECTIONS,
      test_case->streams_per_connection, rounds, total_samples, total_samples,
      (unsigned long long)wall_ns,
      wall_ns != 0u ? (double)total_samples * 1.0e9 / (double)wall_ns : 0.0,
      total_samples != 0u
          ? ((double)cpu_elapsed * 1.0e9 / (double)CLOCKS_PER_SEC) /
                (double)total_samples
          : 0.0,
      (unsigned long long)owner_proto_percentile(latencies, total_samples, 50u),
      (unsigned long long)owner_proto_percentile(latencies, total_samples, 95u),
      (unsigned long long)owner_proto_percentile(latencies, total_samples, 99u),
      (unsigned long long)stats.accepted_connections,
      (unsigned long long)stats.rejected_connections,
      pressure.owner_leases[0], pressure.owner_leases[1],
      pressure.owner_leases[2], pressure.owner_leases[3],
      pressure.peak_owner_rings[0], pressure.peak_owner_rings[1],
      pressure.peak_owner_rings[2], pressure.peak_owner_rings[3],
      pressure.cross_owner_handoffs,
      test_case->stream_receive_window != 0u
          ? test_case->stream_receive_window
          : UINT32_C(65535),
      test_case->connection_receive_window != 0u
          ? test_case->connection_receive_window
          : UINT32_C(65535),
      test_case->client_nodelay ? "true" : "false",
      test_case->server_nodelay ? "true" : "false");
  fflush(stdout);
  result = 0;

cleanup_threads:
  atomic_store_explicit(&barrier.start, 1, memory_order_release);
  for (index = 0u; index < OWNER_PROTO_CONNECTIONS; ++index) {
    if (thread_started[index]) {
      (void)salts_thread_join(&threads[index]);
      salts_thread_destroy(&threads[index]);
    }
  }

cleanup:
  if (server.impl != NULL) {
    (void)chttp_server_stop(&server, OWNER_PROTO_TIMEOUT_MS);
    (void)chttp_server_destroy(&server);
  }
  free(latencies);
  return result;
}

int main(void) {
  static const owner_proto_case CASES[] = {
      {"h2-tcp-1k-copy", CHTTP_HTTP_2, false, false, false, false,
       OWNER_PROTO_SMALL_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h2-tcp-1k-copy-server-nodelay", CHTTP_HTTP_2, false, false, true, false,
       OWNER_PROTO_SMALL_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h2-tcp-1k-copy-client-nodelay", CHTTP_HTTP_2, false, false, false, true,
       OWNER_PROTO_SMALL_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h2-tcp-1k-copy-both-nodelay", CHTTP_HTTP_2, false, false, true, true,
       OWNER_PROTO_SMALL_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h2-tcp-64k-retained", CHTTP_HTTP_2, false, true, false, false,
       OWNER_PROTO_RETAINED_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h2-tcp-64k-retained-client-nodelay", CHTTP_HTTP_2, false, true, false, true,
       OWNER_PROTO_RETAINED_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h1-tls-1k-copy", CHTTP_HTTP_1_1, true, false, false, false,
       OWNER_PROTO_SMALL_BYTES, 1u, 0u, 0u, NULL},
      {"h1-tls-64k-retained", CHTTP_HTTP_1_1, true, true, false, false,
       OWNER_PROTO_RETAINED_BYTES, 1u, 0u, 0u, NULL},
      {"h2-tls-1k-copy", CHTTP_HTTP_2, true, false, false, false,
       OWNER_PROTO_SMALL_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h2-tls-1k-copy-client-nodelay", CHTTP_HTTP_2, true, false, false, true,
       OWNER_PROTO_SMALL_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h2-tls-16383-copy-single", CHTTP_HTTP_2, true, false, false, false,
       OWNER_PROTO_BELOW_WINDOW_BYTES, 1u, 0u, 0u, NULL},
      {"h2-tls-64k-copy-single", CHTTP_HTTP_2, true, false, false, false,
       OWNER_PROTO_RETAINED_BYTES, 1u, 0u, 0u, NULL},
      {"h2-tls-16383-copy", CHTTP_HTTP_2, true, false, false, false,
       OWNER_PROTO_BELOW_WINDOW_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h2-tls-16384-copy", CHTTP_HTTP_2, true, false, false, false,
       OWNER_PROTO_CROSS_WINDOW_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h2-tls-64k-copy", CHTTP_HTTP_2, true, false, false, false,
       OWNER_PROTO_RETAINED_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h2-tls-64k-copy-client-nodelay", CHTTP_HTTP_2, true, false, false, true,
       OWNER_PROTO_RETAINED_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h2-tls-64k-retained", CHTTP_HTTP_2, true, true, false, false,
       OWNER_PROTO_RETAINED_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL},
      {"h2-tls-64k-retained-client-nodelay", CHTTP_HTTP_2, true, true, false, true,
       OWNER_PROTO_RETAINED_BYTES, OWNER_PROTO_H2_DEPTH, 0u, 0u, NULL}};
  static const owner_proto_case FLOW_CASES[] = {
#define FLOW_COPY_CASE(name_, tls_, nodelay_, bytes_, stream_, conn_) \
      {.name = name_, .protocol = CHTTP_HTTP_2, .tls = tls_,            \
       .retained = false, .server_nodelay = false,                     \
       .client_nodelay = nodelay_, .payload_bytes = bytes_,            \
       .streams_per_connection = OWNER_PROTO_H2_DEPTH,                 \
       .stream_receive_window = stream_,                               \
       .connection_receive_window = conn_,                             \
       .benchmark_name = "chttp_h2_flow_window"}
#define FLOW_RETAINED_CASE(name_, tls_, bytes_, stream_, conn_) \
      {.name = name_, .protocol = CHTTP_HTTP_2, .tls = tls_,    \
       .retained = true, .server_nodelay = false,               \
       .client_nodelay = true, .payload_bytes = bytes_,         \
       .streams_per_connection = OWNER_PROTO_H2_DEPTH,          \
       .stream_receive_window = stream_,                        \
       .connection_receive_window = conn_,                      \
       .benchmark_name = "chttp_h2_flow_window"}

      FLOW_COPY_CASE("tcp-copy-16383-default", false, true,
                     OWNER_PROTO_BELOW_WINDOW_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tcp-copy-16383-wide", false, true,
                     OWNER_PROTO_BELOW_WINDOW_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tcp-copy-16384-default", false, true,
                     OWNER_PROTO_CROSS_WINDOW_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tcp-copy-16384-wide", false, true,
                     OWNER_PROTO_CROSS_WINDOW_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tcp-copy-32768-default", false, true,
                     OWNER_PROTO_32K_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tcp-copy-32768-wide", false, true,
                     OWNER_PROTO_32K_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tcp-copy-65535-default", false, true,
                     OWNER_PROTO_DEFAULT_WINDOW_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tcp-copy-65535-wide", false, true,
                     OWNER_PROTO_DEFAULT_WINDOW_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tcp-copy-65536-default", false, true,
                     OWNER_PROTO_RETAINED_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tcp-copy-65536-wide", false, true,
                     OWNER_PROTO_RETAINED_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tcp-copy-131072-default", false, true,
                     OWNER_PROTO_128K_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tcp-copy-131072-wide", false, true,
                     OWNER_PROTO_128K_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tcp-copy-65535-conn-wide", false, true,
                     OWNER_PROTO_DEFAULT_WINDOW_BYTES, 0u,
                     OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tcp-copy-65536-conn-wide", false, true,
                     OWNER_PROTO_RETAINED_BYTES, 0u,
                     OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tcp-copy-131072-conn-wide", false, true,
                     OWNER_PROTO_128K_BYTES, 0u,
                     OWNER_PROTO_FLOW_CONNECTION_WINDOW),

      FLOW_COPY_CASE("tls-copy-16383-default", true, true,
                     OWNER_PROTO_BELOW_WINDOW_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tls-copy-16383-wide", true, true,
                     OWNER_PROTO_BELOW_WINDOW_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tls-copy-16384-default", true, true,
                     OWNER_PROTO_CROSS_WINDOW_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tls-copy-16384-wide", true, true,
                     OWNER_PROTO_CROSS_WINDOW_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tls-copy-32768-default", true, true,
                     OWNER_PROTO_32K_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tls-copy-32768-wide", true, true,
                     OWNER_PROTO_32K_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tls-copy-65535-default", true, true,
                     OWNER_PROTO_DEFAULT_WINDOW_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tls-copy-65535-wide", true, true,
                     OWNER_PROTO_DEFAULT_WINDOW_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tls-copy-65536-default", true, true,
                     OWNER_PROTO_RETAINED_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tls-copy-65536-wide", true, true,
                     OWNER_PROTO_RETAINED_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tls-copy-131072-default", true, true,
                     OWNER_PROTO_128K_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tls-copy-131072-wide", true, true,
                     OWNER_PROTO_128K_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tls-copy-65535-conn-wide", true, true,
                     OWNER_PROTO_DEFAULT_WINDOW_BYTES, 0u,
                     OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tls-copy-65536-conn-wide", true, true,
                     OWNER_PROTO_RETAINED_BYTES, 0u,
                     OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tls-copy-131072-conn-wide", true, true,
                     OWNER_PROTO_128K_BYTES, 0u,
                     OWNER_PROTO_FLOW_CONNECTION_WINDOW),

      FLOW_RETAINED_CASE("tcp-retained-65535-default", false,
                         OWNER_PROTO_DEFAULT_WINDOW_BYTES, 0u, 0u),
      FLOW_RETAINED_CASE("tcp-retained-65535-wide", false,
                         OWNER_PROTO_DEFAULT_WINDOW_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_RETAINED_CASE("tcp-retained-65536-default", false,
                         OWNER_PROTO_RETAINED_BYTES, 0u, 0u),
      FLOW_RETAINED_CASE("tcp-retained-65536-wide", false,
                         OWNER_PROTO_RETAINED_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_RETAINED_CASE("tcp-retained-131072-default", false,
                         OWNER_PROTO_128K_BYTES, 0u, 0u),
      FLOW_RETAINED_CASE("tcp-retained-131072-wide", false,
                         OWNER_PROTO_128K_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_RETAINED_CASE("tcp-retained-65535-conn-wide", false,
                         OWNER_PROTO_DEFAULT_WINDOW_BYTES, 0u,
                         OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_RETAINED_CASE("tcp-retained-65536-conn-wide", false,
                         OWNER_PROTO_RETAINED_BYTES, 0u,
                         OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_RETAINED_CASE("tcp-retained-131072-conn-wide", false,
                         OWNER_PROTO_128K_BYTES, 0u,
                         OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_RETAINED_CASE("tls-retained-65535-default", true,
                         OWNER_PROTO_DEFAULT_WINDOW_BYTES, 0u, 0u),
      FLOW_RETAINED_CASE("tls-retained-65535-wide", true,
                         OWNER_PROTO_DEFAULT_WINDOW_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_RETAINED_CASE("tls-retained-65536-default", true,
                         OWNER_PROTO_RETAINED_BYTES, 0u, 0u),
      FLOW_RETAINED_CASE("tls-retained-65536-wide", true,
                         OWNER_PROTO_RETAINED_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_RETAINED_CASE("tls-retained-131072-default", true,
                         OWNER_PROTO_128K_BYTES, 0u, 0u),
      FLOW_RETAINED_CASE("tls-retained-131072-wide", true,
                         OWNER_PROTO_128K_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_RETAINED_CASE("tls-retained-65535-conn-wide", true,
                         OWNER_PROTO_DEFAULT_WINDOW_BYTES, 0u,
                         OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_RETAINED_CASE("tls-retained-65536-conn-wide", true,
                         OWNER_PROTO_RETAINED_BYTES, 0u,
                         OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_RETAINED_CASE("tls-retained-131072-conn-wide", true,
                         OWNER_PROTO_128K_BYTES, 0u,
                         OWNER_PROTO_FLOW_CONNECTION_WINDOW),

      /* Nagle/delayed-ACK attribution controls around the RFC window edge. */
      FLOW_COPY_CASE("tcp-copy-65535-default-nagle", false, false,
                     OWNER_PROTO_DEFAULT_WINDOW_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tcp-copy-65536-default-nagle", false, false,
                     OWNER_PROTO_RETAINED_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tcp-copy-65535-wide-nagle", false, false,
                     OWNER_PROTO_DEFAULT_WINDOW_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tcp-copy-65536-wide-nagle", false, false,
                     OWNER_PROTO_RETAINED_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tls-copy-65535-default-nagle", true, false,
                     OWNER_PROTO_DEFAULT_WINDOW_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tls-copy-65536-default-nagle", true, false,
                     OWNER_PROTO_RETAINED_BYTES, 0u, 0u),
      FLOW_COPY_CASE("tls-copy-65535-wide-nagle", true, false,
                     OWNER_PROTO_DEFAULT_WINDOW_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW),
      FLOW_COPY_CASE("tls-copy-65536-wide-nagle", true, false,
                     OWNER_PROTO_RETAINED_BYTES, 131072u, OWNER_PROTO_FLOW_CONNECTION_WINDOW)
#undef FLOW_RETAINED_CASE
#undef FLOW_COPY_CASE
  };

  static const size_t OWNERS[] = {1u, 2u, 4u};
  const size_t small_rounds =
      owner_proto_env_count("CHTTP_OWNER_PROTO_SMALL_ROUNDS",
                            OWNER_PROTO_SMALL_ROUNDS);
  const size_t retained_rounds =
      owner_proto_env_count("CHTTP_OWNER_PROTO_RETAINED_ROUNDS",
                            OWNER_PROTO_RETAINED_ROUNDS);
  const size_t warmup_rounds =
      owner_proto_env_count("CHTTP_OWNER_PROTO_WARMUP_ROUNDS",
                            OWNER_PROTO_WARMUP_ROUNDS);
  const size_t flow_rounds =
      owner_proto_env_count("CHTTP_H2_FLOW_WINDOW_ROUNDS", 4u);
  const char *flow_mode_env = getenv("CHTTP_H2_FLOW_WINDOW_MODE");
  const bool flow_mode =
      flow_mode_env != NULL && strcmp(flow_mode_env, "0") != 0;
  owner_proto_route small_route;
  owner_proto_route below_window_copy_route;
  owner_proto_route cross_window_copy_route;
  owner_proto_route large_copy_route;
  owner_proto_route retained_route;
  owner_proto_route flow_copy_route;
  mem_buffer_t *retained = NULL;
  char *cert_path = NULL;
  char *key_path = NULL;
  char *ca_path = NULL;
  size_t case_index;
  size_t owner_index;
  int result = 0;

  memset(OWNER_PROTO_SMALL_BODY, 's', sizeof(OWNER_PROTO_SMALL_BODY));
  memset(OWNER_PROTO_LARGE_COPY_BODY, 's', sizeof(OWNER_PROTO_LARGE_COPY_BODY));
  memset(OWNER_PROTO_RETAINED_BODY, 'r', sizeof(OWNER_PROTO_RETAINED_BODY));
  retained = mem_wrap_external(OWNER_PROTO_RETAINED_BODY,
                               sizeof(OWNER_PROTO_RETAINED_BODY), NULL, NULL);
  if (retained == NULL) return 2;
  mem_set_used(retained, OWNER_PROTO_RETAINED_BYTES);

  cert_path = tt_make_temp_file("chttp-owner-cert", ".pem");
  key_path = tt_make_temp_file("chttp-owner-key", ".pem");
  ca_path = tt_make_temp_file("chttp-owner-ca", ".pem");
  if (cert_path == NULL || key_path == NULL || ca_path == NULL ||
      tt_write_file(cert_path, CHTTP_TLS_TEST_CERTIFICATE,
                    sizeof(CHTTP_TLS_TEST_CERTIFICATE) - 1u) != 0 ||
      tt_write_file(key_path, CHTTP_TLS_TEST_KEY,
                    sizeof(CHTTP_TLS_TEST_KEY) - 1u) != 0 ||
      tt_write_file(ca_path, CHTTP_TLS_TEST_CA_CERTIFICATE,
                    sizeof(CHTTP_TLS_TEST_CA_CERTIFICATE) - 1u) != 0) {
    result = 3;
    goto cleanup;
  }

  small_route = (owner_proto_route){
      .small_body = OWNER_PROTO_SMALL_BODY,
      .small_size = sizeof(OWNER_PROTO_SMALL_BODY),
      .retained = false};
  below_window_copy_route = (owner_proto_route){
      .small_body = OWNER_PROTO_LARGE_COPY_BODY,
      .small_size = OWNER_PROTO_BELOW_WINDOW_BYTES,
      .retained = false};
  cross_window_copy_route = (owner_proto_route){
      .small_body = OWNER_PROTO_LARGE_COPY_BODY,
      .small_size = OWNER_PROTO_CROSS_WINDOW_BYTES,
      .retained = false};
  large_copy_route = (owner_proto_route){
      .small_body = OWNER_PROTO_LARGE_COPY_BODY,
      .small_size = OWNER_PROTO_RETAINED_BYTES,
      .retained = false};
  retained_route = (owner_proto_route){
      .retained_body = retained,
      .retained = true};
  flow_copy_route = (owner_proto_route){
      .small_body = OWNER_PROTO_LARGE_COPY_BODY,
      .small_size = OWNER_PROTO_SMALL_BYTES,
      .retained = false};

  printf(
      "{\"kind\":\"environment\","
      "\"benchmark\":\"%s\","
      "\"commit\":\"%s\","
      "\"backend\":\"%s\","
      "\"connections\":%u,"
      "\"h2_streams_per_connection\":%u,"
      "\"small_rounds\":%zu,"
      "\"retained_rounds\":%zu,"
      "\"warmup_rounds\":%zu,"
      "\"flow_rounds\":%zu,"
      "\"flow_mode\":%s,"
      "\"note\":\"TLS profile and handshake are completed in warmup; H2 submits four streams before polling completions\"}\n",
      flow_mode ? "chttp_h2_flow_window"
                : "chttp_server_owner_protocol_scaling",
      getenv("GITHUB_SHA") != NULL ? getenv("GITHUB_SHA") : "unknown",
#if defined(_WIN32)
      "iocp",
#elif defined(__linux__)
      "epoll",
#else
      "kqueue",
#endif
      OWNER_PROTO_CONNECTIONS, OWNER_PROTO_H2_DEPTH,
      small_rounds, retained_rounds, warmup_rounds, flow_rounds,
      flow_mode ? "true" : "false");
  fflush(stdout);

  {
    const owner_proto_case *selected_cases =
        flow_mode ? FLOW_CASES : CASES;
    const size_t selected_count =
        flow_mode ? sizeof(FLOW_CASES) / sizeof(FLOW_CASES[0])
                  : sizeof(CASES) / sizeof(CASES[0]);
    for (case_index = 0u; case_index < selected_count; ++case_index) {
      const owner_proto_case *test_case = &selected_cases[case_index];
      owner_proto_route *route;
      size_t rounds;
      if (flow_mode) {
        if (test_case->retained) {
          mem_set_used(retained, test_case->payload_bytes);
          route = &retained_route;
        } else {
          flow_copy_route.small_size = test_case->payload_bytes;
          route = &flow_copy_route;
        }
        rounds = flow_rounds;
      } else {
        route =
            test_case->retained
                ? &retained_route
                : (test_case->payload_bytes == OWNER_PROTO_BELOW_WINDOW_BYTES
                       ? &below_window_copy_route
                       : (test_case->payload_bytes == OWNER_PROTO_CROSS_WINDOW_BYTES
                              ? &cross_window_copy_route
                              : (test_case->payload_bytes == OWNER_PROTO_RETAINED_BYTES
                                     ? &large_copy_route
                                     : &small_route)));
        rounds =
            test_case->payload_bytes == OWNER_PROTO_SMALL_BYTES
                ? small_rounds
                : retained_rounds;
      }
    for (owner_index = 0u;
         owner_index < sizeof(OWNERS) / sizeof(OWNERS[0]); ++owner_index) {
      if (owner_proto_run(test_case, OWNERS[owner_index], rounds,
                          warmup_rounds, route, cert_path, key_path,
                          ca_path) != 0) {
        fprintf(stderr,
                "protocol owner benchmark failed workload=%s owners=%zu\n",
                test_case->name, OWNERS[owner_index]);
        result = 4;
        goto cleanup;
      }
    }
  }

cleanup:
  if (cert_path != NULL) {
    (void)tt_remove_file(cert_path);
    free(cert_path);
  }
  if (key_path != NULL) {
    (void)tt_remove_file(key_path);
    free(key_path);
  }
  if (ca_path != NULL) {
    (void)tt_remove_file(ca_path);
    free(ca_path);
  }
  mem_buffer_release(retained);
  return result;
}
