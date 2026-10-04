#include "chttp_server_runtime.h"
#include "chttp_tls_test_material.h"
#include "tinytest.h"

#include <http_client/http.h>
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

enum {
  WS_BENCH_CONNECTIONS = 8,
  WS_BENCH_TIMEOUT_MS = 10000,
  WS_BENCH_SMALL_BYTES = 64,
  WS_BENCH_LARGE_BYTES = 64 * 1024,
  WS_BENCH_SMALL_ROUNDS = 240,
  WS_BENCH_LARGE_ROUNDS = 32,
  WS_BENCH_WARMUP_ROUNDS = 2
};

typedef struct ws_bench_case {
  const char *name;
  bool tls;
  bool copied_command;
  size_t payload_bytes;
  size_t rounds;
  const char *latency_kind;
} ws_bench_case;

typedef struct ws_bench_shared {
  const ws_bench_case *test_case;
  chttp_server_websocket_session sessions[WS_BENCH_CONNECTIONS];
  const unsigned char *payload;
  atomic_uint_fast64_t *push_started_ns;
  atomic_int captured;
  atomic_int connected;
  atomic_int ready;
  atomic_int done;
  atomic_int first_error;
  atomic_size_t received;
  atomic_bool start;
  atomic_bool cleanup;
  atomic_int close_events;
} ws_bench_shared;

typedef struct ws_bench_worker {
  ws_bench_shared *shared;
  size_t index;
  uint16_t port;
  const char *ca_path;
  uint64_t *latencies;
} ws_bench_worker;

typedef struct ws_bench_pressure {
  size_t owner_leases[4];
  size_t peak_owner_commands[4];
  size_t peak_owner_rings[4];
  size_t admission_handoffs;
} ws_bench_pressure;

static unsigned char WS_BENCH_SMALL_PAYLOAD[WS_BENCH_SMALL_BYTES];
static unsigned char WS_BENCH_LARGE_PAYLOAD[WS_BENCH_LARGE_BYTES];

static int ws_bench_u64_compare(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t ws_bench_percentile(const uint64_t *values, size_t count,
                                    unsigned int percent) {
  uint64_t *ordered;
  size_t rank;
  uint64_t result;
  if (values == NULL || count == 0u) return 0u;
  ordered = (uint64_t *)malloc(count * sizeof(*ordered));
  if (ordered == NULL) return 0u;
  memcpy(ordered, values, count * sizeof(*ordered));
  qsort(ordered, count, sizeof(*ordered), ws_bench_u64_compare);
  rank = ((size_t)percent * count + 99u) / 100u;
  if (rank == 0u) rank = 1u;
  if (rank > count) rank = count;
  result = ordered[rank - 1u];
  free(ordered);
  return result;
}

static size_t ws_bench_env_count(const char *name, size_t fallback) {
  const char *value = getenv(name);
  char *end = NULL;
  unsigned long parsed;
  if (value == NULL || value[0] == '\0') return fallback;
  parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0ul) return fallback;
  return (size_t)parsed;
}

static native_io_backend_kind ws_bench_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static void ws_bench_set_error(ws_bench_shared *shared, int status) {
  int expected = SALTS_OK;
  if (shared == NULL || status == SALTS_OK) return;
  (void)atomic_compare_exchange_strong_explicit(
      &shared->first_error, &expected, status,
      memory_order_acq_rel, memory_order_acquire);
}

static chttp_server_config ws_bench_server_config(bool tls) {
  chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 64u,
      .network = {
          .backend = ws_bench_backend(),
          .connection_capacity = 32u,
          .command_capacity = 64u,
          .request_capacity = 32u,
          .completion_batch_capacity = 64u,
          .event_capacity = 128u,
          .max_send_bytes = 128u * 1024u,
          .receive_buffer_bytes = 128u * 1024u,
          .connect_timeout_ms = WS_BENCH_TIMEOUT_MS,
          .read_timeout_ms = WS_BENCH_TIMEOUT_MS,
          .write_timeout_ms = WS_BENCH_TIMEOUT_MS},
      .route_capacity = 2u,
      .middleware_capacity = 0u,
      .max_route_middleware_count = 0u,
      .max_route_param_count = 1u,
      .max_route_param_bytes = 16u,
      .max_target_bytes = 128u,
      .max_header_count = 16u,
      .max_header_bytes = 2048u,
      .max_request_body_bytes = 1024u,
      .max_response_header_count = 16u,
      .max_response_header_bytes = 2048u,
      .max_response_body_bytes = 1024u,
      .session_capacity = 0u,
      .poll_slice_ms = 1u,
      .max_buffered_response_body_bytes = 1024u,
      .buffer_capacity_bytes = 64u * 1024u * 1024u};
  if (tls) {
    config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    config.network.tls_handshake_timeout_ms = WS_BENCH_TIMEOUT_MS;
  }
  return config;
}

