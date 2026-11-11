/* Keep lifecycle and attempt assertions on the real production Owner code,
 * with the same source-unit method as chttp_pool_lifecycle_test. */
#include "../src/chttp_connection_recovery.c"
#include "../src/chttp_client.c"
#include "../src/chttp_requests.c"
#include <http_server/http.h>
#include <stdatomic.h>
#include "chttp_tls_test_material.h"
#include "tinytest.h"

enum { RECOVERY_TEST_TIMEOUT_MS = 6000, RECOVERY_TEST_WIRE_BYTES = 64 * 1024 };
static struct {
  chttp_async_client client;
  chttp_server server;
  chttp_client blocking;
  chttp_client_config client_config;
  chttp_server_config server_config;
  chttp_connection_recovery_options policy;
  chttp_tls_profile tls;
  cnet_tls_server_config server_tls;
  char *tls_files[3];
  size_t source_reads;
  char uri[128];
  atomic_int requests;
  size_t results;
  int status;
  int native_status;
  char stage[64];
} recovery_test;

static int recovery_test_reply(void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  (void)user; (void)request;
  atomic_fetch_add_explicit(&recovery_test.requests, 1, memory_order_relaxed);
  for (size_t index = 0u; index < request->header_count; ++index) {
    if (vstr_ieq(vstr_from_cstr(request->headers[index].name), vstr_from_cstr("X-Recovery")) &&
        (strcmp(request->headers[index].value, "owned") != 0 || request->body_size != 3u ||
         memcmp(request->body, "abc", 3u) != 0))
      return chttp_server_reply(response, 400u, "text/plain", "bad", 3u);
  }
  return chttp_server_reply(response, 200u, "text/plain", "ok", 2u);
}
static void recovery_test_complete(void *user, chttp_request request,
    const chttp_response_view *response, const chttp_error *error) {
  (void)user; (void)request;
  ++recovery_test.results;
  recovery_test.status = error != NULL ? error->status :
      response != NULL && response->status_code == 200u ? SALTS_OK : SALTS_EPROTO;
  recovery_test.native_status = error != NULL ? error->native_status : 0;
  snprintf(recovery_test.stage, sizeof(recovery_test.stage), "%s",
      error != NULL && error->stage != NULL ? error->stage : "");
}
static chttp_request_options recovery_test_request(chttp_protocol protocol) {
  return (chttp_request_options){.connection_uri = recovery_test.uri,
      .authority = "recovery.test", .target = "/ok", .method = CHTTP_METHOD_GET,
      .protocol = protocol, .on_complete = recovery_test_complete};
}
static void recovery_test_start(void) {
  uint16_t port = 0u;
  check_equal(chttp_server_init(&recovery_test.server, &recovery_test.server_config), SALTS_OK);
  check_equal(chttp_server_get(&recovery_test.server, "/ok", recovery_test_reply, NULL), SALTS_OK);
  check_equal(chttp_server_post(&recovery_test.server, "/ok", recovery_test_reply, NULL), SALTS_OK);
  check_equal(chttp_server_start(&recovery_test.server), SALTS_OK);
  check_equal(chttp_server_port(&recovery_test.server, &port), SALTS_OK);
  recovery_test.server_config.port = port;
  check_true(snprintf(recovery_test.uri, sizeof(recovery_test.uri),
                      "%s://127.0.0.1:%u", recovery_test.server_config.tls != NULL ? "tls" : "tcp",
                      (unsigned int)port) > 0);
}
static void recovery_test_peer_stop(void) {
  check_equal(chttp_server_stop(&recovery_test.server, RECOVERY_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_server_destroy(&recovery_test.server), SALTS_OK);
}
static void recovery_test_poll(void) {
  size_t completions = 0u;
  check_equal(chttp_async_client_poll(&recovery_test.client, 5u, &completions), SALTS_OK);
}
static void recovery_test_wait_result(size_t count) {
  const uint64_t deadline = monotonic_ms() + RECOVERY_TEST_TIMEOUT_MS;
  while (recovery_test.results < count && monotonic_ms() < deadline) recovery_test_poll();
  check_equal(recovery_test.results, count);
}
static chttp_connection_recovery *recovery_test_dial(chttp_protocol protocol) {
  chttp_client_impl *impl = chttp_client_get(&recovery_test.client);
  return protocol == CHTTP_HTTP_2 ? &impl->h2_sessions[0].recovery : &impl->h1_sessions[0].recovery;
}
static void recovery_test_first_failure(chttp_protocol protocol) {
  const uint64_t deadline = monotonic_ms() + RECOVERY_TEST_TIMEOUT_MS;
  chttp_connection_recovery *r = recovery_test_dial(protocol);
  cnet_managed_dial_snapshot snapshot;
  while (!r->terminal && monotonic_ms() < deadline) {
    recovery_test_poll();
    if (recovery_test.results != 0u)
      info("result status=%d native=%d stage=%s configured=%d last=%d permanent=%d requests=%d", recovery_test.status,
           recovery_test.native_status, recovery_test.stage,
           r->configured, r->last_status, r->permanent,
           atomic_load_explicit(&recovery_test.requests, memory_order_relaxed));
    check_equal(recovery_test.results, (size_t)0u);
  }
  check_true(r->terminal);
  check_equal(cnet_managed_dial_get_snapshot(&r->dial, &snapshot), SALTS_OK);
  check_equal(snapshot.recovery.attempts, (uint32_t)1u);
  check_equal(recovery_test.results, (size_t)0u);
}
static int recovery_test_source(void *user, void *data, size_t capacity, size_t *out_size) {
  (void)user;
  if (recovery_test.source_reads++ == 0u) {
    if (capacity < 3u) return SALTS_EMSGSIZE;
    memcpy(data, "abc", 3u); *out_size = 3u;
  } else *out_size = 0u;
  return SALTS_OK;
}
static void recovery_test_cold(chttp_protocol protocol) {
  chttp_request_options options = recovery_test_request(protocol);
  chttp_request request = {0};
  cnet_managed_dial_snapshot previous;
  chttp_connection_recovery *r = recovery_test_dial(protocol);
  char target[] = "/ok";
  char value[] = "owned";
  const chttp_header header = {"X-Recovery", value};
  const chttp_body_source source = {.read = recovery_test_source,
      .content_length_known = 1, .content_length = 3u};
  options.method = CHTTP_METHOD_POST;
  options.target = target;
  options.headers = &header; options.header_count = 1u;
  options.body_source = &source;
  options.tls = recovery_test.tls.impl != NULL ? &recovery_test.tls : NULL;
  recovery_test_peer_stop();
  check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client,
                                                        &recovery_test.policy), SALTS_OK);
  check_equal(chttp_async_client_submit(&recovery_test.client, &options, &request), SALTS_OK);
  memcpy(target, "/no", sizeof(target));
  memcpy(value, "other", sizeof(value));
  recovery_test_first_failure(protocol);
  check_equal(recovery_test.source_reads, (size_t)0u);
  check_equal(cnet_managed_dial_get_snapshot(&r->dial, &previous), SALTS_OK);
  recovery_test_start();
  recovery_test_wait_result(1u);
  check_equal(recovery_test.status, SALTS_OK);
  check_equal(atomic_load_explicit(&recovery_test.requests, memory_order_relaxed), 1);
  check_greater(recovery_test.source_reads, (size_t)0u);
  check_equal(cnet_managed_dial_protocol_ready(&r->dial, previous.recovery_ticket, monotonic_ms()),
              SALTS_ENOENT);
  check_true(r->ready);
}

