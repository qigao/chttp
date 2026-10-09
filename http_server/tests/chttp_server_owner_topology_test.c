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
  if (user != NULL) {
    chttp_server_impl *impl = ((chttp_server *)user)->impl;
    size_t matched = 0u;
    for (size_t i = 0u; i < impl->owner_count; ++i) {
      chttp_server_owner_lane *owner = chttp_server_owner_at(impl, i);
      cnet_manager_snapshot snapshot;
      const int status = cnet_manager_get_snapshot(&owner->manager, &snapshot);
      if (status == SALTS_EPERM) continue;
      check_equal(status, SALTS_OK);
      check_equal(snapshot.record_capacity, owner->connection_count);
      check(snapshot.bound > 0u);
      check(snapshot.context_holds >= snapshot.bound);
      check(!snapshot.drained);
      ++matched;
    }
    check_equal(matched, (size_t)1u);
  }
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
  const uint64_t deadline = cmeta_monotonic_ms() + timeout_ms;
  for (;;) {
    chttp_server_owner_lane *owner0 = chttp_server_owner_at(impl, 0u);
    chttp_server_owner_lane *owner1 = chttp_server_owner_at(impl, 1u);
    if (owner0 != NULL && owner1 != NULL &&
        chttp_server_owner_lease_count(owner0) == first &&
        chttp_server_owner_lease_count(owner1) == second)
      return SALTS_OK;
    if (cmeta_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
    cmeta_thread_yield();
  }
}

typedef struct owner_topology_block_probe {
  atomic_int entered;
  atomic_int release;
} owner_topology_block_probe;

typedef struct owner_topology_request_thread_args {
  chttp_client_config config;
  char uri[64];
  const char *target;
  atomic_int completed;
  int status;
} owner_topology_request_thread_args;

typedef struct owner_topology_stop_thread_args {
  chttp_server *server;
  atomic_int completed;
  int status;
} owner_topology_stop_thread_args;

static int owner_topology_block(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  owner_topology_block_probe *probe = (owner_topology_block_probe *)user;
  (void)request;
  if (probe == NULL) return SALTS_EINVAL;
  atomic_store_explicit(&probe->entered, 1, memory_order_release);
  while (!atomic_load_explicit(&probe->release, memory_order_acquire))
    cmeta_thread_yield();
  return chttp_server_reply(response, 200u, "text/plain", "released", 8u);
}

static void owner_topology_request_thread(void *user) {
  owner_topology_request_thread_args *args =
      (owner_topology_request_thread_args *)user;
  chttp_client client = {0};
  chttp_response response = {0};
  chttp_error error = {0};
  chttp_options options;
  int status;
  if (args == NULL) return;
  status = chttp_client_init(&client, &args->config);
  if (status == SALTS_OK) {
    options = (chttp_options){
        .connection_uri = args->uri,
        .authority = "127.0.0.1",
        .target = args->target,
        .timeout_ms = 5000u};
    status = chttp_get(&client, &options, &response, &error);
    chttp_response_destroy(&response);
    {
      const int destroy_status = chttp_client_destroy(&client, 5000u);
      if (status == SALTS_OK && destroy_status != SALTS_OK)
        status = destroy_status;
    }
  }
  args->status = status;
  atomic_store_explicit(&args->completed, 1, memory_order_release);
}

static void owner_topology_stop_thread(void *user) {
  owner_topology_stop_thread_args *args =
      (owner_topology_stop_thread_args *)user;
  if (args == NULL || args->server == NULL) return;
  args->status = chttp_server_stop(args->server, 5000u);
  atomic_store_explicit(&args->completed, 1, memory_order_release);
}

