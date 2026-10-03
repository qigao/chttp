#include "chttp_server_runtime.h"
#include "tinytest.h"
#include <http_client/http.h>
#include <salts/clock.h>
#include <salts/thread.h>

#include <stdio.h>

static cnet_client_config owner_topology_client_network(void) {
  const cnet_client_config config = {
      .backend =
#if defined(_WIN32)
          NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
          NATIVE_IO_BACKEND_EPOLL,
#else
          NATIVE_IO_BACKEND_KQUEUE,
#endif
      .connection_capacity = 1u,
      .command_capacity = 8u,
      .request_capacity = 4u,
      .completion_batch_capacity = 4u,
      .event_capacity = 8u,
      .max_send_bytes = 4096u,
      .receive_buffer_bytes = 512u,
      .connect_timeout_ms = 2000u,
      .read_timeout_ms = 2000u,
      .write_timeout_ms = 2000u};
  return config;
}

static chttp_client_config owner_topology_client_config(void) {
  const chttp_client_config config = {
      .network = owner_topology_client_network(),
      .request_capacity = 1u,
      .max_start_line_bytes = 128u,
      .max_header_count = 8u,
      .max_header_bytes = 512u,
      .max_request_body_bytes = 128u,
      .max_response_body_bytes = 256u,
      .max_informational_responses = 1u};
  return config;
}

static int owner_topology_ok(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  (void)user;
  (void)request;
  return chttp_server_reply(response, 200u, "text/plain", "ok", 2u);
}

static int owner_topology_get(
    chttp_client *client, const char *uri, chttp_response *out_response) {
  const chttp_options options = {
      .connection_uri = uri,
      .authority = "127.0.0.1",
      .target = "/ok",
      .timeout_ms = 2000u};
  chttp_error error = {0};
  return chttp_get(client, &options, out_response, &error);
}

