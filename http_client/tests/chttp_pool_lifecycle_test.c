/* Compile the production Owner unit to exercise its callback boundary. The
 * fixture uses real Manager/CNet transports and controls observer delivery or
 * external NativeIO completion progress; no test hook enters the library. */
#include "../src/chttp_client.c"
#include <http_server/http.h>
#include <salts/thread.h>
#include "tinytest.h"

enum { POOL_TEST_TIMEOUT_MS = 5000, POOL_TEST_WRITE_TIMEOUT_MS = 1000 };

static struct {
  chttp_async_client client;
  chttp_server server;
  chttp_server_config server_config;
  cnet_client_config network_config;
  native_io_backend backend;
  native_io_completion delayed_completion;
  chttp_h1_session *session;
  chttp_request request;
  cnet_connection send_connection;
  cnet_connection terminal_connection;
  cnet_connection_state terminal_state;
  cnet_error terminal_error;
  size_t send_bytes;
  size_t results;
  int result_status;
  bool connected;
  bool hold_send;
  bool hold_terminal;
  bool send_waiting;
  bool terminal_waiting;
  bool terminal_failed;
  bool external;
  bool completion_waiting;
  size_t source_bytes;
  char uri[64];
} fixture;

static int pool_test_reply(void *user, const chttp_server_request_view *request,
                           chttp_server_response *response) {
  (void)user;
  (void)request;
  return chttp_server_reply(response, 200u, "text/plain", "ok", 2u);
}

static void pool_test_complete(void *user, chttp_request request,
                              const chttp_response_view *response, const chttp_error *error) {
  (void)user;
  (void)request;
  ++fixture.results;
  fixture.result_status = error != NULL ? error->status
      : response != NULL && response->status_code == 200u ? SALTS_OK : SALTS_EPROTO;
}

static void pool_test_send(void *user, cnet_connection connection, size_t size) {
  if (!fixture.hold_send) {
    chttp_cnet_send(user, connection, size);
    return;
  }
  fixture.send_connection = connection;
  fixture.send_bytes = size;
  fixture.send_waiting = true;
}

static void pool_test_state(void *user, cnet_connection connection,
                            cnet_connection_state state, const cnet_error *error) {
  if (state == CNET_CONNECTION_CONNECTED) fixture.connected = true;
  if (fixture.hold_terminal &&
      (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED)) {
    fixture.terminal_connection = connection;
    fixture.terminal_state = state;
    fixture.terminal_failed = error != NULL;
    if (error != NULL) {
      fixture.terminal_error = *error;
      fixture.terminal_error.stage = "test-delayed-terminal";
    }
    fixture.terminal_waiting = true;
    return;
  }
  chttp_cnet_state(user, connection, state, error);
}

static void pool_test_deliver_send(void) {
  if (!fixture.send_waiting) return;
  fixture.send_waiting = false;
  chttp_cnet_send(fixture.session, fixture.send_connection, fixture.send_bytes);
}

static void pool_test_deliver_terminal(void) {
  if (!fixture.terminal_waiting) return;
  fixture.terminal_waiting = false;
  chttp_cnet_state(fixture.session, fixture.terminal_connection, fixture.terminal_state,
                  fixture.terminal_failed ? &fixture.terminal_error : NULL);
}

static chttp_request_options pool_test_options(void) {
  return (chttp_request_options){.connection_uri = fixture.uri, .authority = "pool.test",
      .target = "/ok", .method = CHTTP_METHOD_GET, .on_complete = pool_test_complete};
}

/* Observe only when requested: command admission and NativeIO submission can
 * advance without consuming the native completion that ends a payload borrow. */
static int pool_test_external_step(bool observe) {
  chttp_client_impl *impl = chttp_client_get(&fixture.client);
  native_io_completion completions[4];
  size_t events = 0u, count = 0u;
  int status = cnet_client_advance_external(&impl->network, &events);
  if (status != SALTS_OK) return status;
  if (observe) {
    status = native_io_backend_observe(&fixture.backend, completions, 4u, 1u, &count);
    if (status != SALTS_OK && status != SALTS_ETIMEDOUT) return status;
    for (size_t index = 0u; index < count; ++index) {
      bool consumed = false;
      status = cnet_client_route_external_completion(&impl->network, &completions[index],
                                                      &consumed, &events);
      if (status != SALTS_OK) return status;
      if (!consumed) return SALTS_EPROTO;
    }
  }
  return chttp_client_pool_progress(impl);
}

