#include "chttp_managed_stream.h"
#include <fmt.h>
#include <salts/clock.h>
#include <tinytest.h>

enum { WAIT_MS = 3000 };
static cnet_client client;
static cnet_listener listener;
static chttp_managed_stream stream;
static cnet_connection connection;
static tstr uri;
static size_t connected, terminal;
static int callback_progress;

static void on_state(void *user, cnet_connection handle,
    cnet_connection_state state, const cnet_error *error) {
  (void)handle; (void)error;
  if (state == CNET_CONNECTION_CONNECTED) {
    ++connected;
    if (user != NULL) callback_progress = chttp_managed_stream_progress(&stream);
  }
  if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) ++terminal;
}
static void on_receive(void *user, cnet_connection handle, const cnet_receive_view *view) {
  (void)user; (void)handle; (void)view;
}
static cnet_connect_options options(const char *target) {
  return (cnet_connect_options){.uri = target,
      .observer = {.on_state = on_state, .on_receive = on_receive, .user = &stream}};
}
static void wait_count(size_t *count, size_t expected) {
  const uint64_t deadline = cmeta_monotonic_ms() + WAIT_MS;
  while (*count < expected && cmeta_monotonic_ms() < deadline) {
    size_t events = 0u;
    check_equal(cnet_client_poll(&client, 1u, &events), SALTS_OK);
    check_equal(chttp_managed_stream_progress(&stream), SALTS_OK);
  }
  check_equal(*count, expected);
}

spec("CHttp dedicated WS managed stream") {
  before_each() {
    native_io_backend_kind backend;
#if defined(_WIN32)
    backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    backend = NATIVE_IO_BACKEND_EPOLL;
#else
    backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
    client = (cnet_client){0};
    listener = (cnet_listener){0};
    stream = (chttp_managed_stream){0};
    connection = (cnet_connection){0};
    connected = terminal = 0u;
    callback_progress = SALTS_OK;
    const cnet_client_config config = {.backend = backend,
        .connection_capacity = 2u, .command_capacity = 8u,
        .request_capacity = 8u, .completion_batch_capacity = 8u,
        .event_capacity = 8u, .max_send_bytes = 128u, .receive_buffer_bytes = 128u,
        .connect_timeout_ms = WAIT_MS};
    const cnet_listener_config listen = {backend, "127.0.0.1", 0u, 8u};
    uint16_t port;
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(chttp_managed_stream_init(&stream, &client), SALTS_OK);
    check_equal(cnet_listener_init(&listener, &listen), SALTS_OK);
    check_equal(cnet_listener_port(&listener, &port), SALTS_OK);
    uri = tstr_format("tcp://127.0.0.1:{}", port);
    check_not_null(uri);
  }
  after_each() {
    check_equal(cnet_client_stop(&client, WAIT_MS), SALTS_OK);
    check_equal(chttp_managed_stream_destroy(&stream), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_equal(cnet_listener_close(&listener), SALTS_OK);
    check_equal(cnet_listener_destroy(&listener), SALTS_OK);
    tstr_free(uri);
    uri = NULL;
  }
  it("recycles synchronous rejection without callbacks and admits a later real connection") {
    cnet_connect_options connect = options("pipe://unsupported");
    check_equal(chttp_managed_stream_connect(&stream, &connect, &connection), SALTS_ENOTSUP);
    cnet_manager_snapshot snapshot;
    check_equal(cnet_manager_get_snapshot(&stream.manager, &snapshot), SALTS_OK);
    check_true(snapshot.drained);
    check_equal(connected + terminal, (size_t)0u);
    connect = options(uri);
    check_equal(chttp_managed_stream_connect(&stream, &connect, &connection), SALTS_OK);
    wait_count(&connected, 1u);
  }
  it("preserves the context through real terminal until the protocol explicitly retires") {
    const cnet_connect_options connect = options(uri);
    check_equal(chttp_managed_stream_connect(&stream, &connect, &connection), SALTS_OK);
    wait_count(&connected, 1u);
    check_equal(callback_progress, SALTS_EBUSY);
    check_equal(chttp_managed_stream_destroy(&stream), SALTS_EBUSY);
    cnet_connection rejected;
    const uint64_t generation = stream.managed.generation;
    check_equal(chttp_managed_stream_connect(&stream, &connect, &rejected), SALTS_EALREADY);
    check_equal(stream.managed.generation, generation);
    check_equal(chttp_managed_stream_close(&stream), SALTS_OK);
    wait_count(&terminal, 1u);
    cnet_manager_snapshot snapshot;
    check_equal(cnet_manager_get_snapshot(&stream.manager, &snapshot), SALTS_OK);
    check_equal(snapshot.retired, (size_t)1u);
    check_equal(snapshot.context_holds, (size_t)1u);
    check_false(snapshot.drained);
    check_equal(chttp_managed_stream_retire(&stream), SALTS_OK);
    check_equal(chttp_managed_stream_retire(&stream), SALTS_OK);
    check_equal(cnet_manager_get_snapshot(&stream.manager, &snapshot), SALTS_OK);
    check_true(snapshot.drained);
  }
  it("closes only its managed physical stream and keeps an unmanaged neighbor usable") {
    const cnet_connect_options connect = options(uri);
    check_equal(chttp_managed_stream_connect(&stream, &connect, &connection), SALTS_OK);
    wait_count(&connected, 1u);
    cnet_connection neighbor;
    cnet_connect_options raw = options(uri);
    raw.observer.user = NULL;
    check_equal(cnet_connect(&client, &raw, &neighbor), SALTS_OK);
    wait_count(&connected, 2u);
    check_equal(chttp_managed_stream_close(&stream), SALTS_OK);
    wait_count(&terminal, 1u);
    check_equal(cnet_receive(&client, neighbor, 1u), SALTS_OK);
    check_equal(chttp_managed_stream_retire(&stream), SALTS_OK);
    check_equal(terminal, (size_t)1u);
  }
}
