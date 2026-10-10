#include "tinytest.h"
#include "chttp_server_runtime.h"

#include <stdatomic.h>
#include <salts/clock.h>
#include <salts/thread.h>

enum { TERMINAL_TEST_TIMEOUT_MS = 2000 };

typedef struct terminal_fixture_state {
  chttp_server_impl server;
  chttp_server_connection connection;
  chttp_server_response deferred_response;
  chttp_server_deferred handle;
  cmeta_thread_t thread;
  mem_buffer_t *body;
  atomic_int finished;
  int operation;
  int status;
  bool thread_started;
  bool locked;
} terminal_fixture_state;
static terminal_fixture_state terminal_fixture;

static void terminal_complete(void *user) {
  (void)user;
  const chttp_server_deferred_response reply = {
      .size = sizeof(reply), .status_code = 204u};
  if (terminal_fixture.operation == 0)
    terminal_fixture.status = chttp_server_deferred_reply(&terminal_fixture.handle, &reply);
  else if (terminal_fixture.operation == 1)
    terminal_fixture.status = chttp_server_deferred_reply_buffer(
        &terminal_fixture.handle, 200u, NULL, NULL, 0u, terminal_fixture.body);
  else
    terminal_fixture.status = chttp_server_deferred_cancel(&terminal_fixture.handle);
  atomic_store_explicit(&terminal_fixture.finished, 1, memory_order_release);
}

static int terminal_defer(void *user, const chttp_server_request_view *request,
                           chttp_server_response *response) {
  (void)user;
  (void)request;
  int status = chttp_server_response_defer(response, &terminal_fixture.handle);
  if (status != SALTS_OK) return status;
  if (terminal_fixture.operation == 1) {
    const chttp_server_deferred_response reply = {.size = sizeof(reply), .status_code = 204u};
    return chttp_server_deferred_reply(&terminal_fixture.handle, &reply);
  }
  if (terminal_fixture.operation == 2)
    return chttp_server_deferred_claim(&terminal_fixture.connection.deferred_token,
                                       terminal_fixture.handle.generation);
  return SALTS_OK;
}

static int terminal_interceptor(void *user, const chttp_server_request_view *request,
                                 chttp_server_response *response, chttp_server_next *next) {
  (void)user;
  (void)request;
  int status = chttp_server_response_set_header(response, "X-Sealed", "preserved");
  if (status == SALTS_OK) status = chttp_server_next_call(next);
  /* A post-handler mutation must fail without reopening the sealed response. */
  return status == SALTS_OK
      ? chttp_server_response_set_header(response, "X-After", "rejected") : status;
}

spec("CHTTP deferred generation token") {
  it("rejects a stale generation without changing a reused pending slot") {
    const uint32_t stale_generation = 7u;
    const uint32_t current_generation = stale_generation + 1u;
    const uint_fast64_t current_pending =
        chttp_server_deferred_token(current_generation, CHTTP_SERVER_DEFERRED_PENDING);
    atomic_uint_fast64_t token;

    atomic_init(&token, current_pending);
    check_equal(chttp_server_deferred_claim(&token, stale_generation), SALTS_ENOENT);
    check_equal(atomic_load_explicit(&token, memory_order_acquire), current_pending);
    check_equal(chttp_server_deferred_claim(&token, current_generation), SALTS_OK);
    check_equal(chttp_server_deferred_token_generation(
                    atomic_load_explicit(&token, memory_order_acquire)),
                current_generation);
    check_equal(chttp_server_deferred_token_state(
                    atomic_load_explicit(&token, memory_order_acquire)),
                CHTTP_SERVER_DEFERRED_WRITING);
    check_equal(chttp_server_deferred_claim(&token, current_generation), SALTS_EALREADY);
  }
}

