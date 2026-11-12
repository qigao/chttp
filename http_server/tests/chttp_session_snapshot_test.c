#include "../src/chttp_server_engine.h"
#include "chttp_server_runtime.h"
#include "tinytest.h"

#include <http_server/http.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static native_io_backend_kind session_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config session_test_network(size_t connections) {
  return (cnet_client_config){
      .backend = session_test_backend(),
      .connection_capacity = connections,
      .command_capacity = 16u,
      .request_capacity = 8u,
      .completion_batch_capacity = 8u,
      .event_capacity = 16u,
      .max_send_bytes = 4096u,
      .receive_buffer_bytes = 256u,
      .connect_timeout_ms = 1000u,
      .read_timeout_ms = 1000u,
      .write_timeout_ms = 1000u};
}

static chttp_server_config session_test_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = session_test_network(4u),
      .route_capacity = 2u,
      .middleware_capacity = 1u,
      .max_route_middleware_count = 1u,
      .max_route_param_count = 1u,
      .max_route_param_bytes = 32u,
      .max_target_bytes = 128u,
      .max_header_count = 8u,
      .max_header_bytes = 512u,
      .max_request_body_bytes = 128u,
      .max_response_header_count = 8u,
      .max_response_header_bytes = 512u,
      .max_response_body_bytes = 128u,
      .session_capacity = 4u,
      .session_entry_capacity = 4u,
      .max_session_key_bytes = 32u,
      .max_session_value_bytes = 64u,
      .session_idle_timeout_ms = 60000u,
      .session_cookie_name = "castle_sid",
      .session_cookie_secure = 0,
      .poll_slice_ms = 2u};
}

static chttp_server_request_view session_request(const char *cookie,
                                                 chttp_header *header) {
  chttp_server_request_view request = {0};
  if (cookie != NULL) {
    *header = (chttp_header){"Cookie", cookie};
    request.headers = header;
    request.header_count = 1u;
  }
  return request;
}

