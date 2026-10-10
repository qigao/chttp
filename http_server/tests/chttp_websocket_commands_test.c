/* Compile the production WS unit with isolated symbols to inject allocation
 * failures without adding hooks to the public library. Protocol engines and
 * server lifecycle remain real. */
#define chttp_server_websocket_profile_reset test_chttp_server_websocket_profile_reset
#define chttp_server_websocket_profile_send_complete test_chttp_server_websocket_profile_send_complete
#define chttp_server_websocket_profile_callback_send_begin test_chttp_server_websocket_profile_callback_send_begin
#define chttp_server_websocket_peer_init test_chttp_server_websocket_peer_init
#define chttp_server_websocket_peer_reset test_chttp_server_websocket_peer_reset
#define chttp_server_websocket_peer_open test_chttp_server_websocket_peer_open
#define chttp_server_websocket_peer_feed test_chttp_server_websocket_peer_feed
#define chttp_server_websocket_peer_flush test_chttp_server_websocket_peer_flush
#define chttp_server_websocket_peer_transport_closed test_chttp_server_websocket_peer_transport_closed
#define chttp_server_websocket_peer_terminal test_chttp_server_websocket_peer_terminal
#define chttp_server_websocket_reset test_chttp_server_websocket_reset
#define chttp_server_websocket_route_open test_chttp_server_websocket_route_open
#define chttp_server_websocket_upgrade test_chttp_server_websocket_upgrade
#define chttp_server_websocket_input test_chttp_server_websocket_input
#define chttp_server_websocket_progress test_chttp_server_websocket_progress
#define chttp_server_websocket_send_complete test_chttp_server_websocket_send_complete
#define chttp_server_websocket_transport_closed test_chttp_server_websocket_transport_closed
#define chttp_websocket_state_get test_chttp_websocket_state_get
#define chttp_websocket_send_text test_chttp_websocket_send_text
#define chttp_websocket_send_binary test_chttp_websocket_send_binary
#define chttp_websocket_send_ping test_chttp_websocket_send_ping
#define chttp_websocket_send_pong test_chttp_websocket_send_pong
#define chttp_websocket_close test_chttp_websocket_close
#define chttp_server_websocket_session_capture test_chttp_server_websocket_session_capture
#define chttp_server_websocket_send_text test_chttp_server_websocket_send_text
#define chttp_server_websocket_send_binary test_chttp_server_websocket_send_binary
#define chttp_server_websocket_send_ping test_chttp_server_websocket_send_ping
#define chttp_server_websocket_send_pong test_chttp_server_websocket_send_pong
#define chttp_server_websocket_close test_chttp_server_websocket_close
#define chttp_server_websocket_commands_progress test_chttp_server_websocket_commands_progress

#define chttp_active_callback_server test_active_callback_server
#include "chttp_server_runtime.h"
#include "tinytest.h"
#include <stdlib.h>
#include <string.h>

SALTS_THREAD_LOCAL chttp_server_impl *chttp_active_callback_server;

static size_t allocation_attempts;
static bool fail_allocation;
static void (*on_allocate)(void);
static chttp_server fixture;
static chttp_server_impl *impl;
static chttp_server_websocket_session sessions[2];

static int command_buffer_grow(void *context, unsigned char **buffer, size_t *capacity,
                               size_t required, size_t limit, size_t preserve_size) {
  ++allocation_attempts;
  if (on_allocate != NULL) {
    void (*hook)(void) = on_allocate;
    on_allocate = NULL;
    hook();
  }
  if (fail_allocation) return SALTS_ENOMEM;
  return chttp_server_buffer_grow(context, buffer, capacity, required, limit, preserve_size);
}
#define chttp_server_buffer_grow command_buffer_grow
#include "../src/chttp_websocket_server.c"
#undef chttp_server_buffer_grow

typedef struct command_writer {
  bool blocked;
  bool replenish;
  unsigned char payload[32];
  size_t count;
} command_writer;
static command_writer writers[2];
static chttp_server_route_record route;

static int command_write(void *transport, const uint8_t *data, size_t size) {
  command_writer *writer = transport;
  if (writer->blocked) return SALTS_EBUSY;
  if (size != 3u || data[1] != 1u || writer->count == sizeof(writer->payload))
    return SALTS_EPROTO;
  writer->payload[writer->count++] = data[2];
  if (writer->replenish)
    check_equal(chttp_server_websocket_send_text(&sessions[1], "n", 1u), SALTS_OK);
  return SALTS_OK;
}