static int owner_topology_wait_leases(
    chttp_server_impl *impl, size_t first, size_t second,
    uint32_t timeout_ms) {
  const uint64_t deadline = salts_monotonic_ms() + timeout_ms;
  for (;;) {
    chttp_server_owner_lane *owner0 = chttp_server_owner_at(impl, 0u);
    chttp_server_owner_lane *owner1 = chttp_server_owner_at(impl, 1u);
    if (owner0 != NULL && owner1 != NULL &&
        chttp_server_owner_lease_count(owner0) == first &&
        chttp_server_owner_lease_count(owner1) == second)
      return SALTS_OK;
    if (salts_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
    salts_thread_yield();
  }
}

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
    check_not_null(impl->owner.admissions);
    check(impl->owner.admission_sync_initialized);
    check_equal(impl->owner.admission_count, (size_t)0u);

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
      check_not_null(owner->admissions);
      check(owner->admission_sync_initialized);
      check_equal(owner->admission_count, (size_t)0u);
      check_not_null(owner->file_transfers);
      check_not_null(owner->websocket_commands);
      check_equal(owner->websocket_command_count, (size_t)0u);
      check_equal(chttp_server_owner_lease_count(owner), (size_t)0u);
      check_equal(
          chttp_server_owner_runtime_state_get(owner),
          CHTTP_SERVER_OWNER_RUNTIME_IDLE);
      check_equal(
          chttp_server_owner_runtime_transition(
              owner, CHTTP_SERVER_OWNER_RUNTIME_IDLE,
              CHTTP_SERVER_OWNER_RUNTIME_READY),
          SALTS_EINVAL);
      check_equal(
          chttp_server_owner_runtime_transition(
              owner, CHTTP_SERVER_OWNER_RUNTIME_IDLE,
              CHTTP_SERVER_OWNER_RUNTIME_STARTING),
          SALTS_OK);
      check_equal(
          chttp_server_owner_runtime_transition(
              owner, CHTTP_SERVER_OWNER_RUNTIME_STARTING,
              CHTTP_SERVER_OWNER_RUNTIME_IDLE),
          SALTS_OK);
      check_equal(
          chttp_server_owner_runtime_transition(
              owner, CHTTP_SERVER_OWNER_RUNTIME_IDLE,
              CHTTP_SERVER_OWNER_RUNTIME_STARTING),
          SALTS_OK);
      check_equal(
          chttp_server_owner_runtime_transition(
              owner, CHTTP_SERVER_OWNER_RUNTIME_STARTING,
              CHTTP_SERVER_OWNER_RUNTIME_READY),
          SALTS_OK);
      check_equal(
          chttp_server_owner_runtime_transition(
              owner, CHTTP_SERVER_OWNER_RUNTIME_READY,
              CHTTP_SERVER_OWNER_RUNTIME_STOPPING),
          SALTS_OK);
      check_equal(
          chttp_server_owner_runtime_transition(
              owner, CHTTP_SERVER_OWNER_RUNTIME_STOPPING,
              CHTTP_SERVER_OWNER_RUNTIME_DONE),
          SALTS_OK);
      check_equal(
          chttp_server_owner_runtime_transition(
              owner, CHTTP_SERVER_OWNER_RUNTIME_DONE,
              CHTTP_SERVER_OWNER_RUNTIME_READY),
          SALTS_EINVAL);
      check_equal(owner->terminal_status, SALTS_OK);
      check(!owner->network_initialized);
      check(!owner->thread_started);
      for (connection_index = 0u;
           connection_index < owner->connection_count; ++connection_index)
        check(chttp_server_owner_lease_try_acquire(owner));
      check(!chttp_server_owner_lease_try_acquire(owner));
      check_equal(chttp_server_owner_lease_count(owner),
                  owner->connection_count);
      check_equal(chttp_server_owner_lease_release(owner), SALTS_OK);
      check_equal(chttp_server_owner_lease_count(owner),
                  owner->connection_count - 1u);
      check(chttp_server_owner_lease_try_acquire(owner));
      while (chttp_server_owner_lease_count(owner) != 0u)
        check_equal(chttp_server_owner_lease_release(owner), SALTS_OK);
      check_equal(chttp_server_owner_lease_release(owner), SALTS_EALREADY);
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

  it("publishes owner0 READY only after worker-thread network startup") {
    chttp_server server = {0};
    chttp_server_config config = owner_topology_config();
    chttp_server_impl *impl;
    uint16_t port = 0u;

    config.network.connection_capacity = 2u;
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    impl = (chttp_server_impl *)server.impl;
    check_not_null(impl);
    check_equal(
        chttp_server_owner_runtime_state_get(&impl->owner),
        CHTTP_SERVER_OWNER_RUNTIME_IDLE);

    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(
        chttp_server_owner_runtime_state_get(&impl->owner),
        CHTTP_SERVER_OWNER_RUNTIME_READY);
    check(impl->network_initialized);
    check(impl->owner.network_initialized);
    check(impl->thread_started);
    check(impl->owner.thread_started);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check(port != 0u);

    check_equal(chttp_server_stop(&server, 0u), SALTS_OK);
    check_equal(
        chttp_server_owner_runtime_state_get(&impl->owner),
        CHTTP_SERVER_OWNER_RUNTIME_DONE);
    check(!impl->network_initialized);
    check(!impl->owner.network_initialized);
    check(!impl->listener_initialized);
    check(!impl->thread_started);
    check(!impl->owner.thread_started);

    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("runs two fixed owners through detached round-robin admission") {
    chttp_server server = {0};
    chttp_server_config config = owner_topology_config();
    chttp_server_execution_options execution =
        (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
    chttp_client first = {0};
    chttp_client second = {0};
    chttp_client_config client_config = owner_topology_client_config();
    chttp_response first_response = {0};
    chttp_response second_response = {0};
    chttp_server_impl *impl;
    char uri[64];
    uint16_t port = 0u;

    config.network.connection_capacity = 4u;
    execution.owner_count = 2u;
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(
        chttp_server_set_execution_options(&server, &execution), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/ok", owner_topology_ok, NULL), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    impl = (chttp_server_impl *)server.impl;
    check_not_null(impl);
    check_equal(impl->owner_count, (size_t)2u);
    check_equal(
        chttp_server_owner_runtime_state_get(chttp_server_owner_at(impl, 0u)),
        CHTTP_SERVER_OWNER_RUNTIME_READY);
    check_equal(
        chttp_server_owner_runtime_state_get(chttp_server_owner_at(impl, 1u)),
        CHTTP_SERVER_OWNER_RUNTIME_READY);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check(port != 0u);
    check(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                   (unsigned int)port) > 0);

    check_equal(chttp_client_init(&first, &client_config), SALTS_OK);
    check_equal(owner_topology_get(&first, uri, &first_response), SALTS_OK);
    check_equal(first_response.status_code, 200u);
    check_equal(first_response.body, "ok", 2u);
    check(first_response.protocol_keep_alive != 0);
    chttp_response_destroy(&first_response);
    check_equal(owner_topology_wait_leases(impl, 1u, 0u, 2000u), SALTS_OK);

    check_equal(chttp_client_init(&second, &client_config), SALTS_OK);
    check_equal(owner_topology_get(&second, uri, &second_response), SALTS_OK);
    check_equal(second_response.status_code, 200u);
    check_equal(second_response.body, "ok", 2u);
    check(second_response.protocol_keep_alive != 0);
    chttp_response_destroy(&second_response);
    check_equal(owner_topology_wait_leases(impl, 1u, 1u, 2000u), SALTS_OK);

    check_equal(chttp_client_destroy(&first, 2000u), SALTS_OK);
    check_equal(chttp_client_destroy(&second, 2000u), SALTS_OK);
    check_equal(owner_topology_wait_leases(impl, 0u, 0u, 2000u), SALTS_OK);

    check_equal(chttp_server_stop(&server, 2000u), SALTS_OK);
    check_equal(
        chttp_server_owner_runtime_state_get(chttp_server_owner_at(impl, 0u)),
        CHTTP_SERVER_OWNER_RUNTIME_DONE);
    check_equal(
        chttp_server_owner_runtime_state_get(chttp_server_owner_at(impl, 1u)),
        CHTTP_SERVER_OWNER_RUNTIME_DONE);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("rejects exactly once when all owner leases are full and reuses released capacity") {
    chttp_server server = {0};
    chttp_server_config config = owner_topology_config();
    chttp_server_execution_options execution =
        (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
    chttp_client first = {0};
    chttp_client second = {0};
    chttp_client overflow = {0};
    chttp_client replacement = {0};
    chttp_client_config client_config = owner_topology_client_config();
    chttp_response response = {0};
    chttp_server_stats stats = {0};
    chttp_server_impl *impl;
    char uri[64];
    uint16_t port = 0u;
    int status;

    config.network.connection_capacity = 2u;
    execution.owner_count = 2u;
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(
        chttp_server_set_execution_options(&server, &execution), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/ok", owner_topology_ok, NULL), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    impl = (chttp_server_impl *)server.impl;
    check_not_null(impl);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                   (unsigned int)port) > 0);

    check_equal(chttp_client_init(&first, &client_config), SALTS_OK);
    check_equal(owner_topology_get(&first, uri, &response), SALTS_OK);
    chttp_response_destroy(&response);
    response = (chttp_response){0};
    check_equal(owner_topology_wait_leases(impl, 1u, 0u, 2000u), SALTS_OK);

    check_equal(chttp_client_init(&second, &client_config), SALTS_OK);
    check_equal(owner_topology_get(&second, uri, &response), SALTS_OK);
    chttp_response_destroy(&response);
    response = (chttp_response){0};
    check_equal(owner_topology_wait_leases(impl, 1u, 1u, 2000u), SALTS_OK);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)2u);
    check_equal(stats.rejected_connections, (uint64_t)0u);

    check_equal(chttp_client_init(&overflow, &client_config), SALTS_OK);
    status = owner_topology_get(&overflow, uri, &response);
    check(status != SALTS_OK);
    chttp_response_destroy(&response);
    response = (chttp_response){0};
    check_equal(chttp_client_destroy(&overflow, 2000u), SALTS_OK);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)2u);
    check_equal(stats.rejected_connections, (uint64_t)1u);
    check_equal(owner_topology_wait_leases(impl, 1u, 1u, 2000u), SALTS_OK);

    check_equal(chttp_client_destroy(&first, 2000u), SALTS_OK);
    check_equal(owner_topology_wait_leases(impl, 0u, 1u, 2000u), SALTS_OK);

    check_equal(chttp_client_init(&replacement, &client_config), SALTS_OK);
    check_equal(owner_topology_get(&replacement, uri, &response), SALTS_OK);
    check_equal(response.status_code, 200u);
    chttp_response_destroy(&response);
    check_equal(owner_topology_wait_leases(impl, 1u, 1u, 2000u), SALTS_OK);
    check_equal(chttp_server_get_stats(&server, &stats), SALTS_OK);
    check_equal(stats.accepted_connections, (uint64_t)3u);
    check_equal(stats.rejected_connections, (uint64_t)1u);

    check_equal(chttp_client_destroy(&replacement, 2000u), SALTS_OK);
    check_equal(chttp_client_destroy(&second, 2000u), SALTS_OK);
    check_equal(owner_topology_wait_leases(impl, 0u, 0u, 2000u), SALTS_OK);
    check_equal(chttp_server_stop(&server, 2000u), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("rolls failed worker startup back to IDLE and remains retryable") {
    chttp_server first = {0};
    chttp_server second = {0};
    chttp_server_config first_config = owner_topology_config();
    chttp_server_config second_config;
    chttp_server_impl *second_impl;
    uint16_t occupied_port = 0u;

    first_config.network.connection_capacity = 2u;
    check_equal(chttp_server_init(&first, &first_config), SALTS_OK);
    check_equal(chttp_server_start(&first), SALTS_OK);
    check_equal(chttp_server_port(&first, &occupied_port), SALTS_OK);
    check(occupied_port != 0u);

    second_config = owner_topology_config();
    second_config.network.connection_capacity = 2u;
    second_config.port = occupied_port;
    check_equal(chttp_server_init(&second, &second_config), SALTS_OK);
    second_impl = (chttp_server_impl *)second.impl;
    check_not_null(second_impl);

    check_equal(chttp_server_start(&second), SALTS_EADDRINUSE);
    check_equal(
        chttp_server_owner_runtime_state_get(&second_impl->owner),
        CHTTP_SERVER_OWNER_RUNTIME_IDLE);
    check(!second_impl->network_initialized);
    check(!second_impl->owner.network_initialized);
    check(!second_impl->listener_initialized);
    check(!second_impl->thread_started);
    check(!second_impl->owner.thread_started);

    check_equal(chttp_server_stop(&first, 0u), SALTS_OK);
    check_equal(chttp_server_destroy(&first), SALTS_OK);

    check_equal(chttp_server_start(&second), SALTS_OK);
    check_equal(
        chttp_server_owner_runtime_state_get(&second_impl->owner),
        CHTTP_SERVER_OWNER_RUNTIME_READY);
    check_equal(chttp_server_stop(&second, 0u), SALTS_OK);
    check_equal(chttp_server_destroy(&second), SALTS_OK);
  }
}
