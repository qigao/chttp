#include "chttp_server_runtime.h"
#include <tinytest.h>
#include <string.h>

enum { MANAGER_SLOTS = 4 };
static chttp_server_impl server;
static chttp_server_connection connections[MANAGER_SLOTS];
static cnet_client network;
static size_t recycled[MANAGER_SLOTS], callbacks;

static void manager_state(void *user, cnet_connection handle,
                           cnet_connection_state state, const cnet_error *error) {
  (void)user; (void)handle; (void)state; (void)error;
  ++callbacks;
}
static void manager_recycle(void *user) {
  chttp_server_connection *connection = user;
  ++recycled[connection->server_slot];
}
static void reserve_connection(size_t index) {
  const cnet_manager_attachment attachment = {
      .observer = {.on_state = manager_state, .user = &connections[index]},
      .on_recycle = manager_recycle, .hold_context = true};
  check_equal(cnet_manager_reserve(&server.owner.manager, &attachment,
      &connections[index].managed), SALTS_OK);
}
static void reject_connection(size_t index) {
  cnet_connection handle;
  const cnet_connect_options options = {.uri = "pipe://not-a-managed-stream"};
  reserve_connection(index);
  /* A rejected real Manager admission retires its context without a callback. */
  check_equal(cnet_manager_connect(&server.owner.manager, connections[index].managed,
      &options, &handle), SALTS_ENOTSUP);
  chttp_server_manager_retire(&server.owner, &connections[index]);
}

spec("CHttp owner-local Manager retirement") {
  before_each() {
    memset(&server, 0, sizeof(server));
    memset(connections, 0, sizeof(connections));
    memset(recycled, 0, sizeof(recycled));
    callbacks = 0u;
    network = (cnet_client){0};
    const cnet_client_config config = {
#if defined(_WIN32)
        .backend = NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
        .backend = NATIVE_IO_BACKEND_EPOLL,
#else
        .backend = NATIVE_IO_BACKEND_KQUEUE,
#endif
        .connection_capacity = MANAGER_SLOTS, .command_capacity = 8u,
        .request_capacity = 8u, .completion_batch_capacity = 8u,
        .event_capacity = 8u, .max_send_bytes = 128u,
        .receive_buffer_bytes = 128u};
    check_equal(cnet_client_init(&network, &config), SALTS_OK);
    server.owner.server = &server;
    server.owner.network = &network;
    server.owner.connection_count = MANAGER_SLOTS;
    /* Progress must use its retirement set, never the full connection array. */
    server.connections = NULL;
    const cnet_manager_config manager = {sizeof(manager), CNET_MANAGER_VERSION,
        &network, MANAGER_SLOTS, MANAGER_SLOTS};
    check_equal(cnet_manager_init(&server.owner.manager, &manager), SALTS_OK);
    for (size_t i = 0u; i < MANAGER_SLOTS; ++i) {
      connections[i].owner = &server.owner;
      connections[i].server = &server;
      connections[i].server_slot = (uint32_t)i;
      atomic_init(&connections[i].deferred_token,
          chttp_server_deferred_token(0u, CHTTP_SERVER_DEFERRED_IDLE));
    }
  }
  after_each() {
    size_t work;
    for (size_t i = 0u; i < MANAGER_SLOTS; ++i) {
      cnet_manager_entry entry;
      if (connections[i].managed.slot == 0u) continue;
      if (cnet_manager_lookup(&server.owner.manager, connections[i].managed,
          &entry) != SALTS_OK) continue;
      if (entry.state == CNET_MANAGER_RESERVED)
        check_equal(cnet_manager_cancel(&server.owner.manager, entry.managed), SALTS_OK);
      if (entry.context_held)
        check_equal(cnet_manager_release_context(&server.owner.manager, entry.managed), SALTS_OK);
    }
    check_equal(cnet_manager_advance(&server.owner.manager, MANAGER_SLOTS, &work), SALTS_OK);
    check_equal(cnet_manager_destroy(&server.owner.manager), SALTS_OK);
    check_equal(cnet_client_stop(&network, 1000u), SALTS_OK);
    check_equal(cnet_client_destroy(&network), SALTS_OK);
  }
  it("does no connection traversal or premature cleanup when the retirement set is empty") {
    reserve_connection(0u);
    check_equal(chttp_server_manager_progress(&server.owner), SALTS_OK);
    cnet_manager_snapshot snapshot;
    check_equal(cnet_manager_get_snapshot(&server.owner.manager, &snapshot), SALTS_OK);
    check_equal(snapshot.reserved, (size_t)1u);
    check_equal(snapshot.context_holds, (size_t)1u);
    check_equal(recycled[0], (size_t)0u);
  }
  it("keeps deferred contexts while removing ready entries from head middle and tail") {
    for (size_t i = 0u; i < 3u; ++i) reject_connection(i);
    atomic_store(&connections[2].deferred_token,
        chttp_server_deferred_token(1u, CHTTP_SERVER_DEFERRED_PENDING));
    atomic_store(&connections[0].deferred_token,
        chttp_server_deferred_token(1u, CHTTP_SERVER_DEFERRED_PENDING));
    check_equal(chttp_server_manager_progress(&server.owner), SALTS_OK);
    check_equal(recycled[1], (size_t)1u);
    check_equal(recycled[0] + recycled[2], (size_t)0u);
    atomic_store(&connections[2].deferred_token,
        chttp_server_deferred_token(1u, CHTTP_SERVER_DEFERRED_IDLE));
    check_equal(chttp_server_manager_progress(&server.owner), SALTS_OK);
    check_equal(recycled[2], (size_t)1u);
    check_true(server.owner.manager_retired == &connections[0]);
    atomic_store(&connections[0].deferred_token,
        chttp_server_deferred_token(1u, CHTTP_SERVER_DEFERRED_IDLE));
    check_equal(chttp_server_manager_progress(&server.owner), SALTS_OK);
    check_equal(recycled[0], (size_t)1u);
    check_null(server.owner.manager_retired);
    check_equal(callbacks, (size_t)0u);
  }
  it("registers once and allows the same slot to retire again after real recycle") {
    reject_connection(0u);
    chttp_server_manager_retire(&server.owner, &connections[0]);
    check_null(connections[0].manager_retired_next);
    check_equal(chttp_server_manager_progress(&server.owner), SALTS_OK);
    check_false(connections[0].manager_retired);
    reject_connection(0u);
    check_equal(chttp_server_manager_progress(&server.owner), SALTS_OK);
    check_equal(recycled[0], (size_t)2u);
    check_equal(connections[0].managed.slot, (size_t)0u);
  }
}