static void recovery_test_tls(chttp_protocol protocol, bool wrong_name, bool wrong_alpn) {
  static const char *h1[] = {"http/1.1"};
  static const char *h2[] = {"h2"};
  cnet_tls_client_config tls;
  recovery_test_peer_stop();
  check_equal(chttp_async_client_stop(&recovery_test.client, RECOVERY_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_async_client_destroy(&recovery_test.client), SALTS_OK);
  recovery_test.tls_files[0] = tt_make_temp_file("recovery-cert", ".pem");
  recovery_test.tls_files[1] = tt_make_temp_file("recovery-key", ".pem");
  recovery_test.tls_files[2] = tt_make_temp_file("recovery-ca", ".pem");
  for (size_t i = 0u; i < 3u; ++i) check_not_null(recovery_test.tls_files[i]);
  check_equal(tt_write_file(recovery_test.tls_files[0], CHTTP_TLS_TEST_CERTIFICATE,
                            sizeof(CHTTP_TLS_TEST_CERTIFICATE) - 1u), 0);
  check_equal(tt_write_file(recovery_test.tls_files[1], CHTTP_TLS_TEST_KEY,
                            sizeof(CHTTP_TLS_TEST_KEY) - 1u), 0);
  check_equal(tt_write_file(recovery_test.tls_files[2], CHTTP_TLS_TEST_CA_CERTIFICATE,
                            sizeof(CHTTP_TLS_TEST_CA_CERTIFICATE) - 1u), 0);
  recovery_test.server_tls = (cnet_tls_server_config){.size = sizeof(recovery_test.server_tls),
      .cert_file = recovery_test.tls_files[0], .key_file = recovery_test.tls_files[1],
      .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
      .alpn_protocols = protocol == CHTTP_HTTP_2 && !wrong_alpn ? h2 : h1,
      .alpn_protocol_count = 1u};
  tls = (cnet_tls_client_config){.size = sizeof(tls), .ca_file = recovery_test.tls_files[2],
      .server_name = wrong_name ? "wrong.example" : "localhost",
      .alpn_protocols = protocol == CHTTP_HTTP_2 ? h2 : h1, .alpn_protocol_count = 1u};
  check_equal(chttp_tls_profile_init(&recovery_test.tls, &tls), SALTS_OK);
  recovery_test.server_config.tls = &recovery_test.server_tls;
  recovery_test.client_config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  recovery_test.client_config.network.tls_handshake_timeout_ms = RECOVERY_TEST_TIMEOUT_MS;
  recovery_test.server_config.network = recovery_test.client_config.network;
  recovery_test_start();
  check_equal(chttp_async_client_init(&recovery_test.client, &recovery_test.client_config), SALTS_OK);
}

static void recovery_test_security(bool alpn) {
  chttp_request_options options;
  chttp_request request = {0};
  chttp_server_stats stats;
  chttp_connection_recovery *r;
  const uint64_t deadline = monotonic_ms() + RECOVERY_TEST_TIMEOUT_MS;
  recovery_test_tls(CHTTP_HTTP_2, !alpn, alpn);
  options = recovery_test_request(CHTTP_HTTP_2);
  options.tls = &recovery_test.tls;
  check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client,
                                                        &recovery_test.policy), SALTS_OK);
  check_equal(chttp_async_client_submit(&recovery_test.client, &options, &request), SALTS_OK);
  recovery_test_wait_result(1u);
  check_not_equal(recovery_test.status, SALTS_OK);
  r = recovery_test_dial(CHTTP_HTTP_2);
  while (r->dial.impl != NULL && monotonic_ms() < deadline) recovery_test_poll();
  check_null(r->dial.impl);
  check_equal(chttp_server_get_stats(&recovery_test.server, &stats), SALTS_OK);
  check_equal(stats.accepted_connections, UINT64_C(1));
  check_equal(atomic_load_explicit(&recovery_test.requests, memory_order_relaxed), 0);
}

