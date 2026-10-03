#include "chttp_server_runtime.h"
#include "tinytest.h"

#include <stdio.h>
#include <string.h>

static chttp_server_config session_test_config(void) {
  const chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 4u,
      .network = {
          .backend = NATIVE_IO_BACKEND_EPOLL,
          .connection_capacity = 1u,
          .command_capacity = 8u,
          .request_capacity = 4u,
          .completion_batch_capacity = 4u,
          .event_capacity = 8u,
          .max_send_bytes = 4096u,
          .receive_buffer_bytes = 256u,
          .connect_timeout_ms = 1000u,
          .read_timeout_ms = 1000u,
          .write_timeout_ms = 1000u},
      .route_capacity = 1u,
      .middleware_capacity = 1u,
      .max_route_middleware_count = 1u,
      .max_route_param_count = 1u,
      .max_route_param_bytes = 64u,
      .max_target_bytes = 128u,
      .max_header_count = 8u,
      .max_header_bytes = 512u,
      .max_request_body_bytes = 128u,
      .max_response_header_count = 8u,
      .max_response_header_bytes = 512u,
      .max_response_body_bytes = 256u,
      .session_capacity = 8u,
      .session_entry_capacity = 4u,
      .max_session_key_bytes = 32u,
      .max_session_value_bytes = 64u,
      .session_idle_timeout_ms = 60000u,
      .session_cookie_name = "shard_sid",
      .poll_slice_ms = 2u};
  return config;
}

spec("CHttp session shard snapshots") {
  it("keeps borrowed values request-local while shard writes commit immediately") {
    chttp_server server = {0};
    chttp_server_config config = session_test_config();
    chttp_server_impl *impl;
    chttp_server_request_state seed = {0};
    chttp_server_request_state first = {0};
    chttp_server_request_state second = {0};
    chttp_server_request_state observer = {0};
    chttp_server_request_view empty = {0};
    chttp_server_request_view with_cookie = {0};
    chttp_header cookie_header = {0};
    char cookie[96];
    char session_id[33];
    const char *first_borrowed;
    const char *second_borrowed;

#if !defined(__linux__)
    config.network.backend =
#if defined(_WIN32)
        NATIVE_IO_BACKEND_IOCP;
#else
        NATIVE_IO_BACKEND_KQUEUE;
#endif
#endif

    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    impl = (chttp_server_impl *)server.impl;
    check_not_null(impl);
    check_equal(impl->session_shard_count, (size_t)8u);

    check_equal(chttp_server_request_state_init(&seed, impl), SALTS_OK);
    chttp_session_request_begin(&seed, &empty);
    check_equal(chttp_session_set(&seed.session, "value", "old"), SALTS_OK);
    check(seed.session_context.id[0] != '\0');
    memcpy(session_id, seed.session_context.id, sizeof(session_id));
    check_equal(chttp_session_request_finish(&seed), SALTS_OK);
    chttp_server_request_state_reset(&seed);

    check_greater(
        snprintf(cookie, sizeof(cookie), "shard_sid=%s", session_id), 0);
    cookie_header = (chttp_header){.name = "Cookie", .value = cookie};
    with_cookie.headers = &cookie_header;
    with_cookie.header_count = 1u;

    check_equal(chttp_server_request_state_init(&first, impl), SALTS_OK);
    check_equal(chttp_server_request_state_init(&second, impl), SALTS_OK);
    check_equal(chttp_server_request_state_init(&observer, impl), SALTS_OK);

    chttp_session_request_begin(&first, &with_cookie);
    chttp_session_request_begin(&second, &with_cookie);
    first_borrowed = chttp_session_get(&first.session, "value");
    second_borrowed = chttp_session_get(&second.session, "value");
    check_not_null(first_borrowed);
    check_not_null(second_borrowed);
    check_equal(first_borrowed, "old");
    check_equal(second_borrowed, "old");

    check_equal(chttp_session_set(&second.session, "value", "new"), SALTS_OK);
    check_equal(chttp_session_get(&second.session, "value"), "new");
    check_equal(first_borrowed, "old");
    check_equal(chttp_session_get(&first.session, "value"), "old");

    second_borrowed = chttp_session_get(&second.session, "value");
    check_equal(chttp_session_set(&first.session, "value", "again"), SALTS_OK);
    check_equal(chttp_session_get(&first.session, "value"), "again");
    check_equal(second_borrowed, "new");

    chttp_session_request_begin(&observer, &with_cookie);
    check_equal(chttp_session_get(&observer.session, "value"), "again");

    check_equal(chttp_session_invalidate(&observer.session), SALTS_OK);

    chttp_server_request_state_destroy(&observer);
    chttp_server_request_state_destroy(&second);
    chttp_server_request_state_destroy(&first);
    chttp_server_request_state_destroy(&seed);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }
}
