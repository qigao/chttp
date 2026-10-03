#include <http_server/http.h>
#include "tinytest.h"

static chttp_server_config execution_options_config(void) {
  chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 4u,
      .network = {
          .connection_capacity = 2u,
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
      .max_target_bytes = 128u,
      .max_header_count = 8u,
      .max_header_bytes = 512u,
      .max_request_body_bytes = 128u,
      .max_response_header_count = 8u,
      .max_response_header_bytes = 512u,
      .max_response_body_bytes = 256u,
      .poll_slice_ms = 2u};

#if defined(_WIN32)
  config.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  config.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
  config.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
  return config;
}

spec("CHttp server execution options") {
  it("validates the versioned pre-start contract and fails closed for unavailable multi-owner") {
    chttp_server server = {0};
    chttp_server_config config = execution_options_config();
    chttp_server_execution_options options =
        (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
    chttp_server_execution_options invalid;

    check_equal(chttp_server_init(&server, &config), SALTS_OK);

    invalid = options;
    invalid.size = sizeof(invalid) - 1u;
    check_equal(chttp_server_set_execution_options(&server, &invalid), SALTS_EINVAL);

    invalid = options;
    invalid.version = CHTTP_SERVER_EXECUTION_OPTIONS_VERSION + 1u;
    check_equal(chttp_server_set_execution_options(&server, &invalid), SALTS_EINVAL);

    invalid = options;
    invalid.owner_count = 0u;
    check_equal(chttp_server_set_execution_options(&server, &invalid), SALTS_EINVAL);

    invalid = options;
    invalid.owner_count = config.network.connection_capacity + 1u;
    check_equal(chttp_server_set_execution_options(&server, &invalid), SALTS_EINVAL);

    options.owner_count = 2u;
    check_equal(chttp_server_set_execution_options(&server, &options), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_ENOTSUP);

    options.owner_count = 1u;
    check_equal(chttp_server_set_execution_options(&server, &options), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }
}
