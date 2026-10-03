#include "chttp_server_runtime.h"
#include "tinytest.h"

static chttp_server_config owner_topology_config(void) {
  chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = {
          .connection_capacity = 5u,
          .command_capacity = 8u,
          .request_capacity = 8u,
          .completion_batch_capacity = 4u,
          .event_capacity = 8u,
          .max_send_bytes = 4096u,
          .receive_buffer_bytes = 512u,
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

spec("CHttp owner topology") {
  it("partitions fixed connection ranges and bounded owner-local storage") {
    chttp_server server = {0};
    chttp_server_config config = owner_topology_config();
    chttp_server_execution_options options =
        (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
    chttp_server_impl *impl;
    const size_t expected_begin[] = {0u, 2u, 4u};
    const size_t expected_count[] = {2u, 2u, 1u};
    size_t owner_index;
    size_t connection_index;

    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    impl = (chttp_server_impl *)server.impl;
    check_not_null(impl);
    check_equal(impl->owner_count, (size_t)1u);
    check(impl->additional_owners == NULL);

    options.owner_count = 3u;
    check_equal(chttp_server_set_execution_options(&server, &options), SALTS_OK);
    check_equal(impl->owner_count, (size_t)3u);
    check_not_null(impl->additional_owners);

    for (owner_index = 0u; owner_index < impl->owner_count; ++owner_index) {
      chttp_server_owner_lane *owner = chttp_server_owner_at(impl, owner_index);
      const size_t end = expected_begin[owner_index] + expected_count[owner_index];
      check_not_null(owner);
      check(owner->server == impl);
      check_equal(owner->connection_begin, expected_begin[owner_index]);
      check_equal(owner->connection_count, expected_count[owner_index]);
      check_equal(owner->file_transfer_capacity, expected_count[owner_index]);
      check_not_null(owner->file_transfers);
      check_not_null(owner->websocket_commands);
      check_equal(owner->websocket_command_count, (size_t)0u);
      check(owner_index == 0u ? owner->network == &impl->network
                              : owner->network == NULL);
      for (connection_index = expected_begin[owner_index];
           connection_index < end; ++connection_index)
        check(impl->connections[connection_index].owner == owner);
    }

    options.owner_count = 1u;
    check_equal(chttp_server_set_execution_options(&server, &options), SALTS_OK);
    check_equal(impl->owner_count, (size_t)1u);
    check(impl->additional_owners == NULL);
    check_equal(impl->owner.connection_begin, (size_t)0u);
    check_equal(impl->owner.connection_count, config.network.connection_capacity);
    check_equal(impl->owner.file_transfer_capacity,
                config.network.connection_capacity);
    for (connection_index = 0u;
         connection_index < config.network.connection_capacity;
         ++connection_index)
      check(impl->connections[connection_index].owner == &impl->owner);

    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }
}