static int owner_topology_wait_atomic(
    const atomic_int *value, int expected, uint32_t timeout_ms) {
  const uint64_t deadline = cmeta_monotonic_ms() + timeout_ms;
  while (atomic_load_explicit(value, memory_order_acquire) != expected) {
    if (cmeta_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
    cmeta_thread_yield();
  }
  return SALTS_OK;
}

static int owner_topology_wait_pending(
    chttp_server_impl *impl, size_t owner_index, size_t admission_count,
    size_t lease_count, uint32_t timeout_ms) {
  const uint64_t deadline = cmeta_monotonic_ms() + timeout_ms;
  chttp_server_owner_lane *owner;
  if (impl == NULL) return SALTS_EINVAL;
  owner = chttp_server_owner_at(impl, owner_index);
  if (owner == NULL || owner->handoff.impl == NULL) return SALTS_EINVAL;
  for (;;) {
    size_t observed_admissions;
    observed_admissions = chttp_server_owner_admission_count(owner);
    if (observed_admissions == admission_count &&
        chttp_server_owner_lease_count(owner) == lease_count)
      return SALTS_OK;
    if (cmeta_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
    cmeta_thread_yield();
  }
}

static int owner_topology_wait_stop_requested(
    chttp_server_impl *impl, uint32_t timeout_ms) {
  const uint64_t deadline = cmeta_monotonic_ms() + timeout_ms;
  if (impl == NULL) return SALTS_EINVAL;
  for (;;) {
    bool stop_requested;
    cmeta_mutex_lock(&impl->mutex);
    stop_requested = impl->stop_requested;
    cmeta_mutex_unlock(&impl->mutex);
    if (stop_requested) return SALTS_OK;
    if (cmeta_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
    cmeta_thread_yield();
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
    check_not_null(impl->placement_hints);
    check(impl->additional_owners == NULL);
    check_not_null(impl->owner.handoff.impl);
    check_equal(chttp_server_owner_admission_count(&impl->owner), (size_t)0u);

    options.owner_count = 3u;
    check_equal(chttp_server_set_execution_options(&server, &options), SALTS_OK);
    check_equal(impl->owner_count, (size_t)3u);
    check_not_null(impl->placement_hints);
    check_not_null(impl->additional_owners);

    for (owner_index = 0u; owner_index < impl->owner_count; ++owner_index) {
      chttp_server_owner_lane *owner = chttp_server_owner_at(impl, owner_index);
      const size_t end = expected_begin[owner_index] + expected_count[owner_index];
      check_not_null(owner);
      check(owner->server == impl);
      check_equal(owner->connection_begin, expected_begin[owner_index]);
      check_equal(owner->connection_count, expected_count[owner_index]);
      check_equal(owner->file_transfer_capacity, expected_count[owner_index]);
      check_not_null(owner->handoff.impl);
      check_equal(chttp_server_owner_admission_count(owner), (size_t)0u);
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
      cnet_handoff_ticket tickets[5] = {{0}}, rejected = {0};
      for (connection_index = 0u; connection_index < owner->connection_count; ++connection_index)
        check_equal(cnet_handoff_reserve(&owner->handoff, &tickets[connection_index]), SALTS_OK);
      check_equal(cnet_handoff_reserve(&owner->handoff, &rejected), SALTS_ENOBUFS);
      check_equal(chttp_server_owner_lease_count(owner), owner->connection_count);
      const cnet_handoff_ticket stale = tickets[0];
      check_equal(cnet_handoff_release(&owner->handoff, stale), SALTS_OK);
      check_equal(chttp_server_owner_lease_count(owner), owner->connection_count - 1u);
      check_equal(cnet_handoff_reserve(&owner->handoff, &tickets[0]), SALTS_OK);
      check_equal(cnet_handoff_release(&owner->handoff, stale), SALTS_ENOENT);
      for (connection_index = 0u; connection_index < owner->connection_count; ++connection_index)
        check_equal(cnet_handoff_release(&owner->handoff, tickets[connection_index]), SALTS_OK);
      check_equal(chttp_server_owner_lease_count(owner), 0u);
      check(owner_index == 0u ? owner->network == &impl->network
                              : owner->network == NULL);
      for (connection_index = expected_begin[owner_index];
           connection_index < end; ++connection_index)
        check(impl->connections[connection_index].owner == owner);
    }

    options.owner_count = 1u;
    check_equal(chttp_server_set_execution_options(&server, &options), SALTS_OK);
    check_equal(impl->owner_count, (size_t)1u);
    check_not_null(impl->placement_hints);
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
    check(impl->listener_initialized);
    check(impl->listener_thread_started);
    check(impl->listener_startup_reported);
    check(impl->listener_ready);
    check(!impl->listener_done);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check(port != 0u);

    check_equal(chttp_server_stop(&server, 0u), SALTS_OK);
    check_equal(
        chttp_server_owner_runtime_state_get(&impl->owner),
        CHTTP_SERVER_OWNER_RUNTIME_DONE);
    check(!impl->network_initialized);
    check(!impl->owner.network_initialized);
    check(!impl->listener_initialized);
    check(!impl->listener_thread_started);
    check(impl->listener_done);
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
        chttp_server_get(&server, "/ok", owner_topology_ok, &server), SALTS_OK);
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

  it("cancels a pending detached admission during multi-owner stop") {
    chttp_server server = {0};
    chttp_server_config config = owner_topology_config();
    chttp_server_execution_options execution =
        (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
    chttp_client first = {0};
    chttp_client third = {0};
    chttp_client_config client_config = owner_topology_client_config();
    chttp_response response = {0};
    chttp_server_impl *impl;
    owner_topology_block_probe block = {0};
    owner_topology_request_thread_args blocked_request = {0};
    owner_topology_request_thread_args pending_request = {0};
    owner_topology_stop_thread_args stop_args = {0};
    cmeta_thread_t blocked_thread = NULL;
    cmeta_thread_t pending_thread = NULL;
    cmeta_thread_t stop_thread = NULL;
    char uri[64];
    uint16_t port = 0u;

    atomic_init(&block.entered, 0);
    atomic_init(&block.release, 0);
    atomic_init(&blocked_request.completed, 0);
    atomic_init(&pending_request.completed, 0);
    atomic_init(&stop_args.completed, 0);

    config.network.connection_capacity = 4u;
    config.route_capacity = 2u;
    execution.owner_count = 2u;
    check_equal(chttp_server_init(&server, &config), SALTS_OK);
    check_equal(
        chttp_server_set_execution_options(&server, &execution), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/ok", owner_topology_ok, NULL), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/block", owner_topology_block, &block),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    impl = (chttp_server_impl *)server.impl;
    check_not_null(impl);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                   (unsigned int)port) > 0);

    /* First physical connection -> owner0. Keep it alive. */
    check_equal(chttp_client_init(&first, &client_config), SALTS_OK);
    check_equal(owner_topology_get(&first, uri, &response), SALTS_OK);
    chttp_response_destroy(&response);
    response = (chttp_response){0};
    check_equal(owner_topology_wait_leases(impl, 1u, 0u, 2000u), SALTS_OK);

    /* Second physical connection -> owner1. Block inside its handler. */
    blocked_request.config = client_config;
    blocked_request.target = "/block";
    check(snprintf(blocked_request.uri, sizeof(blocked_request.uri), "%s", uri) > 0);
    check_equal(
        cmeta_thread_create(
            &blocked_thread, owner_topology_request_thread, &blocked_request),
        SALTS_OK);
    check_equal(owner_topology_wait_atomic(&block.entered, 1, 2000u), SALTS_OK);
    check_equal(owner_topology_wait_leases(impl, 1u, 1u, 2000u), SALTS_OK);

    /* Third physical connection -> owner0's second slot. */
    check_equal(chttp_client_init(&third, &client_config), SALTS_OK);
    check_equal(owner_topology_get(&third, uri, &response), SALTS_OK);
    chttp_response_destroy(&response);
    response = (chttp_response){0};
    check_equal(owner_topology_wait_leases(impl, 2u, 1u, 2000u), SALTS_OK);

    /*
     * Fourth physical connection -> owner1. owner1 is blocked in user code, so
     * the detached stream must remain pending in owner1's bounded ring.
     */
    pending_request.config = client_config;
    pending_request.target = "/ok";
    check(snprintf(pending_request.uri, sizeof(pending_request.uri), "%s", uri) > 0);
    check_equal(
        cmeta_thread_create(
            &pending_thread, owner_topology_request_thread, &pending_request),
        SALTS_OK);
    check_equal(owner_topology_wait_pending(impl, 1u, 1u, 2u, 2000u), SALTS_OK);

    stop_args.server = &server;
    check_equal(
        cmeta_thread_create(&stop_thread, owner_topology_stop_thread, &stop_args),
        SALTS_OK);
    check_equal(owner_topology_wait_stop_requested(impl, 2000u), SALTS_OK);

    /* Let owner1 leave the handler and enter its owner-local shutdown path. */
    atomic_store_explicit(&block.release, 1, memory_order_release);
    check_equal(owner_topology_wait_atomic(&stop_args.completed, 1, 5000u), SALTS_OK);
    check_equal(cmeta_thread_join(&stop_thread), SALTS_OK);
    cmeta_thread_destroy(&stop_thread);
    check_equal(stop_args.status, SALTS_OK);

    check_equal(owner_topology_wait_atomic(&blocked_request.completed, 1, 2000u), SALTS_OK);
    check_equal(cmeta_thread_join(&blocked_thread), SALTS_OK);
    cmeta_thread_destroy(&blocked_thread);
    check_equal(owner_topology_wait_atomic(&pending_request.completed, 1, 2000u), SALTS_OK);
    check_equal(cmeta_thread_join(&pending_thread), SALTS_OK);
    cmeta_thread_destroy(&pending_thread);
    check(pending_request.status != SALTS_OK);

    check_equal(chttp_server_owner_lease_count(chttp_server_owner_at(impl, 0u)),
                (size_t)0u);
    check_equal(chttp_server_owner_lease_count(chttp_server_owner_at(impl, 1u)),
                (size_t)0u);
    check_equal(owner_topology_wait_pending(impl, 1u, 0u, 0u, 2000u), SALTS_OK);
    check_equal(
        chttp_server_owner_runtime_state_get(chttp_server_owner_at(impl, 0u)),
        CHTTP_SERVER_OWNER_RUNTIME_DONE);
    check_equal(
        chttp_server_owner_runtime_state_get(chttp_server_owner_at(impl, 1u)),
        CHTTP_SERVER_OWNER_RUNTIME_DONE);

    check_equal(chttp_client_destroy(&first, 2000u), SALTS_OK);
    check_equal(chttp_client_destroy(&third, 2000u), SALTS_OK);
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
    check(!second_impl->listener_thread_started);
    check(!second_impl->listener_startup_reported);
    check(!second_impl->listener_ready);
    check(!second_impl->listener_done);
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