static chttp_server_config command_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1", .backlog = 4u,
      .network = {.backend =
#if defined(_WIN32)
        NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
        NATIVE_IO_BACKEND_EPOLL,
#else
        NATIVE_IO_BACKEND_KQUEUE,
#endif
        .connection_capacity = 2u, .command_capacity = 8u, .request_capacity = 8u,
        .completion_batch_capacity = 4u, .event_capacity = 8u,
        .max_send_bytes = 4096u, .receive_buffer_bytes = 512u,
        .connect_timeout_ms = 1000u, .read_timeout_ms = 1000u, .write_timeout_ms = 1000u},
      .route_capacity = 2u, .max_target_bytes = 128u, .max_header_count = 8u,
      .max_header_bytes = 512u, .max_request_body_bytes = 128u,
      .max_response_header_count = 8u, .max_response_header_bytes = 512u,
      .max_response_body_bytes = 256u, .poll_slice_ms = 2u};
}

static void check_reserved_capacity(void) {
  check_equal(impl->owner.websocket_command_claims, 1u);
  /* Recursive admission also proves copying does not hold the server lock. */
  check_equal(chttp_server_websocket_send_text(&sessions[1], "", 0u), SALTS_ENOBUFS);
}
static void stop_during_copy(void) {
  cmeta_mutex_lock(&impl->mutex);
  impl->stats.running = 0;
  cmeta_mutex_unlock(&impl->mutex);
  check_equal(chttp_server_destroy(&fixture), SALTS_EBUSY);
  check_not_equal(fixture.impl, NULL);
}

