#include "chttp_server_runtime.h"
#include "chttp_tls_test_material.h"
#define TINYTEST_NO_MAIN 1
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
  OWNER_WS_CONNECTIONS = 8,
  OWNER_WS_TIMEOUT_MS = 10000,
  OWNER_WS_SMALL_BYTES = 64,
  OWNER_WS_16K_BYTES = 16 * 1024,
  OWNER_WS_32K_BYTES = 32 * 1024,
  OWNER_WS_LARGE_BYTES = 64 * 1024,
  OWNER_WS_SMALL_MESSAGES = 240,
  OWNER_WS_LARGE_MESSAGES = 24,
  OWNER_WS_WARMUP = 8
};

typedef enum owner_ws_mode {
  OWNER_WS_CALLBACK_ECHO = 1,
  OWNER_WS_CAPTURED_PUSH
} owner_ws_mode;

typedef struct owner_ws_case {
  const char *name;
  bool tls;
  owner_ws_mode mode;
  size_t payload_bytes;
} owner_ws_case;

typedef struct owner_ws_session_probe {
  chttp_server_websocket_session session;
  atomic_int captured;
} owner_ws_session_probe;

typedef struct owner_ws_server_state {
  owner_ws_mode mode;
  owner_ws_session_probe sessions[OWNER_WS_CONNECTIONS];
  atomic_int callback_errors;
  atomic_uint_fast64_t callback_echo_send_ns;
  atomic_uint_fast64_t callback_echo_send_calls;
  _Atomic(chttp_server_websocket_profile *) callback_echo_profile;
} owner_ws_server_state;

typedef struct owner_ws_barrier {
  atomic_int ready;
  atomic_int start;
  atomic_int done;
} owner_ws_barrier;

typedef struct owner_ws_worker {
  const owner_ws_case *test_case;
  size_t client_id;
  uint16_t port;
  const char *ca_path;
  owner_ws_session_probe *probe;
  owner_ws_barrier *barrier;
  size_t warmup;
  size_t messages;
  uint64_t *latencies;
  uint64_t *send_latencies;
  uint64_t *receive_latencies;
  atomic_int status;
} owner_ws_worker;

typedef struct owner_ws_pressure {
  size_t owner_leases[4];
  size_t peak_command_queue[4];
  size_t peak_admission_ring[4];
  size_t cross_owner_handoffs;
} owner_ws_pressure;

static unsigned char OWNER_WS_SMALL_PAYLOAD[OWNER_WS_SMALL_BYTES];
static unsigned char OWNER_WS_LARGE_PAYLOAD[OWNER_WS_LARGE_BYTES];

static int owner_ws_u64_compare(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t owner_ws_percentile(const uint64_t *values, size_t count,
                                    unsigned int percent) {
  uint64_t *ordered;
  size_t rank;
  uint64_t result;
  if (values == NULL || count == 0u) return 0u;
  ordered = (uint64_t *)malloc(count * sizeof(*ordered));
  if (ordered == NULL) return 0u;
  memcpy(ordered, values, count * sizeof(*ordered));
  qsort(ordered, count, sizeof(*ordered), owner_ws_u64_compare);
  rank = ((size_t)percent * count + 99u) / 100u;
  if (rank == 0u) rank = 1u;
  if (rank > count) rank = count;
  result = ordered[rank - 1u];
  free(ordered);
  return result;
}

static size_t owner_ws_env_count(const char *name, size_t fallback) {
  const char *value = getenv(name);
  char *end = NULL;
  unsigned long parsed;
  if (value == NULL || value[0] == '\0') return fallback;
  parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0ul) return fallback;
  return (size_t)parsed;
}

static native_io_backend_kind owner_ws_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config owner_ws_network(bool tls, size_t connections) {
  cnet_client_config config = {
      .backend = owner_ws_backend(),
      .connection_capacity = connections,
      .command_capacity = 128u,
      .request_capacity = 64u,
      .completion_batch_capacity = 64u,
      .event_capacity = 128u,
      .max_send_bytes = 256u * 1024u,
      .receive_buffer_bytes = 128u * 1024u,
      .connect_timeout_ms = OWNER_WS_TIMEOUT_MS,
      .read_timeout_ms = OWNER_WS_TIMEOUT_MS,
      .write_timeout_ms = OWNER_WS_TIMEOUT_MS};
  if (tls) {
    config.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    config.tls_handshake_timeout_ms = OWNER_WS_TIMEOUT_MS;
  }
  return config;
}