spec("CHttp session snapshot concurrency contract") {
  it("keeps handler values request-local and commits snapshots without pointer aliasing") {
    chttp_server server = {0};
    chttp_server_config config = session_test_config();
    chttp_server_impl *impl;
    chttp_server_request_state *creator;
    chttp_server_request_state *first;
    chttp_server_request_state *second;
    chttp_server_request_state *reader;
    chttp_server_request_view request;
    chttp_header cookie_header = {0};
    char cookie[128];
    const char *held;
    int size;

    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    impl = (chttp_server_impl *)server.impl;
    check_not_null(impl);

    creator = &impl->connections[0].request_state;
    first = &impl->connections[1].request_state;
    second = &impl->connections[2].request_state;
    reader = &impl->connections[3].request_state;

    request = session_request(NULL, &cookie_header);
    chttp_session_request_begin(creator, &request);
    check_equal(chttp_session_set(&creator->session, "value", "initial"),
                SALTS_OK);
    check_equal(chttp_session_request_finish(creator), SALTS_OK);
    check_true(creator->session_context.id[0] != '\0');
    size = snprintf(cookie, sizeof(cookie), "castle_sid=%s",
                    creator->session_context.id);
    check_greater(size, 0);
    check_less((size_t)size, sizeof(cookie));
    chttp_server_request_state_reset(creator);

    request = session_request(cookie, &cookie_header);
    chttp_session_request_begin(first, &request);
    chttp_session_request_begin(second, &request);
    held = chttp_session_get(&first->session, "value");
    check_not_null(held);
    check_true(strcmp(held, "initial") == 0);

    check_equal(chttp_session_set(&second->session, "value", "second"),
                SALTS_OK);
    check_equal(chttp_session_request_finish(second), SALTS_OK);

    /* The first request owns a bounded snapshot, not a pointer into shared storage. */
    check_true(strcmp(held, "initial") == 0);
    check_true(strcmp(chttp_session_get(&first->session, "value"), "initial") == 0);

    check_equal(chttp_session_set(&first->session, "value", "first"),
                SALTS_OK);
    check_equal(chttp_session_request_finish(first), SALTS_OK);

    chttp_session_request_begin(reader, &request);
    check_true(strcmp(chttp_session_get(&reader->session, "value"), "first") == 0);

    chttp_server_request_state_reset(first);
    chttp_server_request_state_reset(second);
    chttp_server_request_state_reset(reader);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("rejects a stale snapshot after another request invalidates the record") {
    chttp_server server = {0};
    chttp_server_config config = session_test_config();
    chttp_server_impl *impl;
    chttp_server_request_state *creator;
    chttp_server_request_state *stale;
    chttp_server_request_state *invalidator;
    chttp_server_request_state *reader;
    chttp_server_request_view request;
    chttp_header cookie_header = {0};
    char cookie[128];
    int size;

    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    impl = (chttp_server_impl *)server.impl;
    check_not_null(impl);

    creator = &impl->connections[0].request_state;
    stale = &impl->connections[1].request_state;
    invalidator = &impl->connections[2].request_state;
    reader = &impl->connections[3].request_state;

    request = session_request(NULL, &cookie_header);
    chttp_session_request_begin(creator, &request);
    check_equal(chttp_session_set(&creator->session, "value", "live"),
                SALTS_OK);
    check_equal(chttp_session_request_finish(creator), SALTS_OK);
    size = snprintf(cookie, sizeof(cookie), "castle_sid=%s",
                    creator->session_context.id);
    check_greater(size, 0);
    check_less((size_t)size, sizeof(cookie));
    chttp_server_request_state_reset(creator);

    request = session_request(cookie, &cookie_header);
    chttp_session_request_begin(stale, &request);
    chttp_session_request_begin(invalidator, &request);
    check_true(strcmp(chttp_session_get(&stale->session, "value"), "live") == 0);

    check_equal(chttp_session_invalidate(&invalidator->session), SALTS_OK);
    check_equal(chttp_session_request_finish(invalidator), SALTS_OK);

    check_equal(chttp_session_set(&stale->session, "value", "resurrect"),
                SALTS_OK);
    /* Finish expires the stale cookie rather than restoring the invalidated record. */
    check_equal(chttp_session_request_finish(stale), SALTS_OK);

    chttp_session_request_begin(reader, &request);
    check_null(chttp_session_get(&reader->session, "value"));

    chttp_server_request_state_reset(stale);
    chttp_server_request_state_reset(invalidator);
    chttp_server_request_state_reset(reader);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }
}

static chttp_server detached_server;
static void *detached_storage;
static chttp_session_work *detached_work;
static int detached_commits, detached_status;
static void detached_committed(void *user, int status) {
  (void)user; ++detached_commits; detached_status = status;
}

spec("CHttp detached Session transaction admission") {
  before_each() {
    detached_server = (chttp_server){0}; detached_storage = NULL; detached_work = NULL;
    detached_commits = 0; detached_status = SALTS_OK;
    chttp_server_config config = session_test_config();
    config.session_capacity = 1u;
    check_equal(chttp_server_init(&detached_server, &config), SALTS_OK);
  }
  after_each() {
    chttp_server_impl *impl = detached_server.impl;
    if (impl) {
      for (size_t i = 0u; i < impl->config.network.connection_capacity; ++i)
        chttp_session_work_detach(&impl->connections[i].request_state, SALTS_ECANCELED);
    }
    chttp_session_work_release(detached_work); free(detached_storage);
    check_equal(chttp_server_destroy(&detached_server), SALTS_OK);
  }
  it("does not resurrect an explicitly invalidated generation or reuse its pinned record") {
    chttp_server_impl *impl = detached_server.impl;
    chttp_server_request_state *creator = &impl->connections[0].request_state;
    chttp_server_request_state *stale = &impl->connections[1].request_state;
    chttp_server_request_state *invalidator = &impl->connections[2].request_state;
    chttp_server_request_state *fresh = &impl->connections[3].request_state;
    chttp_header header = {0};
    chttp_server_request_view request = session_request(NULL, &header);
    chttp_session_request_begin(creator, &request);
    check_equal(chttp_session_set(&creator->session, "value", "original"), SALTS_OK);
    check_equal(chttp_session_request_finish(creator), SALTS_OK);
    char cookie[128]; snprintf(cookie, sizeof(cookie), "castle_sid=%s", creator->session_context.id);
    request = session_request(cookie, &header);
    chttp_session_request_begin(stale, &request);
    chttp_session_request_begin(invalidator, &request);
    size_t bytes = 0u;
    check_equal(chttp_session_work_size(&stale->session, &bytes), SALTS_OK);
    detached_storage = malloc(bytes); check_true(detached_storage != NULL);
    check_equal(chttp_session_work_prepare(detached_storage, bytes, &stale->session,
        detached_committed, NULL, &detached_work), SALTS_OK);
    check_equal(chttp_session_work_attach(stale, detached_work), SALTS_OK);
    check_equal(chttp_session_work_begin(detached_work), SALTS_OK);
    check_equal(chttp_session_set(chttp_session_work_view(detached_work), "value", "resurrect"), SALTS_OK);
    check_equal(chttp_session_invalidate(&invalidator->session), SALTS_OK);
    request = session_request(NULL, &header);
    chttp_session_request_begin(fresh, &request);
    check_equal(chttp_session_set(&fresh->session, "value", "new"), SALTS_ENOBUFS);
    chttp_session_work_seal(detached_work, true);
    check_equal(chttp_session_request_finish_deferred(stale, &stale->response), SALTS_ENOENT);
    check_equal(detached_commits, 1); check_equal(detached_status, SALTS_ENOENT);
    check_equal(chttp_session_request_finish_deferred(stale, &stale->response), SALTS_EALREADY);
    check_equal(detached_commits, 1);
    check_equal(impl->sessions[0].used, false);
    chttp_session_work_detach(stale, SALTS_ECANCELED);
    check_equal(detached_commits, 1);
    chttp_session_work_release(detached_work);
    check_equal(chttp_session_set(&fresh->session, "value", "new"), SALTS_OK);
    check_equal(chttp_session_request_finish(fresh), SALTS_OK);
    check_equal(chttp_session_get(chttp_session_work_view(detached_work), "value"), "resurrect");
    check_equal(chttp_session_set(chttp_session_work_view(detached_work), "value", "late"), SALTS_EALREADY);
  }
  it("refuses to silently discard Session mutations made on the original owner") {
    chttp_server_impl *impl = detached_server.impl;
    chttp_server_request_state *state = &impl->connections[0].request_state;
    chttp_header header = {0};
    chttp_server_request_view request = session_request(NULL, &header);
    chttp_session_request_begin(state, &request);
    check_equal(chttp_session_set(&state->session, "value", "owner"), SALTS_OK);
    size_t bytes = 0u;
    check_equal(chttp_session_work_size(&state->session, &bytes), SALTS_ENOTSUP);
    check_equal(chttp_session_get(&state->session, "value"), "owner");
    check_equal(chttp_session_request_finish(state), SALTS_OK);
  }
  it("never applies fail-closed retirement invalidation to a newer authenticated generation") {
    chttp_server_impl *impl = detached_server.impl;
    chttp_server_request_state *creator = &impl->connections[0].request_state;
    chttp_server_request_state *stale = &impl->connections[1].request_state;
    chttp_server_request_state *fresh = &impl->connections[2].request_state;
    chttp_server_request_state *reader = &impl->connections[3].request_state;
    chttp_header header = {0};
    chttp_server_request_view request = session_request(NULL, &header);
    chttp_session_request_begin(creator, &request);
    check_equal(chttp_session_set(&creator->session, "value", "old-identity"), SALTS_OK);
    check_equal(chttp_session_request_finish(creator), SALTS_OK);
    char cookie[128]; snprintf(cookie, sizeof(cookie), "castle_sid=%s", creator->session_context.id);
    request = session_request(cookie, &header);
    chttp_session_request_begin(stale, &request); chttp_session_request_begin(fresh, &request);
    size_t bytes = 0u; check_equal(chttp_session_work_size(&stale->session, &bytes), SALTS_OK);
    detached_storage = malloc(bytes); check_true(detached_storage != NULL);
    check_equal(chttp_session_work_prepare(detached_storage, bytes, &stale->session,
        detached_committed, NULL, &detached_work), SALTS_OK);
    check_equal(chttp_session_work_attach(stale, detached_work), SALTS_OK);
    check_equal(chttp_session_work_begin(detached_work), SALTS_OK);
    check_equal(chttp_session_invalidate(chttp_session_work_view(detached_work)), SALTS_OK);
    check_equal(chttp_session_regenerate(&fresh->session), SALTS_OK);
    check_equal(chttp_session_set(&fresh->session, "value", "new-identity"), SALTS_OK);
    check_equal(chttp_session_request_finish(fresh), SALTS_OK);
    snprintf(cookie, sizeof(cookie), "castle_sid=%s", fresh->session_context.id);
    chttp_session_work_seal(detached_work, true);
    chttp_session_work_detach(stale, SALTS_ECANCELED);
    chttp_session_work_release(detached_work);
    request = session_request(cookie, &header); chttp_session_request_begin(reader, &request);
    check_equal(chttp_session_get(&reader->session, "value"), "new-identity");
    check_equal(impl->sessions[0].worker_pins, 0u);
    check_equal(detached_commits, 1); check_equal(detached_status, SALTS_ECANCELED);
  }
}