static chttp_websocket_client_config ws_bench_client_config(bool tls) {
  chttp_websocket_client_config config = {
      .size = sizeof(config),
      .network = {
          .backend = ws_bench_backend(),
          .connection_capacity = 1u,
          .command_capacity = 32u,
          .request_capacity = 8u,
          .completion_batch_capacity = 16u,
          .event_capacity = 64u,
          .max_send_bytes = 128u * 1024u,
          .receive_buffer_bytes = 128u * 1024u,
          .connect_timeout_ms = WS_BENCH_TIMEOUT_MS,
          .read_timeout_ms = WS_BENCH_TIMEOUT_MS,
          .write_timeout_ms = WS_BENCH_TIMEOUT_MS},
      .max_frame_bytes = WS_BENCH_LARGE_BYTES,
      .max_message_bytes = WS_BENCH_LARGE_BYTES,
      .max_buffered_input_bytes = 128u * 1024u,
      .max_handshake_header_bytes = 4096u,
      .event_capacity = 32u};
  if (tls) {
    config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    config.network.tls_handshake_timeout_ms = WS_BENCH_TIMEOUT_MS;
  }
  return config;
}

static int ws_bench_open(void *user, chttp_websocket *websocket,
                         const chttp_server_request_view *request,
                         chttp_server_response *response) {
  ws_bench_shared *shared = (ws_bench_shared *)user;
  const char *id_text;
  char *end = NULL;
  unsigned long id;
  int status;
  (void)response;
  if (shared == NULL || websocket == NULL || request == NULL)
    return SALTS_EINVAL;
  id_text = chttp_server_request_param(request, "id");
  if (id_text == NULL) return SALTS_EINVAL;
  id = strtoul(id_text, &end, 10);
  if (end == id_text || end == NULL || *end != '\0' ||
      id >= WS_BENCH_CONNECTIONS)
    return SALTS_EINVAL;
  status = chttp_server_websocket_session_capture(
      websocket, &shared->sessions[id]);
  if (status != SALTS_OK) return status;
  atomic_fetch_add_explicit(&shared->captured, 1, memory_order_acq_rel);
  return SALTS_OK;
}

static void ws_bench_event(void *user, chttp_websocket *websocket,
                           const chttp_websocket_event *event) {
  ws_bench_shared *shared = (ws_bench_shared *)user;
  int status;
  if (shared == NULL || websocket == NULL || event == NULL) return;
  if (event->kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
    atomic_fetch_add_explicit(
        &shared->close_events, 1, memory_order_acq_rel);
    return;
  }
  if (event->kind != CHTTP_WEBSOCKET_EVENT_MESSAGE ||
      shared->test_case == NULL ||
      shared->test_case->copied_command)
    return;
  if (event->message_type != CHTTP_WEBSOCKET_MESSAGE_BINARY ||
      event->size != shared->test_case->payload_bytes ||
      event->data == NULL ||
      event->data[0] != shared->payload[0] ||
      event->data[event->size - 1u] !=
          shared->payload[shared->test_case->payload_bytes - 1u]) {
    ws_bench_set_error(shared, SALTS_EPROTO);
    return;
  }
  status = chttp_websocket_send_binary(websocket, event->data, event->size);
  ws_bench_set_error(shared, status);
}

static int ws_bench_expect_message(chttp_websocket_client *client,
                                   const ws_bench_case *test_case,
                                   const unsigned char *payload) {
  chttp_websocket_event event = {0};
  int status = chttp_websocket_client_receive(
      client, WS_BENCH_TIMEOUT_MS, &event);
  if (status != SALTS_OK) return status;
  if (event.kind != CHTTP_WEBSOCKET_EVENT_MESSAGE ||
      event.message_type != CHTTP_WEBSOCKET_MESSAGE_BINARY ||
      event.size != test_case->payload_bytes ||
      event.data == NULL ||
      event.data[0] != payload[0] ||
      event.data[event.size - 1u] != payload[test_case->payload_bytes - 1u])
    return SALTS_EPROTO;
  return SALTS_OK;
}