static chttp_server_config owner_ws_server_config(bool tls) {
  return (chttp_server_config){
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 64u,
      .network = owner_ws_network(tls, 32u),
      .route_capacity = 4u,
      .middleware_capacity = 1u,
      .max_route_middleware_count = 1u,
      .max_route_param_count = 2u,
      .max_route_param_bytes = 64u,
      .max_target_bytes = 128u,
      .max_header_count = 16u,
      .max_header_bytes = 2048u,
      .max_request_body_bytes = 1024u,
      .max_response_header_count = 16u,
      .max_response_header_bytes = 2048u,
      .max_response_body_bytes = 1024u,
      .session_capacity = 0u,
      .poll_slice_ms = 1u,
      .buffer_capacity_bytes = 64u * 1024u * 1024u};
}

static chttp_websocket_client_config owner_ws_client_config(bool tls) {
  chttp_websocket_client_config config = {
      .size = sizeof(config),
      .network = owner_ws_network(tls, 1u),
      .max_frame_bytes = OWNER_WS_LARGE_BYTES,
      .max_message_bytes = OWNER_WS_LARGE_BYTES,
      .max_buffered_input_bytes = OWNER_WS_LARGE_BYTES + 64u,
      .max_handshake_header_bytes = 4096u,
      .event_capacity = 16u};
  return config;
}

static int owner_ws_open(void *user, chttp_websocket *websocket,
                         const chttp_server_request_view *request,
                         chttp_server_response *response) {
  owner_ws_server_state *state = (owner_ws_server_state *)user;
  const char *id_text = chttp_server_request_param(request, "id");
  char *end = NULL;
  unsigned long id;
  int status;
  (void)response;
  if (state == NULL || id_text == NULL) return SALTS_EINVAL;
  id = strtoul(id_text, &end, 10);
  if (end == id_text || *end != '\0' || id >= OWNER_WS_CONNECTIONS)
    return SALTS_ERANGE;
  status = chttp_server_websocket_session_capture(
      websocket, &state->sessions[id].session);
  if (status == SALTS_OK)
    atomic_store_explicit(&state->sessions[id].captured, 1,
                          memory_order_release);
  return status;
}

static void owner_ws_event(void *user, chttp_websocket *websocket,
                           const chttp_websocket_event *event) {
  owner_ws_server_state *state = (owner_ws_server_state *)user;
  if (state == NULL || event == NULL || websocket == NULL) return;
  if (state->mode != OWNER_WS_CALLBACK_ECHO ||
      event->kind != CHTTP_WEBSOCKET_EVENT_MESSAGE)
    return;
  if (event->message_type != CHTTP_WEBSOCKET_MESSAGE_BINARY) {
    atomic_fetch_add_explicit(&state->callback_errors, 1,
                              memory_order_acq_rel);
  } else {
    chttp_server_websocket_profile *profile =
        atomic_load_explicit(&state->callback_echo_profile,
                             memory_order_acquire);
    const uint64_t started_ns = salts_hrtime();
    int status;
    if (profile != NULL) {
      status = chttp_server_websocket_profile_callback_send_begin(
          websocket, profile);
      if (status != SALTS_OK)
        atomic_fetch_add_explicit(&state->callback_errors, 1,
                                  memory_order_acq_rel);
    }
    status = chttp_websocket_send_binary(websocket, event->data, event->size);
    {
      const uint64_t elapsed_ns = salts_hrtime() - started_ns;
      atomic_fetch_add_explicit(&state->callback_echo_send_ns, elapsed_ns,
                                memory_order_relaxed);
      atomic_fetch_add_explicit(&state->callback_echo_send_calls, 1u,
                                memory_order_relaxed);
    }
    if (status != SALTS_OK)
      atomic_fetch_add_explicit(&state->callback_errors, 1,
                                memory_order_acq_rel);
  }
}

static int owner_ws_receive_expected(chttp_websocket_client *client,
                                     const unsigned char *payload,
                                     size_t payload_bytes) {
  chttp_websocket_event event = {0};
  int status = chttp_websocket_client_receive(
      client, OWNER_WS_TIMEOUT_MS, &event);
  if (status != SALTS_OK) return status;
  if (event.kind != CHTTP_WEBSOCKET_EVENT_MESSAGE ||
      event.message_type != CHTTP_WEBSOCKET_MESSAGE_BINARY ||
      event.size != payload_bytes || event.data == NULL ||
      memcmp(event.data, payload, payload_bytes) != 0)
    return SALTS_EPROTO;
  return SALTS_OK;
}