static void recovery_test_blocking_deadline(chttp_protocol protocol) {
  chttp_options options = {.connection_uri = recovery_test.uri, .authority = "recovery.test",
      .target = "/ok", .timeout_ms = 50u, .protocol = protocol};
  chttp_response response = {0};
  chttp_error error = {0};
  recovery_test_peer_stop();
  check_equal(chttp_client_init(&recovery_test.blocking, &recovery_test.client_config), SALTS_OK);
  check_equal(chttp_client_set_connection_recovery(&recovery_test.blocking,
                                                  &recovery_test.policy), SALTS_OK);
  check_equal(chttp_get(&recovery_test.blocking, &options, &response, &error), SALTS_ETIMEDOUT);
  chttp_response_destroy(&response);
  check_equal(atomic_load_explicit(&recovery_test.requests, memory_order_relaxed), 0);
  recovery_test_start();
  options.timeout_ms = RECOVERY_TEST_TIMEOUT_MS;
  check_equal(chttp_get(&recovery_test.blocking, &options, &response, &error), SALTS_OK);
  chttp_response_destroy(&response);
  check_equal(atomic_load_explicit(&recovery_test.requests, memory_order_relaxed), 1);
  check_equal(chttp_client_get(&chttp_client_get_impl(&recovery_test.blocking)->async)
                  ->recovery_options.max_attempts, (uint32_t)3u);
}