static void ws_bench_worker_main(void *user) {
  static const char *H1_ALPN[] = {"http/1.1"};
  ws_bench_worker *worker = (ws_bench_worker *)user;
  ws_bench_shared *shared;
  chttp_websocket_client client = {0};
  chttp_tls_profile profile = {0};
  chttp_websocket_client_config config;
  cnet_tls_client_config tls_config = {0};
  chttp_websocket_connect_options options = {
      .size = sizeof(options)};
  char uri[128];
  unsigned int http_status = 0u;
  size_t round;
  bool announced = false;
  int status = SALTS_OK;

  if (worker == NULL || worker->shared == NULL) return;
  shared = worker->shared;
  config = ws_bench_client_config(shared->test_case->tls);

  if (shared->test_case->tls) {
    tls_config = (cnet_tls_client_config){
        .size = sizeof(tls_config),
        .ca_file = worker->ca_path,
        .server_name = "localhost",
        .alpn_protocols = H1_ALPN,
        .alpn_protocol_count = 1u};
    status = chttp_tls_profile_init(&profile, &tls_config);
    if (status != SALTS_OK) goto cleanup;
  }

  status = chttp_websocket_client_init(&client, &config);
  if (status != SALTS_OK) goto cleanup;
  if (snprintf(
          uri, sizeof(uri), "%s://127.0.0.1:%u/bench/%zu",
          shared->test_case->tls ? "wss" : "ws",
          (unsigned int)worker->port, worker->index) <= 0) {
    status = SALTS_EIO;
    goto cleanup;
  }
  options.uri = uri;
  options.tls = shared->test_case->tls ? &profile : NULL;
  options.timeout_ms = WS_BENCH_TIMEOUT_MS;
  status = chttp_websocket_client_connect(
      &client, &options, &http_status);
  if (status != SALTS_OK || http_status != 101u) {
    status = status == SALTS_OK ? SALTS_EPROTO : status;
    goto cleanup;
  }
  atomic_fetch_add_explicit(
      &shared->connected, 1, memory_order_acq_rel);

  if (shared->test_case->copied_command) {
    for (round = 0u; round < WS_BENCH_WARMUP_ROUNDS; ++round) {
      status = ws_bench_expect_message(
          &client, shared->test_case, shared->payload);
      if (status != SALTS_OK) goto cleanup;
      atomic_fetch_add_explicit(
          &shared->received, 1u, memory_order_acq_rel);
    }
  } else {
    for (round = 0u; round < WS_BENCH_WARMUP_ROUNDS; ++round) {
      status = chttp_websocket_client_send_binary(
          &client, shared->payload, shared->test_case->payload_bytes,
          WS_BENCH_TIMEOUT_MS);
      if (status != SALTS_OK) goto cleanup;
      status = ws_bench_expect_message(
          &client, shared->test_case, shared->payload);
      if (status != SALTS_OK) goto cleanup;
    }
  }

  atomic_store_explicit(&worker->shared->first_error,
                        atomic_load_explicit(&worker->shared->first_error,
                                             memory_order_acquire),
                        memory_order_release);
  atomic_fetch_add_explicit(
      &shared->ready, 1, memory_order_acq_rel);
  announced = true;
  while (!atomic_load_explicit(&shared->start, memory_order_acquire))
    salts_thread_yield();

  if (shared->test_case->copied_command) {
    for (round = 0u; round < shared->test_case->rounds; ++round) {
      const size_t sample =
          worker->index * shared->test_case->rounds + round;
      status = ws_bench_expect_message(
          &client, shared->test_case, shared->payload);
      if (status != SALTS_OK) goto cleanup;
      worker->latencies[round] =
          salts_hrtime() -
          atomic_load_explicit(&shared->push_started_ns[sample],
                               memory_order_acquire);
      atomic_fetch_add_explicit(
          &shared->received, 1u, memory_order_acq_rel);
    }
  } else {
    for (round = 0u; round < shared->test_case->rounds; ++round) {
      const uint64_t started_ns = salts_hrtime();
      status = chttp_websocket_client_send_binary(
          &client, shared->payload, shared->test_case->payload_bytes,
          WS_BENCH_TIMEOUT_MS);
      if (status != SALTS_OK) goto cleanup;
      status = ws_bench_expect_message(
          &client, shared->test_case, shared->payload);
      if (status != SALTS_OK) goto cleanup;
      worker->latencies[round] = salts_hrtime() - started_ns;
    }
  }

cleanup:
  if (status != SALTS_OK) ws_bench_set_error(shared, status);
  if (!announced)
    atomic_fetch_add_explicit(
        &shared->ready, 1, memory_order_acq_rel);
  atomic_fetch_add_explicit(
      &shared->done, 1, memory_order_acq_rel);
  while (!atomic_load_explicit(&shared->cleanup, memory_order_acquire))
    salts_thread_yield();

  if (client.impl != NULL) {
    const int close_status = chttp_websocket_client_close(
        &client, 1000u, NULL, 0u, WS_BENCH_TIMEOUT_MS);
    if (status == SALTS_OK && close_status != SALTS_OK)
      status = close_status;
    {
      const int destroy_status =
          chttp_websocket_client_destroy(&client, WS_BENCH_TIMEOUT_MS);
      if (status == SALTS_OK && destroy_status != SALTS_OK)
        status = destroy_status;
    }
  }
  if (profile.impl != NULL) {
    const int profile_status = chttp_tls_profile_destroy(&profile);
    if (status == SALTS_OK && profile_status != SALTS_OK)
      status = profile_status;
  }
  if (status != SALTS_OK) ws_bench_set_error(shared, status);
}