static int owner_ws_one_message(owner_ws_worker *worker,
                                chttp_websocket_client *client,
                                const unsigned char *payload,
                                size_t payload_bytes,
                                uint64_t *latency_out,
                                uint64_t *send_latency_out,
                                uint64_t *receive_latency_out) {
  uint64_t started;
  uint64_t stage_started;
  int status;
  if (worker == NULL || client == NULL || payload == NULL ||
      latency_out == NULL)
    return SALTS_EINVAL;
  started = salts_hrtime();
  stage_started = started;
  if (worker->test_case->mode == OWNER_WS_CALLBACK_ECHO)
    status = chttp_websocket_client_send_binary(
        client, payload, payload_bytes, OWNER_WS_TIMEOUT_MS);
  else
    status = chttp_server_websocket_send_binary(
        &worker->probe->session, payload, payload_bytes);
  if (send_latency_out != NULL)
    *send_latency_out = salts_hrtime() - stage_started;
  if (status != SALTS_OK) return status;
  stage_started = salts_hrtime();
  status = owner_ws_receive_expected(client, payload, payload_bytes);
  if (receive_latency_out != NULL)
    *receive_latency_out = salts_hrtime() - stage_started;
  if (status != SALTS_OK) return status;
  *latency_out = salts_hrtime() - started;
  return SALTS_OK;
}