static void recovery_test_admission_deadline(chttp_protocol protocol, bool rpc) {
  chttp_request_options options = recovery_test_request(protocol);
  chttp_request request = {0};
  cnet_managed_dial_snapshot snapshot;
  uint64_t deadline;
  recovery_test_peer_stop();
  check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client,
                                                        &recovery_test.policy), SALTS_OK);
  deadline = monotonic_ms() + 150u;
  if (rpc) {
    options.method = CHTTP_METHOD_POST;
    check_equal(chttp_async_client_rpc_submit_transport(&recovery_test.client,
                &options, 7u, deadline, &request), SALTS_OK);
  } else {
    check_equal(chttp_async_client_submit_timed(&recovery_test.client, &options,
                NULL, NULL, NULL, deadline, &request), SALTS_OK);
  }
  check_equal(cnet_managed_dial_get_snapshot(&recovery_test_dial(protocol)->dial, &snapshot), SALTS_OK);
  check_equal(snapshot.recovery.deadline_ms, deadline);
  recovery_test_wait_result(1u);
  check_equal(recovery_test.status, SALTS_ETIMEDOUT);
  /* Fast refusal can exhaust the recovery budget before the RPC deadline:
   * its next backoff would exceed the same fixed absolute limit. */
  if (rpc) check(strcmp(recovery_test.stage, "rpc-deadline") == 0 ||
                 strcmp(recovery_test.stage, "connection-recovery") == 0);
}

static void recovery_test_warm(chttp_protocol protocol) {
  chttp_request_options options = recovery_test_request(protocol);
  chttp_request request = {0};
  chttp_connection_recovery *r = recovery_test_dial(protocol);
  cnet_connection previous;
  uint64_t deadline;
  check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client,
                                                        &recovery_test.policy), SALTS_OK);
  check_equal(chttp_async_client_submit(&recovery_test.client, &options, &request), SALTS_OK);
  recovery_test_wait_result(1u);
  check_equal(recovery_test.status, SALTS_OK);
  check_true(r->ready);
  previous = *r->connection;
  check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client,
                                                        &recovery_test.policy), SALTS_EBUSY);
  recovery_test_peer_stop();
  check_equal(chttp_async_client_submit(&recovery_test.client, &options, &request), SALTS_OK);
  recovery_test_wait_result(2u);
  check_not_equal(recovery_test.status, SALTS_OK);
  recovery_test_start();
  deadline = monotonic_ms() + RECOVERY_TEST_TIMEOUT_MS;
  while ((!r->ready || !r->pool_bound ||
          (r->connection->slot == previous.slot && r->connection->generation == previous.generation)) &&
         monotonic_ms() < deadline) recovery_test_poll();
  check_true(r->ready);
  check_true(r->pool_bound);
  check_equal(recovery_test.results, (size_t)2u);
  check_equal(atomic_load_explicit(&recovery_test.requests, memory_order_relaxed), 1);
  check_equal(chttp_async_client_submit(&recovery_test.client, &options, &request), SALTS_OK);
  recovery_test_wait_result(3u);
  check_equal(recovery_test.status, SALTS_OK);
  check_equal(atomic_load_explicit(&recovery_test.requests, memory_order_relaxed), 2);
}