static int pool_test_deliver_native(void) {
  chttp_client_impl *impl = chttp_client_get(&fixture.client);
  size_t events = 0u;
  bool consumed = false;
  int status;
  if (!fixture.completion_waiting) return SALTS_OK;
  status = cnet_client_route_external_completion(&impl->network, &fixture.delayed_completion,
                                                 &consumed, &events);
  if (consumed) fixture.completion_waiting = false;
  return status != SALTS_OK ? status : consumed ? SALTS_OK : SALTS_EPROTO;
}

static int pool_test_source(void *user, void *buffer, size_t capacity, size_t *out_size) {
  const size_t total = 128u;
  size_t size = total - fixture.source_bytes;
  (void)user;
  if (size > capacity) size = capacity;
  memset(buffer, 'x', size);
  fixture.source_bytes += size;
  *out_size = size;
  return SALTS_OK;
}

static void pool_test_inflight_send(bool expire) {
  chttp_client_impl *impl = chttp_client_get(&fixture.client);
  const native_io_backend_config backend_config = {
      fixture.network_config.backend, 2u * fixture.network_config.connection_capacity,
      fixture.network_config.request_capacity, fixture.network_config.completion_batch_capacity};
  const chttp_body_source source = {
      .read = pool_test_source, .content_length_known = 1, .content_length = 128u};
  chttp_request_options options = pool_test_options();
  chttp_request warm = {0}, upload = {0}, rejected = {0}, next = {0};
  cnet_pool_snapshot pool;
  native_io_backend_stats native_stats;
  native_io_request body_send = {0};
  chttp_slot *slot;
  uint64_t deadline;
  uint64_t submissions_at_failure;
  bool native_send_live = false;
  if (expire) {
    fixture.network_config.write_timeout_ms = POOL_TEST_WRITE_TIMEOUT_MS;
    fixture.network_config.read_timeout_ms = 0u;
  }
  fixture.hold_send = false;
  pool_test_deliver_send();
  check_equal(cnet_client_stop(&impl->network, POOL_TEST_TIMEOUT_MS), SALTS_OK);
  check_equal(chttp_client_pool_progress(impl), SALTS_OK);
  check_equal(cnet_pool_get_snapshot(&impl->connection_pool, &pool), SALTS_OK);
  check_true(pool.drained);
  check_equal(cnet_client_destroy(&impl->network), SALTS_OK);
  check_equal(native_io_backend_init(&fixture.backend, &backend_config), SALTS_OK);
  check_equal(cnet_client_init_external(&impl->network, &fixture.network_config,
                                        &fixture.backend), SALTS_OK);
  fixture.external = true;
  /* No live attachment survives switching the test's progress owner. */
  check_equal(chttp_async_client_submit(&fixture.client, &options, &warm), SALTS_OK);
  deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
  while (fixture.results < 2u && cmeta_monotonic_ms() < deadline)
    check_equal(pool_test_external_step(true), SALTS_OK);
  check_equal(fixture.results, (size_t)2u);
  check_equal(fixture.result_status, SALTS_OK);
  check_true(fixture.session->pool_ready);
  options.body_source = &source;
  check_equal(chttp_async_client_submit(&fixture.client, &options, &upload), SALTS_OK);
  slot = chttp_slot_find(impl, upload);
  check_not_null(slot);
  deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
  do {
    cnet_external_request_snapshot requests[4];
    size_t count = 0u;
    check_equal(pool_test_external_step(false), SALTS_OK);
    check_equal(cnet_client_external_request_snapshots(&impl->network, slot->h1->connection,
                                                       requests, 4u, &count), SALTS_OK);
    for (size_t index = 0u; index < count; ++index)
      if (requests[index].operation_kind == NATIVE_IO_OPERATION_STREAM_SEND &&
          mem_buffer_ref_count(slot->source_retained) > 1u) {
        native_send_live = true;
        body_send = requests[index].request;
      }
    if (!native_send_live) check_equal(pool_test_external_step(true), SALTS_OK);
  } while (!native_send_live && cmeta_monotonic_ms() < deadline);
  check_true(native_send_live);
  check_greater(mem_buffer_ref_count(slot->source_retained), (uint32_t)1u);
  check_equal(fixture.source_bytes, (size_t)64u);
  check_true(native_io_backend_get_stats(&fixture.backend, &native_stats));
  check_greater(native_stats.active_requests, (size_t)0u);

  if (expire) {
    int expiry_status = SALTS_OK;
    /* Observe the real success, but retain it until after the owner deadline.
     * No synthetic byte count or completion is routed into CNet. */
    deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
    while (!fixture.completion_waiting && cmeta_monotonic_ms() < deadline) {
      native_io_completion completion;
      size_t count = 0u, events = 0u;
      bool consumed = false;
      int status = native_io_backend_observe(&fixture.backend, &completion, 1u, 1u, &count);
      check_true(status == SALTS_OK || status == SALTS_ETIMEDOUT);
      if (count == 0u) continue;
      if (completion.request.slot == body_send.slot &&
          completion.request.generation == body_send.generation) {
        fixture.delayed_completion = completion;
        fixture.completion_waiting = true;
      } else {
        check_equal(cnet_client_route_external_completion(&impl->network, &completion,
                                                           &consumed, &events), SALTS_OK);
        check_true(consumed);
      }
    }
    check_true(fixture.completion_waiting);
    check_equal(fixture.delayed_completion.kind, NATIVE_IO_COMPLETION_OK);
    check_equal(fixture.delayed_completion.bytes, (size_t)64u);
    /* Only the first body chunk reached the wire; HTTP still needs the suffix. */
    deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
    do {
      expiry_status = pool_test_external_step(false);
      if (expiry_status != SALTS_OK) break;
      cmeta_sleep_ms(1u);
    } while (cmeta_monotonic_ms() < deadline);
    /* NativeIO already retired the observed handle. Its cancellation returns
     * ENOENT; CNet retains the write-timeout result until this event is routed.
     * The exact SDK's cnet_owner_profile_test exercises the same contract. */
    check_equal(expiry_status, SALTS_ENOENT);
    check_equal(chttp_client_pool_progress(impl), SALTS_OK);
  } else {
    check_equal(chttp_async_request_cancel(&fixture.client, upload), SALTS_OK);
    check_equal(pool_test_external_step(false), SALTS_OK);
  }
  check_greater(mem_buffer_ref_count(slot->source_retained), (uint32_t)1u);
  check_equal(chttp_cnet_retained_release(&slot->source_retained), SALTS_EBUSY);
  if (!expire)
    check_equal(chttp_async_request_cancel(&fixture.client, upload), SALTS_EALREADY);
  check_equal(cnet_pool_get_snapshot(&impl->connection_pool, &pool), SALTS_OK);
  check_equal(pool.active_leases, (size_t)1u);
  check_equal(fixture.results, (size_t)2u);
  check_equal(chttp_async_client_submit(&fixture.client, &options, &rejected), SALTS_ENOBUFS);

  check_true(native_io_backend_get_stats(&fixture.backend, &native_stats));
  submissions_at_failure = native_stats.submitted;
  check_equal(pool_test_deliver_native(), SALTS_OK);
  deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
  while (fixture.results < 3u && cmeta_monotonic_ms() < deadline)
    check_equal(pool_test_external_step(true), SALTS_OK);
  check_equal(fixture.results, (size_t)3u);
  check_equal(fixture.result_status, expire ? SALTS_ETIMEDOUT : SALTS_ECANCELED);
  check_equal(fixture.source_bytes, (size_t)64u);
  check_equal(chttp_async_request_cancel(&fixture.client, upload), SALTS_ENOENT);
  check_null(slot->source_retained);
  check_equal(cnet_pool_get_snapshot(&impl->connection_pool, &pool), SALTS_OK);
  check_true(pool.drained);
  check_true(native_io_backend_get_stats(&fixture.backend, &native_stats));
  check_equal(native_stats.active_requests, (size_t)0u);
  check_equal(native_stats.submitted, submissions_at_failure);
  options.body_source = NULL;
  check_equal(chttp_async_client_submit(&fixture.client, &options, &next), SALTS_OK);
  deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
  while (fixture.results < 4u && cmeta_monotonic_ms() < deadline)
    check_equal(pool_test_external_step(true), SALTS_OK);
  check_equal(fixture.results, (size_t)4u);
  check_equal(fixture.result_status, SALTS_OK);
}

