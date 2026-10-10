#include "chttp_server_runtime.h"
#include "tinytest.h"
#include <string.h>

enum { RETRY_SLOTS = 6, RETRY_BEGIN = 1, RETRY_COUNT = 4 };
static chttp_server_impl server;
static chttp_server_connection connections[RETRY_SLOTS];
static cnet_client network;
static bool network_stopped;

static void retry_prepare(void) {
  for (size_t i = 0u; i < RETRY_SLOTS; ++i) {
    connections[i].server = &server;
    connections[i].owner = &server.owner;
    connections[i].active = true;
    connections[i].connected = true;
    connections[i].pending_action = CHTTP_SERVER_PENDING_CLOSE;
  }
}

spec("CHttp owner retry traversal") {
  before_each() {
    memset(&server, 0, sizeof(server));
    memset(connections, 0, sizeof(connections));
    network = (cnet_client){0};
    network_stopped = false;
    const cnet_client_config config = {
#if defined(_WIN32)
        .backend = NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
        .backend = NATIVE_IO_BACKEND_EPOLL,
#else
        .backend = NATIVE_IO_BACKEND_KQUEUE,
#endif
        .connection_capacity = RETRY_COUNT, .command_capacity = 8u,
        .request_capacity = 8u, .completion_batch_capacity = 8u,
        .event_capacity = 8u, .max_send_bytes = 128u,
        .receive_buffer_bytes = 128u};
    check_equal(cnet_client_init(&network, &config), SALTS_OK);
    check_equal(cnet_client_stop(&network, 1000u), SALTS_OK);
    network_stopped = true;
    /* Model outstanding HTTP CLOSE retries after transport admission closes.
     * Use the real CNet ESHUTDOWN path: each visited retry must clear its action.
     * No polling thread, fabricated opaque handle, or transport mock is needed. */
    server.config.network.connection_capacity = RETRY_SLOTS;
    server.connections = connections;
    server.owner.server = &server;
    server.owner.network = &network;
    server.owner.connection_begin = RETRY_BEGIN;
    server.owner.connection_count = RETRY_COUNT;
    retry_prepare();
  }
  after_each() {
    if (network.impl != NULL) {
      if (!network_stopped)
        check_equal(cnet_client_stop(&network, 1000u), SALTS_OK);
      check_equal(cnet_client_destroy(&network), SALTS_OK);
    }
  }
  it("services every pending connection in one pass from each cyclic start") {
    for (size_t start = 0u; start < RETRY_COUNT; ++start) {
      retry_prepare();
      server.owner.pending_retry_cursor = start;
      check_equal(chttp_server_retry_pending(&server, &server.owner), SALTS_OK);
      for (size_t i = RETRY_BEGIN; i < RETRY_BEGIN + RETRY_COUNT; ++i)
        check_equal(connections[i].pending_action, CHTTP_SERVER_PENDING_NONE);
      check_equal(server.owner.pending_retry_cursor, start);
      check_equal(connections[0].pending_action, CHTTP_SERVER_PENDING_CLOSE);
      check_equal(connections[RETRY_SLOTS - 1].pending_action, CHTTP_SERVER_PENDING_CLOSE);
    }
  }
  it("skips inactive slots and wraps without losing later retries") {
    connections[2].active = false;
    server.owner.pending_retry_cursor = 3u;
    check_equal(chttp_server_retry_pending(&server, &server.owner), SALTS_OK);
    check_equal(connections[1].pending_action, CHTTP_SERVER_PENDING_NONE);
    check_equal(connections[2].pending_action, CHTTP_SERVER_PENDING_CLOSE);
    check_equal(connections[3].pending_action, CHTTP_SERVER_PENDING_NONE);
    check_equal(connections[4].pending_action, CHTTP_SERVER_PENDING_NONE);
    check_equal(server.owner.pending_retry_cursor, 3u);
  }
  it("normalizes an out-of-range cursor and rejects an invalid owner span") {
    server.owner.pending_retry_cursor = RETRY_COUNT;
    check_equal(chttp_server_retry_pending(&server, &server.owner), SALTS_OK);
    for (size_t i = RETRY_BEGIN; i < RETRY_BEGIN + RETRY_COUNT; ++i)
      check_equal(connections[i].pending_action, CHTTP_SERVER_PENDING_NONE);
    server.owner.connection_begin = RETRY_SLOTS;
    check_equal(chttp_server_retry_pending(&server, &server.owner), SALTS_EINVAL);
  }
}