static void owner_ws_worker_main(void *user) {
  static const char *H1_ALPN[] = {"http/1.1"};
  owner_ws_worker *worker = (owner_ws_worker *)user;
  chttp_websocket_client client = {0};
  chttp_tls_profile profile = {0};
  chttp_websocket_client_config config;
  cnet_tls_client_config tls_config = {0};
  chttp_websocket_connect_options options = {
      .size = sizeof(options),
      .timeout_ms = OWNER_WS_TIMEOUT_MS,
      .protocol = CHTTP_HTTP_1_1};
  const unsigned char *payload =
      worker->test_case->payload_bytes == OWNER_WS_SMALL_BYTES
          ? OWNER_WS_SMALL_PAYLOAD
          : OWNER_WS_LARGE_PAYLOAD;
  char uri[160];
  unsigned int http_status = 0u;
  uint64_t ignored = 0u;
  size_t index;
  bool announced = false;
  int status = SALTS_OK;

  atomic_store_explicit(&worker->status, SALTS_EIO, memory_order_release);
  config = owner_ws_client_config(worker->test_case->tls);
  if (worker->test_case->tls) {
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

  if (snprintf(uri, sizeof(uri), "%s://127.0.0.1:%u/bench/%zu",
               worker->test_case->tls ? "wss" : "ws",
               (unsigned int)worker->port, worker->client_id) <= 0) {
    status = SALTS_EIO;
    goto cleanup;
  }
  options.uri = uri;
  options.tls = worker->test_case->tls ? &profile : NULL;
  status = chttp_websocket_client_connect(&client, &options, &http_status);
  if (status != SALTS_OK || http_status != 101u) {
    if (status == SALTS_OK) status = SALTS_EPROTO;
    goto cleanup;
  }

  while (atomic_load_explicit(&worker->probe->captured,
                              memory_order_acquire) == 0)
    salts_thread_yield();

  for (index = 0u; index < worker->warmup; ++index) {
    status = owner_ws_one_message(
        worker, &client, payload, worker->test_case->payload_bytes, &ignored,
        NULL, NULL);
    if (status != SALTS_OK) goto cleanup;
  }

  atomic_store_explicit(&worker->status, SALTS_OK, memory_order_release);
  atomic_fetch_add_explicit(&worker->barrier->ready, 1, memory_order_acq_rel);
  announced = true;
  while (atomic_load_explicit(&worker->barrier->start,
                              memory_order_acquire) == 0)
    salts_thread_yield();

  for (index = 0u; index < worker->messages; ++index) {
    status = owner_ws_one_message(
        worker, &client, payload, worker->test_case->payload_bytes,
        &worker->latencies[index], &worker->send_latencies[index],
        &worker->receive_latencies[index]);
    if (status != SALTS_OK) {
      atomic_store_explicit(&worker->status, status, memory_order_release);
      goto cleanup;
    }
  }

cleanup:
  if (!announced)
    atomic_fetch_add_explicit(&worker->barrier->ready, 1,
                              memory_order_acq_rel);
  if (announced)
    atomic_fetch_add_explicit(&worker->barrier->done, 1,
                              memory_order_acq_rel);
  if (client.impl != NULL) {
    (void)chttp_websocket_client_close(
        &client, 1000u, NULL, 0u, OWNER_WS_TIMEOUT_MS);
    (void)chttp_websocket_client_destroy(&client, OWNER_WS_TIMEOUT_MS);
  }
  if (profile.impl != NULL) (void)chttp_tls_profile_destroy(&profile);
  if (status != SALTS_OK)
    atomic_store_explicit(&worker->status, status, memory_order_release);
}

static void owner_ws_sample_pressure(chttp_server_impl *impl,
                                     owner_ws_pressure *pressure,
                                     size_t owner_count,
                                     bool capture_leases) {
  size_t index;
  if (impl == NULL || pressure == NULL) return;
  for (index = 0u; index < owner_count && index < 4u; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(impl, index);
    size_t ring;
    size_t commands;
    if (owner == NULL) continue;
    if (capture_leases)
      pressure->owner_leases[index] = chttp_server_owner_lease_count(owner);
    salts_mutex_lock(&owner->admission_mutex);
    ring = owner->admission_count;
    salts_mutex_unlock(&owner->admission_mutex);
    salts_mutex_lock(&impl->mutex);
    commands = owner->websocket_command_count;
    salts_mutex_unlock(&impl->mutex);
    if (ring > pressure->peak_admission_ring[index])
      pressure->peak_admission_ring[index] = ring;
    if (commands > pressure->peak_command_queue[index])
      pressure->peak_command_queue[index] = commands;
  }
}

static void owner_ws_sample_command_pressure(chttp_server_impl *impl,
                                             owner_ws_pressure *pressure,
                                             size_t owner_count) {
  size_t index;
  if (impl == NULL || pressure == NULL) return;
  salts_mutex_lock(&impl->mutex);
  for (index = 0u; index < owner_count && index < 4u; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(impl, index);
    size_t commands;
    if (owner == NULL) continue;
    commands = owner->websocket_command_count;
    if (commands > pressure->peak_command_queue[index])
      pressure->peak_command_queue[index] = commands;
  }
  salts_mutex_unlock(&impl->mutex);
}

static int owner_ws_run(const owner_ws_case *test_case, size_t owner_count,
                        size_t messages, size_t warmup,
                        const char *cert_path, const char *key_path,
                        const char *ca_path) {
  static const char *H1_ALPN[] = {"http/1.1"};
  chttp_server server = {0};
  chttp_server_config config = owner_ws_server_config(test_case->tls);
  chttp_server_execution_options execution =
      (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
  cnet_tls_server_config tls_config = {0};
  chttp_server_websocket_options route = {
      .size = sizeof(route),
      .path = "/bench/:id",
      .max_frame_bytes = OWNER_WS_LARGE_BYTES,
      .max_message_bytes = OWNER_WS_LARGE_BYTES,
      .max_buffered_input_bytes = OWNER_WS_LARGE_BYTES + 64u,
      .on_open = owner_ws_open,
      .on_event = owner_ws_event};
  owner_ws_server_state state;
  owner_ws_barrier barrier;
  owner_ws_worker workers[OWNER_WS_CONNECTIONS];
  salts_thread_t threads[OWNER_WS_CONNECTIONS] = {0};
  bool started[OWNER_WS_CONNECTIONS] = {false};
  owner_ws_pressure pressure = {0};
  chttp_server_websocket_profile command_profile;
  chttp_server_stats stats = {0};
  chttp_server_impl *impl;
  uint64_t *latencies = NULL;
  uint64_t *send_latencies = NULL;
  uint64_t *receive_latencies = NULL;
  uint16_t port = 0u;
  uint64_t started_ns;
  uint64_t wall_ns;
  clock_t cpu_started;
  clock_t cpu_elapsed;
  size_t total_messages;
  size_t index;
  int result = 1;

  if (test_case == NULL || owner_count == 0u || owner_count > 4u)
    return 1;
  if (messages > SIZE_MAX / OWNER_WS_CONNECTIONS) return 1;
  total_messages = messages * OWNER_WS_CONNECTIONS;
  latencies = (uint64_t *)calloc(total_messages, sizeof(*latencies));
  send_latencies =
      (uint64_t *)calloc(total_messages, sizeof(*send_latencies));
  receive_latencies =
      (uint64_t *)calloc(total_messages, sizeof(*receive_latencies));
  if (latencies == NULL || send_latencies == NULL ||
      receive_latencies == NULL)
    goto cleanup;
  memset(&state, 0, sizeof(state));
  chttp_server_websocket_profile_reset(&command_profile);
  state.mode = test_case->mode;
  route.user = &state;
  for (index = 0u; index < OWNER_WS_CONNECTIONS; ++index)
    atomic_init(&state.sessions[index].captured, 0);
  atomic_init(&state.callback_errors, 0);
  atomic_init(&state.callback_echo_send_ns, 0u);
  atomic_init(&state.callback_echo_send_calls, 0u);
  atomic_init(&state.callback_echo_profile, NULL);
  atomic_init(&barrier.ready, 0);
  atomic_init(&barrier.start, 0);
  atomic_init(&barrier.done, 0);
  memset(workers, 0, sizeof(workers));

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

  if (chttp_server_init(&server, &config) != SALTS_OK) goto cleanup;
  execution.owner_count = owner_count;
  if (chttp_server_set_execution_options(&server, &execution) != SALTS_OK)
    goto cleanup;
  if (chttp_server_websocket_with(&server, &route) != SALTS_OK)
    goto cleanup;
  if (chttp_server_start(&server) != SALTS_OK) goto cleanup;
  if (chttp_server_port(&server, &port) != SALTS_OK || port == 0u)
    goto cleanup;
  impl = (chttp_server_impl *)server.impl;

  for (index = 0u; index < OWNER_WS_CONNECTIONS; ++index) {
    workers[index] = (owner_ws_worker){
        .test_case = test_case,
        .client_id = index,
        .port = port,
        .ca_path = ca_path,
        .probe = &state.sessions[index],
        .barrier = &barrier,
        .warmup = warmup,
        .messages = messages,
        .latencies = latencies + index * messages,
        .send_latencies = send_latencies + index * messages,
        .receive_latencies = receive_latencies + index * messages};
    atomic_init(&workers[index].status, SALTS_EIO);
    if (salts_thread_create(&threads[index], owner_ws_worker_main,
                            &workers[index]) != SALTS_OK)
      goto cleanup_threads;
    started[index] = true;
  }

  while (atomic_load_explicit(&barrier.ready, memory_order_acquire) !=
         OWNER_WS_CONNECTIONS) {
    owner_ws_sample_pressure(impl, &pressure, owner_count, true);
    salts_thread_yield();
  }
  owner_ws_sample_pressure(impl, &pressure, owner_count, true);
  for (index = 0u; index < OWNER_WS_CONNECTIONS; ++index)
    if (atomic_load_explicit(&workers[index].status,
                             memory_order_acquire) != SALTS_OK)
      goto cleanup_threads;

  pressure.cross_owner_handoffs = 0u;
  for (index = 1u; index < owner_count; ++index)
    pressure.cross_owner_handoffs += pressure.owner_leases[index];

  if (test_case->mode == OWNER_WS_CALLBACK_ECHO) {
    chttp_server_websocket_profile_reset(&command_profile);
    atomic_store_explicit(&state.callback_echo_send_ns, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&state.callback_echo_send_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&state.callback_echo_profile, &command_profile,
                          memory_order_release);
  }

  if (test_case->mode == OWNER_WS_CAPTURED_PUSH) {
    chttp_server_websocket_profile_reset(&command_profile);
    impl->websocket_profile = &command_profile;
  } else {
    impl->websocket_profile = NULL;
  }

  cpu_started = clock();
  started_ns = salts_hrtime();
  atomic_store_explicit(&barrier.start, 1, memory_order_release);
  while (atomic_load_explicit(&barrier.done, memory_order_acquire) !=
         OWNER_WS_CONNECTIONS) {
    owner_ws_sample_command_pressure(impl, &pressure, owner_count);
    salts_sleep_ms(1u);
  }
  wall_ns = salts_hrtime() - started_ns;
  cpu_elapsed = clock() - cpu_started;

  for (index = 0u; index < OWNER_WS_CONNECTIONS; ++index) {
    if (salts_thread_join(&threads[index]) != SALTS_OK)
      goto cleanup_threads;
    salts_thread_destroy(&threads[index]);
    started[index] = false;
    if (atomic_load_explicit(&workers[index].status,
                             memory_order_acquire) != SALTS_OK)
      goto cleanup_threads;
  }

  /*
   * Transport write completions are sampled evidence only. CNet/TLS may
   * coalesce or split WebSocket command bytes, so they are not a one-to-one
   * command completion contract and must not gate benchmark termination.
   */
  atomic_store_explicit(&state.callback_echo_profile, NULL,
                        memory_order_release);
  impl->websocket_profile = NULL;

  if (atomic_load_explicit(&state.callback_errors,
                           memory_order_acquire) != 0)
    goto cleanup;
  if (chttp_server_get_stats(&server, &stats) != SALTS_OK) goto cleanup;
  if (stats.accepted_connections != OWNER_WS_CONNECTIONS ||
      stats.rejected_connections != 0u)
    goto cleanup;

  printf(
      "{\"kind\":\"measurement\","
      "\"benchmark\":\"chttp_server_owner_ws_scaling\","
      "\"transport\":\"%s\","
      "\"mode\":\"%s\","
      "\"workload\":\"%s\","
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
      "\"client_send_p50_ns\":%llu,"
      "\"client_send_p95_ns\":%llu,"
      "\"client_receive_p50_ns\":%llu,"
      "\"client_receive_p95_ns\":%llu,"
      "\"server_callback_echo_send_ns_per_call\":%.3f,"
      "\"server_callback_echo_send_calls\":%llu,"
      "\"accepted_connections\":%llu,"
      "\"rejected_connections\":%llu,"
      "\"owner0_leases\":%zu,"
      "\"owner1_leases\":%zu,"
      "\"owner2_leases\":%zu,"
      "\"owner3_leases\":%zu,"
      "\"peak_owner0_command_queue\":%zu,"
      "\"peak_owner1_command_queue\":%zu,"
      "\"peak_owner2_command_queue\":%zu,"
      "\"peak_owner3_command_queue\":%zu,"
      "\"peak_owner0_admission_ring\":%zu,"
      "\"peak_owner1_admission_ring\":%zu,"
      "\"peak_owner2_admission_ring\":%zu,"
      "\"peak_owner3_admission_ring\":%zu,"
      "\"captured_command_admissions\":%zu,"
      "\"profile_commands\":%llu,"
      "\"profile_bytes\":%llu,"
      "\"profile_failed_commands\":%llu,"
      "\"profile_copy_ns_per_command\":%.3f,"
      "\"profile_enqueue_ns_per_command\":%.3f,"
      "\"profile_wake_ns_per_command\":%.3f,"
      "\"profile_queue_residence_ns_per_command\":%.3f,"
      "\"profile_send_admission_ns_per_command\":%.3f,"
      "\"profile_send_completion_samples\":%llu,"
      "\"profile_send_completion_ns_per_sample\":%.3f,"
      "\"profile_max_queue_residence_ns\":%llu,"
      "\"profile_max_send_admission_ns\":%llu,"
      "\"profile_max_send_completion_ns\":%llu,"
      "\"cross_owner_admission_handoffs\":%zu,"
      "\"cross_owner_data_plane_hops\":0,"
      "\"errors\":0}\n",
      test_case->tls ? "tls" : "tcp",
      test_case->mode == OWNER_WS_CALLBACK_ECHO ? "callback-echo"
                                                : "captured-push",
      test_case->name, test_case->payload_bytes, owner_count,
      OWNER_WS_CONNECTIONS, messages, total_messages, total_messages,
      (unsigned long long)wall_ns,
      wall_ns != 0u
          ? (double)total_messages * 1.0e9 / (double)wall_ns
          : 0.0,
      total_messages != 0u
          ? ((double)cpu_elapsed * 1.0e9 / (double)CLOCKS_PER_SEC) /
                (double)total_messages
          : 0.0,
      (unsigned long long)owner_ws_percentile(latencies, total_messages, 50u),
      (unsigned long long)owner_ws_percentile(latencies, total_messages, 95u),
      (unsigned long long)owner_ws_percentile(latencies, total_messages, 99u),
      (unsigned long long)owner_ws_percentile(send_latencies, total_messages, 50u),
      (unsigned long long)owner_ws_percentile(send_latencies, total_messages, 95u),
      (unsigned long long)owner_ws_percentile(receive_latencies, total_messages, 50u),
      (unsigned long long)owner_ws_percentile(receive_latencies, total_messages, 95u),
      atomic_load_explicit(
          &state.callback_echo_send_calls, memory_order_relaxed) != 0u
          ? (double)atomic_load_explicit(
                &state.callback_echo_send_ns, memory_order_relaxed) /
                (double)atomic_load_explicit(
                    &state.callback_echo_send_calls, memory_order_relaxed)
          : 0.0,
      (unsigned long long)atomic_load_explicit(
          &state.callback_echo_send_calls, memory_order_relaxed),
      (unsigned long long)stats.accepted_connections,
      (unsigned long long)stats.rejected_connections,
      pressure.owner_leases[0], pressure.owner_leases[1],
      pressure.owner_leases[2], pressure.owner_leases[3],
      pressure.peak_command_queue[0], pressure.peak_command_queue[1],
      pressure.peak_command_queue[2], pressure.peak_command_queue[3],
      pressure.peak_admission_ring[0], pressure.peak_admission_ring[1],
      pressure.peak_admission_ring[2], pressure.peak_admission_ring[3],
      test_case->mode == OWNER_WS_CAPTURED_PUSH ? total_messages : 0u,
      (unsigned long long)atomic_load_explicit(
          &command_profile.commands, memory_order_relaxed),
      (unsigned long long)atomic_load_explicit(
          &command_profile.bytes, memory_order_relaxed),
      (unsigned long long)atomic_load_explicit(
          &command_profile.failed_commands, memory_order_relaxed),
      atomic_load_explicit(&command_profile.commands, memory_order_relaxed) != 0u
          ? (double)atomic_load_explicit(
                &command_profile.copy_ns, memory_order_relaxed) /
                (double)atomic_load_explicit(
                    &command_profile.commands, memory_order_relaxed)
          : 0.0,
      atomic_load_explicit(&command_profile.commands, memory_order_relaxed) != 0u
          ? (double)atomic_load_explicit(
                &command_profile.enqueue_ns, memory_order_relaxed) /
                (double)atomic_load_explicit(
                    &command_profile.commands, memory_order_relaxed)
          : 0.0,
      atomic_load_explicit(&command_profile.commands, memory_order_relaxed) != 0u
          ? (double)atomic_load_explicit(
                &command_profile.wake_ns, memory_order_relaxed) /
                (double)atomic_load_explicit(
                    &command_profile.commands, memory_order_relaxed)
          : 0.0,
      atomic_load_explicit(&command_profile.commands, memory_order_relaxed) != 0u
          ? (double)atomic_load_explicit(
                &command_profile.queue_residence_ns, memory_order_relaxed) /
                (double)atomic_load_explicit(
                    &command_profile.commands, memory_order_relaxed)
          : 0.0,
      atomic_load_explicit(&command_profile.commands, memory_order_relaxed) != 0u
          ? (double)atomic_load_explicit(
                &command_profile.send_admission_ns, memory_order_relaxed) /
                (double)atomic_load_explicit(
                    &command_profile.commands, memory_order_relaxed)
          : 0.0,
      (unsigned long long)atomic_load_explicit(
          &command_profile.send_completion_samples, memory_order_relaxed),
      atomic_load_explicit(
          &command_profile.send_completion_samples, memory_order_relaxed) != 0u
          ? (double)atomic_load_explicit(
                &command_profile.send_completion_ns, memory_order_relaxed) /
                (double)atomic_load_explicit(
                    &command_profile.send_completion_samples, memory_order_relaxed)
          : 0.0,
      (unsigned long long)atomic_load_explicit(
          &command_profile.max_queue_residence_ns, memory_order_relaxed),
      (unsigned long long)atomic_load_explicit(
          &command_profile.max_send_admission_ns, memory_order_relaxed),
      (unsigned long long)atomic_load_explicit(
          &command_profile.max_send_completion_ns, memory_order_relaxed),
      pressure.cross_owner_handoffs);
  fflush(stdout);
  result = 0;

cleanup_threads:
  atomic_store_explicit(&barrier.start, 1, memory_order_release);
  for (index = 0u; index < OWNER_WS_CONNECTIONS; ++index) {
    if (started[index]) {
      (void)salts_thread_join(&threads[index]);
      salts_thread_destroy(&threads[index]);
      started[index] = false;
    }
  }

cleanup:
  if (server.impl != NULL) {
    (void)chttp_server_stop(&server, OWNER_WS_TIMEOUT_MS);
    (void)chttp_server_destroy(&server);
  }
  free(receive_latencies);
  free(send_latencies);
  free(latencies);
  return result;
}

int main(void) {
  static const owner_ws_case CASES[] = {
      {"ws-echo-16k", false, OWNER_WS_CALLBACK_ECHO, OWNER_WS_16K_BYTES},
      {"ws-push-16k", false, OWNER_WS_CAPTURED_PUSH, OWNER_WS_16K_BYTES},
      {"ws-echo-32k", false, OWNER_WS_CALLBACK_ECHO, OWNER_WS_32K_BYTES},
      {"ws-push-32k", false, OWNER_WS_CAPTURED_PUSH, OWNER_WS_32K_BYTES},
      {"ws-echo-64k", false, OWNER_WS_CALLBACK_ECHO, OWNER_WS_LARGE_BYTES},
      {"ws-push-64k", false, OWNER_WS_CAPTURED_PUSH, OWNER_WS_LARGE_BYTES},
      {"wss-echo-16k", true, OWNER_WS_CALLBACK_ECHO, OWNER_WS_16K_BYTES},
      {"wss-push-16k", true, OWNER_WS_CAPTURED_PUSH, OWNER_WS_16K_BYTES},
      {"wss-echo-32k", true, OWNER_WS_CALLBACK_ECHO, OWNER_WS_32K_BYTES},
      {"wss-push-32k", true, OWNER_WS_CAPTURED_PUSH, OWNER_WS_32K_BYTES},
      {"wss-echo-64k", true, OWNER_WS_CALLBACK_ECHO, OWNER_WS_LARGE_BYTES},
      {"wss-push-64k", true, OWNER_WS_CAPTURED_PUSH, OWNER_WS_LARGE_BYTES}};
  static const size_t OWNERS[] = {1u, 2u, 4u};
  const size_t small_messages =
      owner_ws_env_count("CHTTP_OWNER_WS_SMALL_MESSAGES",
                         OWNER_WS_SMALL_MESSAGES);
  const size_t large_messages =
      owner_ws_env_count("CHTTP_OWNER_WS_LARGE_MESSAGES",
                         OWNER_WS_LARGE_MESSAGES);
  const size_t warmup =
      owner_ws_env_count("CHTTP_OWNER_WS_WARMUP", OWNER_WS_WARMUP);
  const char *workload_filter = getenv("CHTTP_OWNER_WS_WORKLOAD");
  char *cert_path = NULL;
  char *key_path = NULL;
  char *ca_path = NULL;
  size_t case_index;
  size_t owner_index;
  int result = 0;

  memset(OWNER_WS_SMALL_PAYLOAD, 's', sizeof(OWNER_WS_SMALL_PAYLOAD));
  memset(OWNER_WS_LARGE_PAYLOAD, 'l', sizeof(OWNER_WS_LARGE_PAYLOAD));

  cert_path = tt_make_temp_file("chttp-owner-wss-cert", ".pem");
  key_path = tt_make_temp_file("chttp-owner-wss-key", ".pem");
  ca_path = tt_make_temp_file("chttp-owner-wss-ca", ".pem");
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
      "\"benchmark\":\"chttp_server_owner_ws_scaling\","
      "\"commit\":\"%s\","
      "\"backend\":\"%s\","
      "\"connections\":%u,"
      "\"small_messages\":%zu,"
      "\"large_messages\":%zu,"
      "\"warmup\":%zu,"
      "\"workload_filter\":\"%s\","
      "\"note\":\"post-fix 16/32/64-KiB WS/WSS matrix; connections and TLS handshakes complete before timing; captured-push measures bounded server session command admission into the fixed owner queue\"}\n",
      getenv("GITHUB_SHA") != NULL ? getenv("GITHUB_SHA") : "unknown",
#if defined(_WIN32)
      "iocp",
#elif defined(__linux__)
      "epoll",
#else
      "kqueue",
#endif
      OWNER_WS_CONNECTIONS, small_messages, large_messages, warmup,
      workload_filter != NULL && workload_filter[0] != '\0'
          ? workload_filter
          : "all");
  fflush(stdout);

  for (case_index = 0u;
       case_index < sizeof(CASES) / sizeof(CASES[0]); ++case_index) {
    const owner_ws_case *test_case = &CASES[case_index];
    if (workload_filter != NULL && workload_filter[0] != '\0' &&
        strcmp(workload_filter, test_case->name) != 0)
      continue;
    const size_t messages =
        test_case->payload_bytes == OWNER_WS_SMALL_BYTES
            ? small_messages
            : large_messages;
    for (owner_index = 0u;
         owner_index < sizeof(OWNERS) / sizeof(OWNERS[0]); ++owner_index) {
      if (owner_ws_run(test_case, OWNERS[owner_index], messages, warmup,
                       cert_path, key_path, ca_path) != 0) {
        fprintf(stderr,
                "WS owner benchmark failed workload=%s owners=%zu\n",
                test_case->name, OWNERS[owner_index]);
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