spec("CHttp Pool callback and context lifecycle") {
  before_each() {
    chttp_client_config config = {
        .network = {.backend =
#if defined(_WIN32)
          NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
          NATIVE_IO_BACKEND_EPOLL,
#else
          NATIVE_IO_BACKEND_KQUEUE,
#endif
          .connection_capacity = 1u, .command_capacity = 8u, .request_capacity = 4u,
          .completion_batch_capacity = 4u, .event_capacity = 8u, .max_send_bytes = 64u * 1024u,
          .receive_buffer_bytes = 256u, .connect_timeout_ms = POOL_TEST_TIMEOUT_MS,
          .read_timeout_ms = POOL_TEST_TIMEOUT_MS, .write_timeout_ms = POOL_TEST_TIMEOUT_MS},
        .request_capacity = 1u, .max_start_line_bytes = 256u, .max_header_count = 16u,
        .max_header_bytes = 512u, .max_request_body_bytes = 128u,
        .max_response_body_bytes = 128u, .max_informational_responses = 4u,
        .stream_chunk_bytes = 64u};
    chttp_server_config server_config = {
        .host = "127.0.0.1", .backlog = 4u, .network = config.network,
        .route_capacity = 1u, .max_target_bytes = 128u, .max_header_count = 16u,
        .max_header_bytes = 512u, .max_request_body_bytes = 128u,
        .max_response_header_count = 8u, .max_response_header_bytes = 512u,
        .max_response_body_bytes = 128u, .poll_slice_ms = 1u,
        .enable_http2 = 1, .h2_stream_capacity = 2u, .h2_input_buffer_bytes = 64u * 1024u,
        .h2_output_buffer_bytes = 64u * 1024u, .h2_hpack_dynamic_table_bytes = 2048u,
        .h2_max_settings_count = 16u};
    chttp_client_impl *impl;
    chttp_request_options options;
    cnet_connect_options connect_options;
    cnet_manager_attachment attachment = {
        .observer = {.on_state = pool_test_state, .on_receive = chttp_cnet_receive,
                     .on_send = pool_test_send},
        .on_recycle = chttp_h1_recycle, .hold_context = true};
    uint16_t port = 0u;
    uint64_t deadline;
    size_t completions = 0u;
    int chars;
    memset(&fixture, 0, sizeof(fixture));
    fixture.hold_send = true;
    server_config.network.connection_capacity = 2u;
    fixture.server_config = server_config;
    fixture.network_config = config.network;
    check_equal(chttp_server_init(&fixture.server, &server_config), SALTS_OK);
    check_equal(chttp_server_get(&fixture.server, "/ok", pool_test_reply, NULL), SALTS_OK);
    check_equal(chttp_server_start(&fixture.server), SALTS_OK);
    check_equal(chttp_server_port(&fixture.server, &port), SALTS_OK);
    chars = snprintf(fixture.uri, sizeof(fixture.uri), "tcp://127.0.0.1:%u", (unsigned int)port);
    check_true(chars > 0 && (size_t)chars < sizeof(fixture.uri));
    check_equal(chttp_async_client_init(&fixture.client, &config), SALTS_OK);
    impl = chttp_client_get(&fixture.client);
    fixture.session = &impl->h1_sessions[0];
    options = pool_test_options();
    check_equal(chttp_pool_key(impl, &options, NULL, NULL, &fixture.session->key), SALTS_OK);
    fixture.session->state = CHTTP_H1_CONNECTING;
    fixture.session->connection_uri = tstr_dup(fixture.uri);
    fixture.session->authority = tstr_dup(options.authority);
    check_not_null(fixture.session->connection_uri);
    check_not_null(fixture.session->authority);
    attachment.observer.user = fixture.session;
    connect_options = (cnet_connect_options){.uri = fixture.uri};
    check_equal(cnet_pool_reserve_connecting(&impl->connection_pool, &fixture.session->key,
                                            &fixture.session->pooled), SALTS_OK);
    check_equal(cnet_manager_reserve(&impl->transport_manager, &attachment,
                                     &fixture.session->managed), SALTS_OK);
    check_equal(cnet_manager_connect(&impl->transport_manager, fixture.session->managed,
                                     &connect_options, &fixture.session->connection), SALTS_OK);
    deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
    while (!fixture.connected && cmeta_monotonic_ms() < deadline)
      check_equal(chttp_async_client_poll(&fixture.client, 5u, &completions), SALTS_OK);
    check_true(fixture.connected);
    /* Seed a clean connected H1 session at the READY boundary. Admission and
     * every request below use the production Pool/Owner path. */
    fixture.session->state = CHTTP_H1_IDLE;
    check_equal(cnet_receive(&impl->network, fixture.session->connection, 1u), SALTS_OK);
    fixture.session->receive_armed = true;
    check_equal(cnet_pool_bind_ready(&impl->connection_pool, fixture.session->pooled,
                                     fixture.session->managed, 1u), SALTS_OK);
    fixture.session->pool_ready = true;
    check_equal(chttp_async_client_submit(&fixture.client, &options, &fixture.request), SALTS_OK);
    deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
    while ((fixture.results == 0u || !fixture.send_waiting) && cmeta_monotonic_ms() < deadline)
      check_equal(chttp_async_client_poll(&fixture.client, 5u, &completions), SALTS_OK);
    check_equal(fixture.results, (size_t)1u);
    check_equal(fixture.result_status, SALTS_OK);
    check_true(fixture.send_waiting);
  }

  after_each() {
    check_equal(pool_test_deliver_native(), SALTS_OK);
    fixture.hold_send = false;
    fixture.hold_terminal = false;
    pool_test_deliver_terminal();
    pool_test_deliver_send();
    if (fixture.client.impl != NULL) {
      if (chttp_client_get(&fixture.client)->network.impl == NULL) {
        /* External fixture initialization failed after the empty standalone
         * transport was destroyed; no transport cleanup remains. */
        chttp_client_get(&fixture.client)->stopped = true;
      } else if (fixture.external) {
        chttp_client_impl *impl = chttp_client_get(&fixture.client);
        const uint64_t deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
        cnet_pool_snapshot pool;
        cnet_manager_snapshot manager;
        int status;
        impl->admission_open = false;
        impl->stop_active = true;
        check_equal(cnet_pool_seal(&impl->connection_pool), SALTS_OK);
        if (fixture.session->connection.slot != 0u && !fixture.session->transport_closed) {
          status = cnet_close(&impl->network, fixture.session->connection);
          check_true(status == SALTS_OK || status == SALTS_EALREADY || status == SALTS_ENOENT);
        }
        do {
          check_equal(pool_test_external_step(true), SALTS_OK);
          status = cnet_client_stop_external(&impl->network);
        } while (status == SALTS_EBUSY && cmeta_monotonic_ms() < deadline);
        check_equal(status, SALTS_OK);
        check_equal(chttp_client_pool_progress(impl), SALTS_OK);
        check_equal(cnet_pool_get_snapshot(&impl->connection_pool, &pool), SALTS_OK);
        check_equal(cnet_manager_get_snapshot(&impl->transport_manager, &manager), SALTS_OK);
        check_true(pool.drained && manager.drained);
        /* This test drives the external owner through proven quiescence. The
         * public CHttp stop/poll API intentionally remains a standalone owner. */
        impl->stopped = true;
      } else {
        check_equal(chttp_async_client_stop(&fixture.client, POOL_TEST_TIMEOUT_MS), SALTS_OK);
      }
      check_equal(chttp_async_client_destroy(&fixture.client), SALTS_OK);
    }
    if (fixture.backend.impl != NULL) {
      check_equal(native_io_backend_close(&fixture.backend), SALTS_OK);
      check_equal(native_io_backend_destroy(&fixture.backend), SALTS_OK);
    }
    if (fixture.server.impl != NULL) {
      check_equal(chttp_server_stop(&fixture.server, POOL_TEST_TIMEOUT_MS), SALTS_OK);
      check_equal(chttp_server_destroy(&fixture.server), SALTS_OK);
    }
  }

  it("retains the logical slot and protocol lease until a late send notification") {
    chttp_client_impl *impl = chttp_client_get(&fixture.client);
    chttp_request_options options = pool_test_options();
    chttp_request rejected = {0};
    cnet_pool_snapshot snapshot;
    size_t completions = 0u;
    check_equal(cnet_pool_get_snapshot(&impl->connection_pool, &snapshot), SALTS_OK);
    check_equal(snapshot.active_leases, (size_t)1u);
    check_equal(chttp_async_request_cancel(&fixture.client, fixture.request), SALTS_EALREADY);
    check_equal(chttp_async_client_submit(&fixture.client, &options, &rejected), SALTS_ENOBUFS);
    check_equal(cnet_pool_terminal(&impl->connection_pool, fixture.session->pooled), SALTS_EBUSY);
    pool_test_deliver_send();
    check_equal(chttp_async_client_poll(&fixture.client, 0u, &completions), SALTS_OK);
    check_equal(chttp_async_request_cancel(&fixture.client, fixture.request), SALTS_ENOENT);
    check_equal(cnet_pool_get_snapshot(&impl->connection_pool, &snapshot), SALTS_OK);
    check_equal(snapshot.active_leases, (size_t)0u);
    check_equal(snapshot.ready, (size_t)1u);
    check_equal(fixture.results, (size_t)1u);
  }

  it("holds retired Manager context until the outstanding protocol lease is released") {
    chttp_client_impl *impl = chttp_client_get(&fixture.client);
    cnet_manager_snapshot manager;
    cnet_pool_snapshot pool;
    const uint64_t deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
    const cnet_connection old_connection = fixture.session->connection;
    chttp_request_options options = pool_test_options();
    chttp_request next = {0};
    size_t completions = 0u;
    fixture.hold_terminal = true;
    check_equal(cnet_close(&impl->network, old_connection), SALTS_OK);
    while (!fixture.terminal_waiting && cmeta_monotonic_ms() < deadline)
      check_equal(chttp_async_client_poll(&fixture.client, 5u, &completions), SALTS_OK);
    check_true(fixture.terminal_waiting);
    check_equal(cnet_manager_get_snapshot(&impl->transport_manager, &manager), SALTS_OK);
    check_equal(manager.retired, (size_t)1u);
    check_equal(manager.context_holds, (size_t)1u);
    check_equal(cnet_pool_get_snapshot(&impl->connection_pool, &pool), SALTS_OK);
    check_equal(pool.active_leases, (size_t)1u);
    check_equal(chttp_async_client_submit(&fixture.client, &options, &next), SALTS_ENOBUFS);
    pool_test_deliver_terminal();
    check_equal(chttp_async_client_poll(&fixture.client, 0u, &completions), SALTS_OK);
    check_equal(cnet_manager_get_snapshot(&impl->transport_manager, &manager), SALTS_OK);
    check_true(manager.drained);
    check_equal(cnet_pool_get_snapshot(&impl->connection_pool, &pool), SALTS_OK);
    check_true(pool.drained);
    check_equal(chttp_async_client_submit(&fixture.client, &options, &next), SALTS_OK);
    check_not_equal(next.generation, fixture.request.generation);
    /* The same storage now has a new physical generation. Old notifications
     * cannot close it, clear its pending operation or redeliver its result. */
    pool_test_deliver_send();
    chttp_cnet_state(fixture.session, old_connection, CNET_CONNECTION_CLOSED, NULL);
    check_equal(fixture.results, (size_t)1u);
    check_equal(fixture.session->state, CHTTP_H1_CONNECTING);
    while (fixture.results < 2u && cmeta_monotonic_ms() < deadline)
      check_equal(chttp_async_client_poll(&fixture.client, 5u, &completions), SALTS_OK);
    check_equal(fixture.results, (size_t)2u);
    check_equal(fixture.result_status, SALTS_OK);
  }

  it("recycles a failed H2 session before admitting another connection at capacity one") {
    chttp_client_impl *impl = chttp_client_get(&fixture.client);
    chttp_request_options options = pool_test_options();
    chttp_request requests[3] = {{0}};
    cnet_pool_snapshot pool;
    cnet_manager_snapshot manager;
    uint64_t deadline;
    uint16_t port = 0u;
    size_t completions = 0u;
    int chars;
    fixture.hold_send = false;
    pool_test_deliver_send();
    check_equal(cnet_close(&impl->network, fixture.session->connection), SALTS_OK);
    deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
    while (fixture.session->state != CHTTP_H1_FREE && cmeta_monotonic_ms() < deadline)
      check_equal(chttp_async_client_poll(&fixture.client, 5u, &completions), SALTS_OK);
    check_equal(fixture.session->state, CHTTP_H1_FREE);
    options.protocol = CHTTP_HTTP_2;
    check_equal(chttp_async_client_submit(&fixture.client, &options, &requests[0]), SALTS_OK);
    deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
    while (fixture.results < 2u && cmeta_monotonic_ms() < deadline)
      check_equal(chttp_async_client_poll(&fixture.client, 5u, &completions), SALTS_OK);
    check_equal(fixture.results, (size_t)2u);
    check_equal(fixture.result_status, SALTS_OK);
    check_true(impl->h2_sessions[0].pool_ready);
    check_equal(chttp_async_client_submit(&fixture.client, &options, &requests[1]), SALTS_OK);
    check_equal(cnet_pool_get_snapshot(&impl->connection_pool, &pool), SALTS_OK);
    check_equal(pool.active_leases, (size_t)1u);
    /* Stop the peer after local stream admission, before client progress can
     * deliver it. The stream and the session must both reach true terminal. */
    check_equal(chttp_server_stop(&fixture.server, POOL_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&fixture.server), SALTS_OK);
    deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
    do {
      check_equal(chttp_async_client_poll(&fixture.client, 5u, &completions), SALTS_OK);
      check_equal(cnet_pool_get_snapshot(&impl->connection_pool, &pool), SALTS_OK);
    } while (!pool.drained && cmeta_monotonic_ms() < deadline);
    check_true(pool.drained);
    check_equal(fixture.results, (size_t)3u);
    check_not_equal(fixture.result_status, SALTS_OK);
    check_equal(chttp_async_request_cancel(&fixture.client, requests[1]), SALTS_ENOENT);
    check_equal(cnet_manager_get_snapshot(&impl->transport_manager, &manager), SALTS_OK);
    check_true(manager.drained);
    check_equal(impl->h2_sessions[0].state, CHTTP_H2_SESSION_FREE);

    check_equal(chttp_server_init(&fixture.server, &fixture.server_config), SALTS_OK);
    check_equal(chttp_server_get(&fixture.server, "/ok", pool_test_reply, NULL), SALTS_OK);
    check_equal(chttp_server_start(&fixture.server), SALTS_OK);
    check_equal(chttp_server_port(&fixture.server, &port), SALTS_OK);
    chars = snprintf(fixture.uri, sizeof(fixture.uri), "tcp://127.0.0.1:%u", (unsigned int)port);
    check_true(chars > 0 && (size_t)chars < sizeof(fixture.uri));
    check_equal(chttp_async_client_submit(&fixture.client, &options, &requests[2]), SALTS_OK);
    check_not_equal(requests[1].generation, requests[2].generation);
    deadline = cmeta_monotonic_ms() + POOL_TEST_TIMEOUT_MS;
    while (fixture.results < 4u && cmeta_monotonic_ms() < deadline)
      check_equal(chttp_async_client_poll(&fixture.client, 5u, &completions), SALTS_OK);
    check_equal(fixture.results, (size_t)4u);
    check_equal(fixture.result_status, SALTS_OK);
  }

  it("keeps a retained payload and its lease alive through cancellation of a real NativeIO send") {
    pool_test_inflight_send(false);
  }

  it("keeps a write timeout terminal when the first body chunk completes late") {
    pool_test_inflight_send(true);
  }

  it("drains shutdown before a late send notification without redelivering the result") {
    check_equal(chttp_async_client_stop(&fixture.client, POOL_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_request_cancel(&fixture.client, fixture.request), SALTS_ENOENT);
    pool_test_deliver_send();
    check_equal(fixture.results, (size_t)1u);
    check_equal(chttp_async_client_stop(&fixture.client, POOL_TEST_TIMEOUT_MS), SALTS_OK);
  }
}
