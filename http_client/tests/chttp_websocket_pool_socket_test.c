#include "../../http_server/src/chttp_server_engine.h"
/* Exercise the production pool and read its live socket through CNet's public
 * owner API, matching the existing pool lifecycle test's source-unit approach.
 * No native descriptor or test hook enters the product API. */
#include "../src/chttp_websocket_pool.c"
#include <http_server/http.h>
#include "tinytest.h"

enum { SOCKET_TEST_TIMEOUT_MS = 5000, SOCKET_TEST_WIRE_BYTES = 64 * 1024 };
static chttp_websocket_pool pool;
static chttp_server server;
static chttp_websocket_pool_config config;

static cnet_client_config socket_test_network(void) {
  return (cnet_client_config){
      .backend =
#if defined(_WIN32)
          NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
          NATIVE_IO_BACKEND_EPOLL,
#else
          NATIVE_IO_BACKEND_KQUEUE,
#endif
      .connection_capacity = 1u, .command_capacity = 8u, .request_capacity = 8u,
      .completion_batch_capacity = 4u, .event_capacity = 8u,
      .max_send_bytes = SOCKET_TEST_WIRE_BYTES, .receive_buffer_bytes = SOCKET_TEST_WIRE_BYTES,
      .connect_timeout_ms = SOCKET_TEST_TIMEOUT_MS, .read_timeout_ms = SOCKET_TEST_TIMEOUT_MS,
      .write_timeout_ms = SOCKET_TEST_TIMEOUT_MS};
}

static void socket_test_event(void *user, chttp_websocket *websocket,
                              const chttp_websocket_event *event) {
  (void)user;
  (void)websocket;
  (void)event;
}

static int socket_test_open(void *user, chttp_websocket *websocket,
                             const chttp_server_request_view *request,
                             chttp_server_response *response) {
  (void)user;
  (void)websocket;
  (void)request;
  return chttp_server_response_set_header(response, "X-Socket-Test", "accepted");
}

spec("WebSocket pool socket policy") {
  before_each() {
    pool = (chttp_websocket_pool){0};
    server = (chttp_server){0};
    config = (chttp_websocket_pool_config){
        .size = sizeof(config), .session_capacity = 2u,
        .client = {.size = sizeof(config.client), .network = socket_test_network(),
                   .event_capacity = 4u, .max_frame_bytes = 1024u,
                   .max_message_bytes = 1024u, .max_handshake_header_bytes = 4096u}};
  }
  after_each() {
    if (pool.impl != NULL)
      check_equal(chttp_websocket_pool_destroy(&pool, SOCKET_TEST_TIMEOUT_MS), SALTS_OK);
    if (server.impl != NULL) {
      check_equal(chttp_server_stop(&server, SOCKET_TEST_TIMEOUT_MS), SALTS_OK);
      check_equal(chttp_server_destroy(&server), SALTS_OK);
    }
  }

  it("rejects invalid socket policy without publishing a pool") {
    config.client.socket_options = (cnet_stream_socket_options)CNET_STREAM_SOCKET_OPTIONS_INIT;
    --config.client.socket_options.size;
    check_equal(chttp_websocket_pool_init(&pool, &config), SALTS_EINVAL);
    check_null(pool.impl);
    config.client.socket_options = (cnet_stream_socket_options)CNET_STREAM_SOCKET_OPTIONS_INIT;
    config.client.socket_options.nodelay = 2;
    check_equal(chttp_websocket_pool_init(&pool, &config), SALTS_EINVAL);
    check_null(pool.impl);
    config.client.socket_options.nodelay = 0;
    config.client.socket_options.keepalive_interval_ms = 1000u;
    check_equal(chttp_websocket_pool_init(&pool, &config), SALTS_EINVAL);
    check_null(pool.impl);
  }

  it("preserves an omitted socket policy") {
    check_equal(chttp_websocket_pool_init(&pool, &config), SALTS_OK);
  }

  it("applies nodelay and keepalive to the shared live H2 socket") {
    const chttp_server_config server_config = {
        .host = "127.0.0.1", .backlog = 4u, .network = socket_test_network(),
        .route_capacity = 1u, .max_target_bytes = 128u, .max_header_count = 16u,
        .max_header_bytes = 1024u, .max_request_body_bytes = 128u,
        .max_response_header_count = 8u, .max_response_header_bytes = 1024u,
        .max_response_body_bytes = 128u, .poll_slice_ms = 1u,
        .enable_http2 = 1, .h2_stream_capacity = 2u,
        .h2_input_buffer_bytes = SOCKET_TEST_WIRE_BYTES,
        .h2_output_buffer_bytes = SOCKET_TEST_WIRE_BYTES,
        .h2_hpack_dynamic_table_bytes = 4096u, .h2_max_settings_count = 16u};
    const chttp_server_websocket_options route = {
        .size = sizeof(route), .path = "/socket", .on_open = socket_test_open,
        .on_event = socket_test_event};
    chttp_websocket_connect_options options = {
        .size = sizeof(options), .protocol = CHTTP_HTTP_2, .timeout_ms = SOCKET_TEST_TIMEOUT_MS};
    chttp_websocket_session sessions[2] = {{0}};
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    uint64_t value = 0u;
    char uri[128];
    config.client.socket_options = (cnet_stream_socket_options)CNET_STREAM_SOCKET_OPTIONS_INIT;
    config.client.socket_options.nodelay = 1;
    config.client.socket_options.keepalive = 1;
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_websocket_with(&server, &route), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/socket", (unsigned int)port) > 0);
    options.uri = uri;
    check_equal(chttp_websocket_pool_init(&pool, &config), SALTS_OK);
    /* Initialization consumes the policy; the caller's config need not survive. */
    config.client.socket_options.nodelay = 0;
    config.client.socket_options.keepalive = 0;
    for (size_t index = 0u; index < 2u; ++index) {
      check_equal(chttp_websocket_pool_open(&pool, &options, &sessions[index], &http_status), SALTS_OK);
      check_equal(http_status, 200u);
      chttp_websocket_pool_impl *impl = (chttp_websocket_pool_impl *)pool.impl;
      check_equal(cnet_connection_tcp_option_get(&impl->network, impl->connection,
                      CNET_TCP_SOCKET_NODELAY, &value), SALTS_OK);
      check_equal(value, UINT64_C(1));
      check_equal(cnet_connection_tcp_option_get(&impl->network, impl->connection,
                      CNET_TCP_SOCKET_KEEPALIVE_ENABLED, &value), SALTS_OK);
      check_equal(value, UINT64_C(1));
    }
  }
}