static void recovery_test_cancel(chttp_protocol protocol, bool stop) {
  chttp_request_options options = recovery_test_request(protocol);
  chttp_request request = {0};
  chttp_connection_recovery *r = recovery_test_dial(protocol);
  const uint64_t deadline = monotonic_ms() + RECOVERY_TEST_TIMEOUT_MS;
  recovery_test_peer_stop();
  check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client,
                                                        &recovery_test.policy), SALTS_OK);
  check_equal(chttp_async_client_submit(&recovery_test.client, &options, &request), SALTS_OK);
  recovery_test_first_failure(protocol);
  if (stop) {
    check_equal(chttp_async_client_stop(&recovery_test.client, RECOVERY_TEST_TIMEOUT_MS), SALTS_OK);
  } else {
    check_equal(chttp_async_request_cancel(&recovery_test.client, request), SALTS_OK);
    while (r->dial.impl != NULL && monotonic_ms() < deadline) recovery_test_poll();
  }
  check_null(r->dial.impl);
  check_equal(recovery_test.results, (size_t)1u);
  check_not_equal(recovery_test.status, SALTS_OK);
  if (!stop) check_equal(recovery_test.status, SALTS_ECANCELED);
  check_equal(chttp_async_request_cancel(&recovery_test.client, request), SALTS_ENOENT);
}

