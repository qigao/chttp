#include <chttp/chttp.h>

#include <type_traits>

static_assert(std::is_standard_layout<chttp_client>::value, "client handle must be C ABI data");
static_assert(std::is_standard_layout<chttp_async_client>::value,
              "async client handle must be C ABI data");
static_assert(std::is_standard_layout<chttp_request>::value, "request handle must be C ABI data");
static_assert(std::is_standard_layout<chttp_response>::value, "owning response must be C ABI data");
static_assert(std::is_standard_layout<chttp_response_view>::value,
              "response view must be C ABI data");
static_assert(std::is_standard_layout<chttp_server>::value, "server handle must be C ABI data");
static_assert(std::is_standard_layout<chttp_server_request_view>::value,
              "server request view must be C ABI data");
static_assert(std::is_standard_layout<chttp_server_response>::value,
              "server response builder must be C ABI data");
static_assert(std::is_standard_layout<chttp_server_deferred>::value,
              "deferred server handle must be C ABI data");
static_assert(std::is_standard_layout<chttp_server_deferred_response>::value,
              "deferred response input must be C ABI data");
static_assert(std::is_standard_layout<chttp_server_next>::value,
              "middleware continuation must be C ABI data");
static_assert(std::is_standard_layout<chttp_session>::value, "session handle must be C ABI data");
static_assert(std::is_standard_layout<chttp_tls_profile>::value, "TLS profile must be C ABI data");
static_assert(std::is_standard_layout<chttp_body_source>::value, "body source must be C ABI data");
static_assert(std::is_standard_layout<chttp_body_sink>::value, "body sink must be C ABI data");
static_assert(std::is_standard_layout<chttp_websocket>::value, "WebSocket peer must be C ABI data");
static_assert(std::is_standard_layout<chttp_server_websocket_session>::value,
              "server WebSocket session must be C ABI data");
static_assert(std::is_standard_layout<chttp_websocket_client>::value,
              "WebSocket client must be C ABI data");
static_assert(std::is_standard_layout<chttp_websocket_pool>::value,
              "WebSocket pool must be C ABI data");
static_assert(std::is_standard_layout<chttp_websocket_session>::value,
              "WebSocket session must be C ABI data");
static_assert(CHTTP_HTTP_1_1 == 0, "zero-initialized requests must remain HTTP/1.1");
static_assert(CHTTP_HTTP_2 != CHTTP_HTTP_1_1, "HTTP/2 must be an explicit protocol selection");

int main() {
  chttp_tls_profile tls_profile{};
  chttp_client client{};
  chttp_async_client async_client{};
  chttp_request request{};
  chttp_request_options async_options{};
  chttp_options options{};
  chttp_client_config client_config{};
  chttp_server server{};
  chttp_server_config server_config{};
  chttp_server_stats server_stats{};
  chttp_body_source body_source{};
  chttp_body_sink body_sink{};
  chttp_websocket_client websocket_client{};
  chttp_server_websocket_session server_websocket_session{};
  chttp_websocket_client_config websocket_config{};
  chttp_server_socket_options server_socket_options = CHTTP_SERVER_SOCKET_OPTIONS_INIT;
  chttp_websocket_connect_options websocket_options{};
  chttp_websocket_pool websocket_pool{};
  chttp_websocket_pool_config websocket_pool_config{};
  chttp_websocket_session websocket_session{};
  auto *response_source = &chttp_server_response_source;
  auto *response_file = &chttp_server_response_file;
  auto *response_defer = &chttp_server_response_defer;
  auto *deferred_reply = &chttp_server_deferred_reply;
  auto *deferred_cancel = &chttp_server_deferred_cancel;
  auto *post_file = &chttp_post_file;
  auto *put_file = &chttp_put_file;
  auto *download_file = &chttp_download_file;
  (void)tls_profile;
  (void)server_stats;
  (void)response_source;
  (void)response_file;
  (void)response_defer;
  (void)deferred_reply;
  (void)deferred_cancel;
  (void)post_file;
  (void)put_file;
  (void)download_file;
  client_config.h2_input_buffer_bytes = 64u * 1024u;
  client_config.h2_hpack_dynamic_table_bytes = 2048u;
  client_config.h2_max_settings_count = 16u;
  server_config.enable_http2 = 1;
  server_config.h2_stream_capacity = 32u;
  server_config.h2_input_buffer_bytes = 64u * 1024u;
  server_config.h2_output_buffer_bytes = 64u * 1024u;
  server_config.h2_hpack_dynamic_table_bytes = 2048u;
  server_config.h2_max_settings_count = 16u;
  websocket_config.h2_input_buffer_bytes = 128u * 1024u;
  websocket_config.h2_hpack_dynamic_table_bytes = 4096u;
  websocket_config.h2_max_settings_count = 16u;
  websocket_config.socket_options = CNET_STREAM_SOCKET_OPTIONS_INIT;
  server_socket_options.stream.keepalive = 1;
  return client.impl == nullptr && async_client.impl == nullptr && request.slot == 0u &&
                 request.generation == 0u && async_options.protocol == CHTTP_HTTP_1_1 &&
                 options.protocol == CHTTP_HTTP_1_1 &&
                 client_config.h2_input_buffer_bytes == 64u * 1024u &&
                 client_config.h2_hpack_dynamic_table_bytes == 2048u &&
                 client_config.h2_max_settings_count == 16u && server_config.enable_http2 == 1 &&
                 server_config.h2_stream_capacity == 32u &&
                 server_config.h2_output_buffer_bytes == 64u * 1024u &&
                 body_source.read == nullptr && body_sink.write == nullptr &&
                 server.impl == nullptr && websocket_client.impl == nullptr &&
                 server_websocket_session.impl == nullptr &&
                 websocket_config.h2_input_buffer_bytes == 128u * 1024u &&
                 server_socket_options.stream.keepalive == 1 &&
                 websocket_config.h2_hpack_dynamic_table_bytes == 4096u &&
                 websocket_config.h2_max_settings_count == 16u &&
                 websocket_options.protocol == CHTTP_HTTP_1_1 && websocket_pool.impl == nullptr &&
                 websocket_pool_config.session_capacity == 0u && websocket_session.slot == 0u &&
                 websocket_session.generation == 0u && CHTTP_METHOD_CONNECT != CHTTP_METHOD_OPTIONS
             ? 0
             : 1;
}