static void ws_bench_sample_pressure(chttp_server_impl *impl,
                                     ws_bench_pressure *pressure,
                                     size_t owner_count) {
  size_t index;
  if (impl == NULL || pressure == NULL) return;
  salts_mutex_lock(&impl->mutex);
  for (index = 0u; index < owner_count && index < 4u; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(impl, index);
    size_t ring = 0u;
    if (owner == NULL) continue;
    pressure->owner_leases[index] =
        chttp_server_owner_lease_count(owner);
    if (owner->websocket_command_count >
        pressure->peak_owner_commands[index])
      pressure->peak_owner_commands[index] =
          owner->websocket_command_count;
    if (owner->admission_sync_initialized) {
      salts_mutex_lock(&owner->admission_mutex);
      ring = owner->admission_count;
      salts_mutex_unlock(&owner->admission_mutex);
    }
    if (ring > pressure->peak_owner_rings[index])
      pressure->peak_owner_rings[index] = ring;
  }
  salts_mutex_unlock(&impl->mutex);
}

static int ws_bench_send_copy_round(ws_bench_shared *shared,
                                    ws_bench_pressure *pressure,
                                    chttp_server_impl *impl,
                                    size_t owner_count,
                                    size_t round) {
  size_t index;
  const size_t warmup_messages =
      WS_BENCH_WARMUP_ROUNDS * WS_BENCH_CONNECTIONS;
  const size_t target =
      warmup_messages + (round + 1u) * WS_BENCH_CONNECTIONS;
  for (index = 0u; index < WS_BENCH_CONNECTIONS; ++index) {
    const size_t sample =
        index * shared->test_case->rounds + round;
    int status;
    atomic_store_explicit(&shared->push_started_ns[sample],
                          salts_hrtime(), memory_order_release);
    status = chttp_server_websocket_send_binary(
        &shared->sessions[index], shared->payload,
        shared->test_case->payload_bytes);
    if (status != SALTS_OK) return status;
    ws_bench_sample_pressure(impl, pressure, owner_count);
  }
  while (atomic_load_explicit(
             &shared->received, memory_order_acquire) < target) {
    if (atomic_load_explicit(
            &shared->first_error, memory_order_acquire) != SALTS_OK)
      return atomic_load_explicit(
          &shared->first_error, memory_order_acquire);
    ws_bench_sample_pressure(impl, pressure, owner_count);
    salts_thread_yield();
  }
  return SALTS_OK;
}