spec("CHTTP deferred completion lifecycle") {
  before_each() {
    terminal_fixture = (terminal_fixture_state){0};
    chttp_server_impl *server = &terminal_fixture.server;
    chttp_server_connection *connection = &terminal_fixture.connection;
    cmeta_mutex_init(&server->mutex);
    atomic_init(&terminal_fixture.finished, 0);
    atomic_init(&server->buffer_bytes, 0u);
    atomic_init(&server->peak_buffer_bytes, 0u);
    atomic_init(&server->rejected_buffer_allocations, 0u);
    server->config = (chttp_server_config){.max_response_header_count = 4u,
        .max_response_header_bytes = 256u, .max_response_body_bytes = 32u,
        .max_buffered_response_body_bytes = 32u, .buffer_capacity_bytes = 128u};
    server->owner.server = server;
    connection->owner = &server->owner;
    connection->server = server;
    connection->wire_protocol = CHTTP_SERVER_WIRE_HTTP_1_1;
    connection->request_state.server = server;
    connection->request_state.response.impl = &connection->request_state.response_builder;
    connection->request_state.response_builder.server = server;
    connection->request_state.response_builder.deferred_target = &connection->deferred_target;
    connection->deferred_builder.server = server;
    terminal_fixture.deferred_response.impl = &connection->deferred_builder;
    check_equal(chttp_server_response_builder_init(
        &connection->request_state.response_builder, &server->config), SALTS_OK);
    check_equal(chttp_server_response_builder_init(
        &connection->deferred_builder, &server->config), SALTS_OK);
    connection->deferred_target = (chttp_server_deferred_target){
        .server = server, .connection = connection, .request_state = &connection->request_state,
        .response_builder = &connection->deferred_builder, .response = &terminal_fixture.deferred_response,
        .token = &connection->deferred_token, .kind = CHTTP_SERVER_DEFERRED_HTTP_1_1};
    atomic_init(&connection->deferred_token,
        chttp_server_deferred_token(1u, CHTTP_SERVER_DEFERRED_IDLE));
  }

  after_each() {
    if (terminal_fixture.locked) cmeta_mutex_unlock(&terminal_fixture.server.mutex);
    if (terminal_fixture.thread_started) {
      check_equal(cmeta_thread_join(&terminal_fixture.thread), SALTS_OK);
      cmeta_thread_destroy(&terminal_fixture.thread);
    }
    mem_buffer_release(terminal_fixture.body);
    chttp_server_response_builder_destroy(&terminal_fixture.connection.deferred_builder);
    chttp_server_response_builder_destroy(&terminal_fixture.connection.request_state.response_builder);
    check_equal(atomic_load(&terminal_fixture.server.buffer_bytes), (size_t)0u);
    cmeta_mutex_destroy(&terminal_fixture.server.mutex);
  }

  it("keeps terminal ownership until the shutdown lock admits publication and wake") {
    terminal_fixture.body = mem_get_buffer(mem_global(), 1u);
    check_not_null(terminal_fixture.body);
    *(char *)mem_buffer_data(terminal_fixture.body) = 'x';
    mem_set_used(terminal_fixture.body, 1u);
    for (int operation = 0; operation < 3; ++operation) {
      terminal_fixture.operation = operation;
      atomic_store(&terminal_fixture.finished, 0);
      atomic_store(&terminal_fixture.connection.deferred_token,
          chttp_server_deferred_token(1u, CHTTP_SERVER_DEFERRED_PENDING));
      terminal_fixture.handle = (chttp_server_deferred){
          .impl = &terminal_fixture.connection.deferred_target, .generation = 1u};
      cmeta_mutex_lock(&terminal_fixture.server.mutex);
      terminal_fixture.locked = true;
      check_equal(cmeta_thread_create(&terminal_fixture.thread, terminal_complete, NULL), SALTS_OK);
      terminal_fixture.thread_started = true;
      const uint64_t deadline = cmeta_monotonic_ms() + TERMINAL_TEST_TIMEOUT_MS;
      while (chttp_server_deferred_token_state(atomic_load_explicit(
          &terminal_fixture.connection.deferred_token, memory_order_acquire)) ==
          CHTTP_SERVER_DEFERRED_PENDING && cmeta_monotonic_ms() < deadline)
        cmeta_thread_yield();
      check_equal(chttp_server_deferred_token_state(atomic_load_explicit(
          &terminal_fixture.connection.deferred_token, memory_order_acquire)),
          CHTTP_SERVER_DEFERRED_WRITING);
      check_equal(atomic_load_explicit(&terminal_fixture.finished, memory_order_acquire), 0);
      cmeta_mutex_unlock(&terminal_fixture.server.mutex);
      terminal_fixture.locked = false;
      check_equal(cmeta_thread_join(&terminal_fixture.thread), SALTS_OK);
      cmeta_thread_destroy(&terminal_fixture.thread);
      terminal_fixture.thread_started = false;
      check_equal(terminal_fixture.status, SALTS_OK);
      check_null(terminal_fixture.handle.impl);
      check_equal(chttp_server_deferred_token_state(atomic_load(
          &terminal_fixture.connection.deferred_token)), operation == 2
          ? CHTTP_SERVER_DEFERRED_CANCELED : CHTTP_SERVER_DEFERRED_READY);
    }
  }

  it("preserves sealed headers and pending writing or ready tokens after interceptor failure") {
    chttp_server_request_state *state = &terminal_fixture.connection.request_state;
    const chttp_server_route_record route = {.handler = terminal_defer};
    const chttp_server_middleware interceptor = {terminal_interceptor, NULL};
    const chttp_server_request_view request = {.method = CHTTP_METHOD_GET, .path = "/"};
    terminal_fixture.server.middleware = (chttp_server_middleware *)&interceptor;
    terminal_fixture.server.middleware_count = 1u;
    state->admission_complete = true;
    state->admitted_route = (chttp_server_route_record *)&route;
    state->response_builder.request = &request;
    for (int operation = 0; operation < 3; ++operation) {
      terminal_fixture.operation = operation;
      atomic_store(&terminal_fixture.connection.deferred_token,
          chttp_server_deferred_token(1u, CHTTP_SERVER_DEFERRED_IDLE));
      check_equal(chttp_server_dispatch_request(state, &request), SALTS_EALREADY);
      check_true(state->response_builder.deferred);
      check_false(state->response_builder.replied);
      check_equal(state->response_builder.header_count, (size_t)1u);
      check_equal(state->response_builder.headers[0].value, "preserved");
      check_equal(chttp_server_deferred_token_state(atomic_load(
          &terminal_fixture.connection.deferred_token)), operation == 0
          ? CHTTP_SERVER_DEFERRED_PENDING : operation == 1
          ? CHTTP_SERVER_DEFERRED_READY : CHTTP_SERVER_DEFERRED_WRITING);
    }
    check_equal(terminal_fixture.server.stats.handler_errors, (uint64_t)3u);
  }
}