spec("CHttp explicit connection recovery") {
  before_each() {
    memset(&recovery_test, 0, sizeof(recovery_test));
    atomic_init(&recovery_test.requests, 0);
    recovery_test.policy = (chttp_connection_recovery_options){
        .size = sizeof(recovery_test.policy), .max_attempts = 3u,
        .episode_timeout_ms = RECOVERY_TEST_TIMEOUT_MS,
        .initial_backoff_ms = 1000u, .maximum_backoff_ms = 1000u, .jitter_seed = 7u};
    recovery_test.client_config = (chttp_client_config){
        .network = {.backend =
#if defined(_WIN32)
            NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
            NATIVE_IO_BACKEND_EPOLL,
#else
            NATIVE_IO_BACKEND_KQUEUE,
#endif
            .connection_capacity = 1u, .command_capacity = 16u, .request_capacity = 8u,
            .completion_batch_capacity = 4u, .event_capacity = 16u,
            .max_send_bytes = RECOVERY_TEST_WIRE_BYTES, .receive_buffer_bytes = 4096u,
            .connect_timeout_ms = RECOVERY_TEST_TIMEOUT_MS,
            .read_timeout_ms = RECOVERY_TEST_TIMEOUT_MS, .write_timeout_ms = RECOVERY_TEST_TIMEOUT_MS},
        .request_capacity = 2u, .max_start_line_bytes = 256u, .max_header_count = 16u,
        .max_header_bytes = 4096u, .max_request_body_bytes = 4096u,
        .max_response_body_bytes = 4096u, .max_informational_responses = 4u};
    recovery_test.server_config = (chttp_server_config){.host = "127.0.0.1", .backlog = 4u,
        .network = recovery_test.client_config.network, .route_capacity = 2u,
        .max_target_bytes = 256u, .max_header_count = 16u, .max_header_bytes = 4096u,
        .max_request_body_bytes = 4096u, .max_response_header_count = 8u,
        .max_response_header_bytes = 4096u, .max_response_body_bytes = 4096u,
        .poll_slice_ms = 1u, .enable_http2 = 1, .h2_stream_capacity = 2u,
        .h2_input_buffer_bytes = RECOVERY_TEST_WIRE_BYTES,
        .h2_output_buffer_bytes = RECOVERY_TEST_WIRE_BYTES,
        .h2_hpack_dynamic_table_bytes = 4096u, .h2_max_settings_count = 16u};
    recovery_test_start();
    check_equal(chttp_async_client_init(&recovery_test.client, &recovery_test.client_config), SALTS_OK);
  }
  after_each() {
    if (recovery_test.blocking.impl != NULL)
      check_equal(chttp_client_destroy(&recovery_test.blocking, RECOVERY_TEST_TIMEOUT_MS), SALTS_OK);
    if (recovery_test.client.impl != NULL) {
      check_equal(chttp_async_client_stop(&recovery_test.client, RECOVERY_TEST_TIMEOUT_MS), SALTS_OK);
      check_equal(chttp_async_client_destroy(&recovery_test.client), SALTS_OK);
    }
    if (recovery_test.server.impl != NULL) recovery_test_peer_stop();
    check_equal(chttp_tls_profile_destroy(&recovery_test.tls), SALTS_OK);
    for (size_t index = 0u; index < 3u; ++index) {
      if (recovery_test.tls_files[index] != NULL) {
        check_equal(remove(recovery_test.tls_files[index]), 0);
        free(recovery_test.tls_files[index]);
      }
    }
  }

  it("validates and copies an explicit fresh-client policy") {
    chttp_connection_recovery_options options = CHTTP_CONNECTION_RECOVERY_OPTIONS_INIT;
    chttp_client_impl *impl = chttp_client_get(&recovery_test.client);
    check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client, NULL), SALTS_EINVAL);
    check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client, &options), SALTS_OK);
    --options.size;
    check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client, &options), SALTS_EINVAL);
    options = recovery_test.policy; options.max_attempts = 0u;
    check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client, &options), SALTS_EINVAL);
    options = recovery_test.policy; options.episode_timeout_ms = 0u;
    check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client, &options), SALTS_EINVAL);
    options = recovery_test.policy; options.initial_backoff_ms = options.maximum_backoff_ms + 1u;
    check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client, &options), SALTS_EINVAL);
    options = recovery_test.policy;
    check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client, &options), SALTS_OK);
    options.max_attempts = 99u;
    check_equal(impl->recovery_options.max_attempts, (uint32_t)3u);
  }

  it("restores a cold H1 connection before issuing the one HTTP request") {
    recovery_test_cold(CHTTP_HTTP_1_1);
  }
  it("restores a cold H2 connection before issuing the one HTTP request") {
    recovery_test_cold(CHTTP_HTTP_2);
  }
  it("restores H1 capability without replaying a failed request") {
    recovery_test_warm(CHTTP_HTTP_1_1);
  }
  it("restores H2 capability without replaying a failed request") {
    recovery_test_warm(CHTTP_HTTP_2);
  }
  it("cancels an H1 request during connection backoff") {
    recovery_test_cancel(CHTTP_HTTP_1_1, false);
  }
  it("cancels an H2 request during connection backoff") {
    recovery_test_cancel(CHTTP_HTTP_2, false);
  }
  it("seals H1 connection backoff before client stop") {
    recovery_test_cancel(CHTTP_HTTP_1_1, true);
  }
  it("seals H2 connection backoff before client stop") {
    recovery_test_cancel(CHTTP_HTTP_2, true);
  }
  it("keeps a single episode deadline while waiting for backoff") {
    chttp_request_options options = recovery_test_request(CHTTP_HTTP_1_1);
    chttp_request request = {0};
    chttp_connection_recovery *r = recovery_test_dial(CHTTP_HTTP_1_1);
    cnet_managed_dial_snapshot snapshot;
    uint64_t episode_deadline;
    const uint64_t guard = monotonic_ms() + RECOVERY_TEST_TIMEOUT_MS;
    recovery_test_peer_stop();
    recovery_test.policy.episode_timeout_ms = 150u;
    check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client,
                                                          &recovery_test.policy), SALTS_OK);
    check_equal(chttp_async_client_submit(&recovery_test.client, &options, &request), SALTS_OK);
    check_equal(cnet_managed_dial_get_snapshot(&r->dial, &snapshot), SALTS_OK);
    episode_deadline = snapshot.recovery.deadline_ms;
    while (recovery_test.results == 0u && monotonic_ms() < guard) {
      if (r->dial.impl != NULL) {
        check_equal(cnet_managed_dial_get_snapshot(&r->dial, &snapshot), SALTS_OK);
        check_equal(snapshot.recovery.deadline_ms, episode_deadline);
      }
      recovery_test_poll();
    }
    check_equal(recovery_test.results, (size_t)1u);
    check_equal(recovery_test.status, SALTS_ETIMEDOUT);
  }
  it("completes the request when the next backoff cannot fit the episode") {
    chttp_request_options options = recovery_test_request(CHTTP_HTTP_1_1);
    chttp_request request = {0};
    recovery_test_peer_stop();
    recovery_test.policy.episode_timeout_ms = 4000u;
    recovery_test.policy.initial_backoff_ms = 10000u;
    recovery_test.policy.maximum_backoff_ms = 10000u;
    check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client,
                                                          &recovery_test.policy), SALTS_OK);
    check_equal(chttp_async_client_submit(&recovery_test.client, &options, &request), SALTS_OK);
    recovery_test_wait_result(1u);
    check_equal(recovery_test.status, SALTS_ETIMEDOUT);
    check_equal(atomic_load_explicit(&recovery_test.requests, memory_order_relaxed), 0);
  }
  it("retains the blocking policy across internal timeout reinitialization") {
    chttp_blocking_client_impl *impl;
    chttp_options options = {.connection_uri = recovery_test.uri, .authority = "recovery.test",
        .target = "/ok", .timeout_ms = RECOVERY_TEST_TIMEOUT_MS};
    chttp_response response = {0};
    chttp_error error = {0};
    check_equal(chttp_client_init(&recovery_test.blocking, &recovery_test.client_config), SALTS_OK);
    check_equal(chttp_client_set_connection_recovery(&recovery_test.blocking,
                                                    &recovery_test.policy), SALTS_OK);
    check_equal(chttp_get(&recovery_test.blocking, &options, &response, &error), SALTS_OK);
    chttp_response_destroy(&response);
    impl = chttp_client_get_impl(&recovery_test.blocking);
    check_equal(chttp_requests_recover(impl, RECOVERY_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_client_get(&impl->async)->recovery_options.max_attempts, (uint32_t)3u);
    check_equal(chttp_client_set_connection_recovery(&recovery_test.blocking,
                                                    &recovery_test.policy), SALTS_EBUSY);
    check_equal(chttp_get(&recovery_test.blocking, &options, &response, &error), SALTS_OK);
    chttp_response_destroy(&response);
  }
  it("restores HTTPS H1 before reading the forward-only request body") {
    recovery_test_tls(CHTTP_HTTP_1_1, false, false);
    recovery_test_cold(CHTTP_HTTP_1_1);
  }
  it("restores HTTPS H2 before reading the forward-only request body") {
    recovery_test_tls(CHTTP_HTTP_2, false, false);
    recovery_test_cold(CHTTP_HTTP_2);
  }
  it("treats TLS identity rejection as permanent without HTTP replay") {
    recovery_test_security(false);
  }
  it("treats TLS ALPN rejection as permanent without protocol downgrade") {
    recovery_test_security(true);
  }
  it("fails closed for unknown native errors and TLS diagnoses") {
    chttp_connection_recovery r = {0};
    cnet_error error = {.status = SALTS_ECONNRESET,
                        .tls = {.reason = CRYPTO_TLS_FAILURE_IDENTITY}};
    check_equal(recovery_classify(&r, CNET_CONNECTION_FAILED, &error), CNET_RECONNECT_SECURITY);
    error = (cnet_error){.status = SALTS_EIO, .native_status = INT_MIN};
    check_equal(recovery_classify(&r, CNET_CONNECTION_FAILED, &error), CNET_RECONNECT_PERMANENT);
    error = (cnet_error){.status = SALTS_EAUTH};
    check_equal(recovery_classify(&r, CNET_CONNECTION_FAILED, &error), CNET_RECONNECT_SECURITY);
    r.tls = true;
    error = (cnet_error){.status = SALTS_ECONNRESET};
    check_equal(recovery_classify(&r, CNET_CONNECTION_FAILED, &error), CNET_RECONNECT_PERMANENT);
    r.transport_connected = true;
    check_equal(recovery_classify(&r, CNET_CONNECTION_FAILED, &error), CNET_RECONNECT_TRANSIENT);
  }
  it("keeps H1's blocking result deadline inside the larger recovery episode") {
    recovery_test_blocking_deadline(CHTTP_HTTP_1_1);
  }
  it("keeps H2's blocking result deadline inside the larger recovery episode") {
    recovery_test_blocking_deadline(CHTTP_HTTP_2);
  }
  it("keeps the one-attempt default without creating a recovery dial") {
    const chttp_connection_recovery_options policy = CHTTP_CONNECTION_RECOVERY_OPTIONS_INIT;
    chttp_request_options options = recovery_test_request(CHTTP_HTTP_1_1);
    chttp_request request = {0};
    recovery_test_peer_stop();
    check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client, &policy), SALTS_OK);
    check_equal(chttp_async_client_submit(&recovery_test.client, &options, &request), SALTS_OK);
    recovery_test_wait_result(1u);
    check_not_equal(recovery_test.status, SALTS_OK);
    check_false(recovery_test_dial(CHTTP_HTTP_1_1)->configured);
    check_equal(atomic_load_explicit(&recovery_test.requests, memory_order_relaxed), 0);
  }
  it("ends a bounded opening episode when the physical attempt count is exhausted") {
    chttp_request_options options = recovery_test_request(CHTTP_HTTP_1_1);
    chttp_request request = {0};
    chttp_connection_recovery *r = recovery_test_dial(CHTTP_HTTP_1_1);
    cnet_managed_dial_snapshot snapshot;
    uint32_t attempts = 0u;
    const uint64_t deadline = monotonic_ms() + RECOVERY_TEST_TIMEOUT_MS;
    recovery_test_peer_stop();
    recovery_test.policy.max_attempts = 2u;
    check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client,
                                                          &recovery_test.policy), SALTS_OK);
    check_equal(chttp_async_client_submit(&recovery_test.client, &options, &request), SALTS_OK);
    while (recovery_test.results == 0u && monotonic_ms() < deadline) {
      if (r->dial.impl != NULL) {
        check_equal(cnet_managed_dial_get_snapshot(&r->dial, &snapshot), SALTS_OK);
        if (snapshot.recovery.attempts > attempts) attempts = snapshot.recovery.attempts;
      }
      recovery_test_poll();
    }
    check_equal(attempts, (uint32_t)2u);
    check_equal(recovery_test.results, (size_t)1u);
    check_not_equal(recovery_test.status, SALTS_OK);
    check_equal(atomic_load_explicit(&recovery_test.requests, memory_order_relaxed), 0);
  }
  it("keeps a pending H2 sibling alive when another opening request is canceled") {
    chttp_request_options options = recovery_test_request(CHTTP_HTTP_2);
    chttp_request first = {0}, sibling = {0};
    recovery_test_peer_stop();
    check_equal(chttp_async_client_set_connection_recovery(&recovery_test.client,
                                                          &recovery_test.policy), SALTS_OK);
    check_equal(chttp_async_client_submit(&recovery_test.client, &options, &first), SALTS_OK);
    check_equal(chttp_async_client_submit(&recovery_test.client, &options, &sibling), SALTS_OK);
    recovery_test_first_failure(CHTTP_HTTP_2);
    check_equal(chttp_async_request_cancel(&recovery_test.client, first), SALTS_OK);
    recovery_test_poll();
    check_equal(recovery_test.results, (size_t)1u);
    check_equal(recovery_test.status, SALTS_ECANCELED);
    recovery_test_start();
    recovery_test_wait_result(2u);
    check_equal(recovery_test.status, SALTS_OK);
    check_equal(atomic_load_explicit(&recovery_test.requests, memory_order_relaxed), 1);
  }
  it("clips the H1 opening episode to the original admission deadline") {
    recovery_test_admission_deadline(CHTTP_HTTP_1_1, false);
  }
  it("clips the H2 opening episode to the original admission deadline") {
    recovery_test_admission_deadline(CHTTP_HTTP_2, false);
  }
  it("carries the RPC transport deadline into the same managed opening episode") {
    recovery_test_admission_deadline(CHTTP_HTTP_2, true);
  }
}