static int ws_bench_run(const ws_bench_case *test_case,
                        size_t owner_count,
                        const char *cert_path,
                        const char *key_path,
                        const char *ca_path) {
  static const char *H1_ALPN[] = {"http/1.1"};
  chttp_server server = {0};
  chttp_server_config config =
      ws_bench_server_config(test_case->tls);
  chttp_server_execution_options execution =
      (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
  cnet_tls_server_config tls_config = {0};
  chttp_server_websocket_options route = {
      .size = sizeof(route),
      .path = "/bench/:id",
      .max_frame_bytes = WS_BENCH_LARGE_BYTES,
      .max_message_bytes = WS_BENCH_LARGE_BYTES,
      .max_buffered_input_bytes = 128u * 1024u,
      .on_open = ws_bench_open,
      .on_event = ws_bench_event};
  chttp_server_stats stats = {0};
  chttp_server_impl *impl;
  ws_bench_shared shared;
  ws_bench_worker workers[WS_BENCH_CONNECTIONS];
  salts_thread_t threads[WS_BENCH_CONNECTIONS] = {0};
  bool thread_started[WS_BENCH_CONNECTIONS] = {false};
  ws_bench_pressure pressure = {0};
  uint64_t *latencies = NULL;
  atomic_uint_fast64_t *push_started_ns = NULL;
  uint16_t port = 0u;
  uint64_t started_ns;
  uint64_t wall_ns;
  clock_t cpu_started;
  clock_t cpu_elapsed;
  size_t total_ops =
      test_case->rounds * WS_BENCH_CONNECTIONS;
  size_t index;
  int status = SALTS_OK;
  int result = 1;

  if (owner_count == 0u || owner_count > 4u ||
      total_ops / WS_BENCH_CONNECTIONS != test_case->rounds)
    return 1;
  latencies = (uint64_t *)calloc(total_ops, sizeof(*latencies));
  if (latencies == NULL) return 1;
  if (test_case->copied_command) {
    push_started_ns =
        (atomic_uint_fast64_t *)calloc(
            total_ops, sizeof(*push_started_ns));
    if (push_started_ns == NULL) goto cleanup;
  }

  memset(&shared, 0, sizeof(shared));
  shared.test_case = test_case;
  shared.payload = test_case->payload_bytes == WS_BENCH_SMALL_BYTES
                       ? WS_BENCH_SMALL_PAYLOAD
                       : WS_BENCH_LARGE_PAYLOAD;
  shared.push_started_ns = push_started_ns;
  atomic_init(&shared.captured, 0);
  atomic_init(&shared.connected, 0);
  atomic_init(&shared.ready, 0);
  atomic_init(&shared.done, 0);
  atomic_init(&shared.first_error, SALTS_OK);
  atomic_init(&shared.received, 0u);
  atomic_init(&shared.start, false);
  atomic_init(&shared.cleanup, false);
  atomic_init(&shared.close_events, 0);
  route.user = &shared;

  if (test_case->tls) {
    tls_config = (cnet_tls_server_config){
        .size = sizeof(tls_config),
        .cert_file = cert_path,
        .key_file = key_path,
        .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
        .alpn_protocols = H1_ALPN,
        .alpn_protocol_count = 1u};
    config.tls = &tls_config;
  }

  status = chttp_server_init(&server, &config);
  if (status != SALTS_OK) goto cleanup;
  execution.owner_count = owner_count;
  status = chttp_server_set_execution_options(&server, &execution);
  if (status != SALTS_OK) goto cleanup;
  status = chttp_server_websocket_with(&server, &route);
  if (status != SALTS_OK) goto cleanup;
  status = chttp_server_start(&server);
  if (status != SALTS_OK) goto cleanup;
  status = chttp_server_port(&server, &port);
  if (status != SALTS_OK || port == 0u) goto cleanup;
  impl = (chttp_server_impl *)server.impl;

  for (index = 0u; index < WS_BENCH_CONNECTIONS; ++index) {
    workers[index] = (ws_bench_worker){
        .shared = &shared,
        .index = index,
        .port = port,
        .ca_path = ca_path,
        .latencies = latencies + index * test_case->rounds};
    status = salts_thread_create(
        &threads[index], ws_bench_worker_main, &workers[index]);
    if (status != SALTS_OK) goto cleanup_threads;
    thread_started[index] = true;
  }

  if (test_case->copied_command) {
    while (atomic_load_explicit(
               &shared.connected, memory_order_acquire) !=
               WS_BENCH_CONNECTIONS ||
           atomic_load_explicit(
               &shared.captured, memory_order_acquire) !=
               WS_BENCH_CONNECTIONS) {
      ws_bench_sample_pressure(impl, &pressure, owner_count);
      if (atomic_load_explicit(
              &shared.first_error, memory_order_acquire) != SALTS_OK) {
        status = atomic_load_explicit(
            &shared.first_error, memory_order_acquire);
        goto cleanup_threads;
      }
      salts_thread_yield();
    }
    for (size_t warmup = 0u;
         warmup < WS_BENCH_WARMUP_ROUNDS; ++warmup) {
      const size_t target =
          (warmup + 1u) * WS_BENCH_CONNECTIONS;
      for (index = 0u; index < WS_BENCH_CONNECTIONS; ++index) {
        status = chttp_server_websocket_send_binary(
            &shared.sessions[index], shared.payload,
            test_case->payload_bytes);
        if (status != SALTS_OK) goto cleanup_threads;
        ws_bench_sample_pressure(impl, &pressure, owner_count);
      }
      while (atomic_load_explicit(
                 &shared.received, memory_order_acquire) < target) {
        ws_bench_sample_pressure(impl, &pressure, owner_count);
        if (atomic_load_explicit(
                &shared.first_error, memory_order_acquire) != SALTS_OK) {
          status = atomic_load_explicit(
              &shared.first_error, memory_order_acquire);
          goto cleanup_threads;
        }
        salts_thread_yield();
      }
    }
  }

  while (atomic_load_explicit(
             &shared.ready, memory_order_acquire) !=
         WS_BENCH_CONNECTIONS) {
    ws_bench_sample_pressure(impl, &pressure, owner_count);
    if (atomic_load_explicit(
            &shared.first_error, memory_order_acquire) != SALTS_OK) {
      status = atomic_load_explicit(
          &shared.first_error, memory_order_acquire);
      goto cleanup_threads;
    }
    salts_thread_yield();
  }
  ws_bench_sample_pressure(impl, &pressure, owner_count);

  pressure.admission_handoffs = 0u;
  for (index = 1u; index < owner_count; ++index)
    pressure.admission_handoffs += pressure.owner_leases[index];

  cpu_started = clock();
  started_ns = salts_hrtime();
  atomic_store_explicit(&shared.start, true, memory_order_release);

  if (test_case->copied_command) {
    for (size_t round = 0u; round < test_case->rounds; ++round) {
      status = ws_bench_send_copy_round(
          &shared, &pressure, impl, owner_count, round);
      if (status != SALTS_OK) goto cleanup_threads;
    }
  }

  while (atomic_load_explicit(
             &shared.done, memory_order_acquire) !=
         WS_BENCH_CONNECTIONS) {
    ws_bench_sample_pressure(impl, &pressure, owner_count);
    if (atomic_load_explicit(
            &shared.first_error, memory_order_acquire) != SALTS_OK) {
      status = atomic_load_explicit(
          &shared.first_error, memory_order_acquire);
      goto cleanup_threads;
    }
    salts_thread_yield();
  }

  wall_ns = salts_hrtime() - started_ns;
  cpu_elapsed = clock() - cpu_started;
  status = chttp_server_get_stats(&server, &stats);
  if (status != SALTS_OK) goto cleanup_threads;
  if (stats.accepted_connections != WS_BENCH_CONNECTIONS ||
      stats.rejected_connections != 0u) {
    status = SALTS_EPROTO;
    goto cleanup_threads;
  }
  {
    size_t lease_sum = 0u;
    size_t lease_min = SIZE_MAX;
    size_t lease_max = 0u;
    for (index = 0u; index < owner_count; ++index) {
      const size_t value = pressure.owner_leases[index];
      lease_sum += value;
      if (value < lease_min) lease_min = value;
      if (value > lease_max) lease_max = value;
    }
    if (lease_sum != WS_BENCH_CONNECTIONS ||
        lease_max > lease_min + 1u) {
      status = SALTS_EPROTO;
      goto cleanup_threads;
    }
  }

  printf(
      "{\"kind\":\"measurement\","
      "\"benchmark\":\"chttp_server_ws_owner_scaling\","
      "\"workload\":\"%s\","
      "\"transport\":\"%s\","
      "\"send_path\":\"%s\","
      "\"latency_kind\":\"%s\","
      "\"payload_bytes\":%zu,"
      "\"owners\":%zu,"
      "\"connections\":%u,"
      "\"messages_per_connection\":%zu,"
      "\"operations\":%zu,"
      "\"samples\":%zu,"
      "\"wall_ns\":%llu,"
      "\"messages_per_second\":%.3f,"
      "\"cpu_ns_per_message\":%.3f,"
      "\"p50_ns\":%llu,"
      "\"p95_ns\":%llu,"
      "\"p99_ns\":%llu,"
      "\"accepted_connections\":%llu,"
      "\"rejected_connections\":%llu,"
      "\"owner0_leases\":%zu,"
      "\"owner1_leases\":%zu,"
      "\"owner2_leases\":%zu,"
      "\"owner3_leases\":%zu,"
      "\"peak_owner0_commands\":%zu,"
      "\"peak_owner1_commands\":%zu,"
      "\"peak_owner2_commands\":%zu,"
      "\"peak_owner3_commands\":%zu,"
      "\"peak_owner0_ring\":%zu,"
      "\"peak_owner1_ring\":%zu,"
      "\"peak_owner2_ring\":%zu,"
      "\"peak_owner3_ring\":%zu,"
      "\"cross_owner_admission_handoffs\":%zu,"
      "\"errors\":0}\n",
      test_case->name,
      test_case->tls ? "wss" : "ws",
      test_case->copied_command ? "copied-command" : "callback-local",
      test_case->latency_kind,
      test_case->payload_bytes, owner_count,
      WS_BENCH_CONNECTIONS, test_case->rounds, total_ops, total_ops,
      (unsigned long long)wall_ns,
      wall_ns != 0u
          ? (double)total_ops * 1.0e9 / (double)wall_ns
          : 0.0,
      total_ops != 0u
          ? ((double)cpu_elapsed * 1.0e9 /
             (double)CLOCKS_PER_SEC) /
                (double)total_ops
          : 0.0,
      (unsigned long long)ws_bench_percentile(
          latencies, total_ops, 50u),
      (unsigned long long)ws_bench_percentile(
          latencies, total_ops, 95u),
      (unsigned long long)ws_bench_percentile(
          latencies, total_ops, 99u),
      (unsigned long long)stats.accepted_connections,
      (unsigned long long)stats.rejected_connections,
      pressure.owner_leases[0], pressure.owner_leases[1],
      pressure.owner_leases[2], pressure.owner_leases[3],
      pressure.peak_owner_commands[0],
      pressure.peak_owner_commands[1],
      pressure.peak_owner_commands[2],
      pressure.peak_owner_commands[3],
      pressure.peak_owner_rings[0],
      pressure.peak_owner_rings[1],
      pressure.peak_owner_rings[2],
      pressure.peak_owner_rings[3],
      pressure.admission_handoffs);
  fflush(stdout);
  result = 0;

cleanup_threads:
  atomic_store_explicit(
      &shared.start, true, memory_order_release);
  atomic_store_explicit(
      &shared.cleanup, true, memory_order_release);
  for (index = 0u; index < WS_BENCH_CONNECTIONS; ++index) {
    if (thread_started[index]) {
      (void)salts_thread_join(&threads[index]);
      salts_thread_destroy(&threads[index]);
      thread_started[index] = false;
    }
  }
  if (status == SALTS_OK)
    status = atomic_load_explicit(
        &shared.first_error, memory_order_acquire);
  if (result == 0 &&
      atomic_load_explicit(
          &shared.close_events, memory_order_acquire) !=
          WS_BENCH_CONNECTIONS) {
    status = SALTS_EPROTO;
    result = 1;
  }

cleanup:
  if (server.impl != NULL) {
    const int stop_status =
        chttp_server_stop(&server, WS_BENCH_TIMEOUT_MS);
    if (status == SALTS_OK && stop_status != SALTS_OK)
      status = stop_status;
    {
      const int destroy_status = chttp_server_destroy(&server);
      if (status == SALTS_OK && destroy_status != SALTS_OK)
        status = destroy_status;
    }
  }
  free(push_started_ns);
  free(latencies);
  if (result != 0 || status != SALTS_OK) {
    fprintf(stderr,
            "ws benchmark failed workload=%s owners=%zu status=%d\n",
            test_case->name, owner_count, status);
    return 1;
  }
  return 0;
}

int main(void) {
  static const ws_bench_case CASES[] = {
      {"ws-echo-64", false, false, WS_BENCH_SMALL_BYTES,
       WS_BENCH_SMALL_ROUNDS, "round-trip"},
      {"ws-echo-64k", false, false, WS_BENCH_LARGE_BYTES,
       WS_BENCH_LARGE_ROUNDS, "round-trip"},
      {"ws-copy-64", false, true, WS_BENCH_SMALL_BYTES,
       WS_BENCH_SMALL_ROUNDS, "server-push"},
      {"wss-echo-64", true, false, WS_BENCH_SMALL_BYTES,
       WS_BENCH_SMALL_ROUNDS, "round-trip"},
      {"wss-echo-64k", true, false, WS_BENCH_LARGE_BYTES,
       WS_BENCH_LARGE_ROUNDS, "round-trip"},
      {"wss-copy-64", true, true, WS_BENCH_SMALL_BYTES,
       WS_BENCH_SMALL_ROUNDS, "server-push"}};
  static const size_t OWNERS[] = {1u, 2u, 4u};
  const size_t small_rounds =
      ws_bench_env_count("CHTTP_WS_BENCH_SMALL_ROUNDS",
                         WS_BENCH_SMALL_ROUNDS);
  const size_t large_rounds =
      ws_bench_env_count("CHTTP_WS_BENCH_LARGE_ROUNDS",
                         WS_BENCH_LARGE_ROUNDS);
  char *cert_path = NULL;
  char *key_path = NULL;
  char *ca_path = NULL;
  size_t case_index;
  size_t owner_index;
  int result = 0;

  memset(WS_BENCH_SMALL_PAYLOAD, 's',
         sizeof(WS_BENCH_SMALL_PAYLOAD));
  memset(WS_BENCH_LARGE_PAYLOAD, 'l',
         sizeof(WS_BENCH_LARGE_PAYLOAD));

  cert_path = tt_make_temp_file("chttp-ws-bench-cert", ".pem");
  key_path = tt_make_temp_file("chttp-ws-bench-key", ".pem");
  ca_path = tt_make_temp_file("chttp-ws-bench-ca", ".pem");
  if (cert_path == NULL || key_path == NULL || ca_path == NULL ||
      tt_write_file(cert_path, CHTTP_TLS_TEST_CERTIFICATE,
                    sizeof(CHTTP_TLS_TEST_CERTIFICATE) - 1u) != 0 ||
      tt_write_file(key_path, CHTTP_TLS_TEST_KEY,
                    sizeof(CHTTP_TLS_TEST_KEY) - 1u) != 0 ||
      tt_write_file(ca_path, CHTTP_TLS_TEST_CA_CERTIFICATE,
                    sizeof(CHTTP_TLS_TEST_CA_CERTIFICATE) - 1u) != 0) {
    result = 2;
    goto cleanup;
  }

  printf(
      "{\"kind\":\"environment\","
      "\"benchmark\":\"chttp_server_ws_owner_scaling\","
      "\"commit\":\"%s\","
      "\"backend\":\"%s\","
      "\"connections\":%u,"
      "\"small_rounds\":%zu,"
      "\"large_rounds\":%zu,"
      "\"warmup_rounds\":%u,"
      "\"note\":\"WS/WSS handshakes occur before the timed window; copied-command latency starts before thread-safe queue admission\"}\n",
      getenv("GITHUB_SHA") != NULL
          ? getenv("GITHUB_SHA")
          : "unknown",
#if defined(_WIN32)
      "iocp",
#elif defined(__linux__)
      "epoll",
#else
      "kqueue",
#endif
      WS_BENCH_CONNECTIONS, small_rounds, large_rounds,
      WS_BENCH_WARMUP_ROUNDS);
  fflush(stdout);

  for (case_index = 0u;
       case_index < sizeof(CASES) / sizeof(CASES[0]);
       ++case_index) {
    ws_bench_case test_case = CASES[case_index];
    if (test_case.payload_bytes == WS_BENCH_SMALL_BYTES)
      test_case.rounds = small_rounds;
    else
      test_case.rounds = large_rounds;
    for (owner_index = 0u;
         owner_index < sizeof(OWNERS) / sizeof(OWNERS[0]);
         ++owner_index) {
      if (ws_bench_run(&test_case, OWNERS[owner_index],
                       cert_path, key_path, ca_path) != 0) {
        result = 3;
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
  return result;
}
