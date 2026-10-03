#include "chttp_server_runtime.h"
#include "tinytest.h"

#include <http_server/http.h>

#include <stdio.h>
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