spec("CHttp bounded WebSocket command scheduling") {
  before_each() {
    fixture = (chttp_server){0};
    const chttp_server_config config = command_config();
    check_equal(chttp_server_init(&fixture, &config), SALTS_OK);
    impl = fixture.impl;
    chttp_active_callback_server = impl;
    allocation_attempts = 0u;
    fail_allocation = false;
    on_allocate = NULL;
    memset(writers, 0, sizeof(writers));
    route = (chttp_server_route_record){
        .websocket_max_frame_bytes = 128u, .websocket_max_message_bytes = 128u,
        .websocket_max_buffered_input_bytes = 256u};
    for (size_t i = 0u; i < 2u; ++i) {
      chttp_server_connection *connection = &impl->connections[i];
      connection->active = true;
      connection->server_slot = (uint32_t)i + 1u;
      connection->server_generation = 1u;
      connection->owner = &impl->owner;
      check_equal(chttp_server_websocket_peer_init(&connection->websocket_peer, impl,
          &route, (cnet_connection){.slot = (uint32_t)i + 1u, .generation = 1u},
          (uint32_t)i + 1u, 1u, 0, command_write, &writers[i]), SALTS_OK);
      chttp_server_websocket_peer_open(&connection->websocket_peer);
      check_equal(chttp_server_websocket_session_capture(
          &connection->websocket_peer.handle, &sessions[i]), SALTS_OK);
    }
    /* Drive the real command consumer deterministically without an IO thread. */
    impl->stats.running = 1;
  }
  after_each() {
    on_allocate = NULL;
    fail_allocation = false;
    for (size_t i = 0u; i < 2u; ++i) impl->connections[i].active = false;
    check_equal(chttp_server_websocket_commands_progress(impl, &impl->owner), SALTS_OK);
    check_equal(atomic_load(&impl->buffer_bytes), 0u);
    check_equal(impl->owner.websocket_command_claims, 0u);
    for (size_t i = 0u; i < 2u; ++i)
      chttp_server_websocket_peer_reset(&impl->connections[i].websocket_peer);
    impl->stats.running = 0;
    chttp_active_callback_server = NULL;
    check_equal(chttp_server_destroy(&fixture), SALTS_OK);
  }
  it("lets a ready peer pass a blocked peer and preserves retries before new commands") {
    writers[0].blocked = true;
    check_equal(chttp_websocket_send_text(&impl->connections[0].websocket_peer.handle,
                                         "p", 1u), SALTS_OK);
    check_equal(chttp_server_websocket_send_text(&sessions[0], "a", 1u), SALTS_OK);
    check_equal(chttp_server_websocket_send_text(&sessions[0], "b", 1u), SALTS_OK);
    check_equal(chttp_server_websocket_send_text(&sessions[1], "z", 1u), SALTS_OK);
    check_equal(chttp_server_websocket_commands_progress(impl, &impl->owner), SALTS_OK);
    check_equal(writers[0].count, 0u);
    check_equal(writers[1].count, 1u);
    check_equal(writers[1].payload[0], 'z');
    check_equal(impl->owner.websocket_pending_count, 2u);
    check_equal(chttp_server_websocket_send_text(&sessions[0], "c", 1u), SALTS_OK);
    writers[0].blocked = false;
    check_equal(chttp_server_websocket_peer_flush(&impl->connections[0].websocket_peer), SALTS_OK);
    check_equal(chttp_server_websocket_commands_progress(impl, &impl->owner), SALTS_OK);
    check_equal(writers[0].count, 4u);
    check_equal(memcmp(writers[0].payload, "pabc", 4u), 0);
    check_equal(impl->owner.websocket_pending_count, 0u);
  }
  it("yields after the initial batch even when producers keep publishing") {
    writers[1].replenish = true;
    check_equal(chttp_server_websocket_send_text(&sessions[1], "a", 1u), SALTS_OK);
    check_equal(chttp_server_websocket_commands_progress(impl, &impl->owner), SALTS_OK);
    check_equal(writers[1].count, 1u);
    check_equal(impl->owner.websocket_command_count, 1u);
    check_equal(chttp_server_websocket_commands_progress(impl, &impl->owner), SALTS_OK);
    check_equal(writers[1].count, 2u);
    writers[1].replenish = false;
  }
  it("rejects a full queue before allocating and restores a failed copy reservation") {
    for (size_t i = 0u; i < 7u; ++i)
      check_equal(chttp_server_websocket_send_text(&sessions[0], "a", 1u), SALTS_OK);
    on_allocate = check_reserved_capacity;
    fail_allocation = true;
    check_equal(chttp_server_websocket_send_text(&sessions[0], "a", 1u), SALTS_ENOMEM);
    check_equal(impl->owner.websocket_command_claims, 0u);
    fail_allocation = false;
    check_equal(chttp_server_websocket_send_text(&sessions[0], "a", 1u), SALTS_OK);
    const size_t before = allocation_attempts;
    check_equal(chttp_server_websocket_send_text(&sessions[1], "z", 1u), SALTS_ENOBUFS);
    check_equal(allocation_attempts, before);
  }
  it("counts retrying payloads against producer capacity") {
    writers[0].blocked = true;
    check_equal(chttp_websocket_send_text(&impl->connections[0].websocket_peer.handle,
                                         "p", 1u), SALTS_OK);
    for (size_t i = 0u; i < 8u; ++i)
      check_equal(chttp_server_websocket_send_text(&sessions[0], "a", 1u), SALTS_OK);
    check_equal(chttp_server_websocket_commands_progress(impl, &impl->owner), SALTS_OK);
    check_equal(impl->owner.websocket_pending_count, 8u);
    const size_t before = allocation_attempts;
    check_equal(chttp_server_websocket_send_text(&sessions[1], "z", 1u), SALTS_ENOBUFS);
    check_equal(allocation_attempts, before);
  }
  it("keeps the server alive through a copy racing shutdown and rejects publication") {
    on_allocate = stop_during_copy;
    check_equal(chttp_server_websocket_send_text(&sessions[0], "a", 1u), SALTS_ESHUTDOWN);
    check_equal(impl->owner.websocket_command_count, 0u);
    check_equal(impl->owner.websocket_command_claims, 0u);
    check_equal(atomic_load(&impl->buffer_bytes), 0u);
  }
  it("charges payload bytes to the server budget and restores capacity after consumption") {
    impl->config.buffer_capacity_bytes = 2u;
    check_equal(chttp_server_websocket_send_text(&sessions[0], "a", 1u), SALTS_OK);
    check_equal(chttp_server_websocket_send_text(&sessions[1], "b", 1u), SALTS_OK);
    check_equal(atomic_load(&impl->buffer_bytes), 2u);
    check_equal(chttp_server_websocket_send_text(&sessions[1], "c", 1u), SALTS_ENOBUFS);
    check_equal(impl->owner.websocket_command_claims, 0u);
    check_equal(atomic_load(&impl->rejected_buffer_allocations), 1u);
    check_equal(chttp_server_websocket_commands_progress(impl, &impl->owner), SALTS_OK);
    check_equal(atomic_load(&impl->buffer_bytes), 0u);
    check_equal(chttp_server_websocket_send_text(&sessions[1], "c", 1u), SALTS_OK);
  }
}
