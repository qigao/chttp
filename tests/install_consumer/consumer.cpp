#if defined(CONSUME_S3_CPP)

  #include <s3/s3.h>
  #include <s3/s3_bucket.h>
  #include <s3/s3_bucket_config.h>
  #include <s3/s3_credentials.h>
  #include <s3/s3_multipart.h>
  #include <s3/s3_object.h>
  #include <s3/s3_signer.h>

int main() {
  s3_client client{};
  s3_async_client async_client{};
  s3_response response{};
  s3_bucket_list buckets{};
  s3_object_list objects{};
  s3_multipart multipart{};
  s3_response_destroy(&response);
  s3_bucket_list_destroy(&buckets);
  s3_object_list_destroy(&objects);
  return client.impl == nullptr && async_client.impl == nullptr &&
                 s3_multipart_destroy(&multipart) == SALTS_OK
             ? 0
             : 1;
}

#elif defined(CONSUME_NETWORK_CPP)

  #include <cnet/cnet.h>
  #include <cnet/websocket.h>
  #include <crpc/crpc.h>
  #include <cstring>

struct InstalledWebSocketProbe {
  uint8_t frame[CNET_WEBSOCKET_MAX_HEADER_BYTES + CNET_WEBSOCKET_MIN_FRAME_BYTES]{};
  size_t frame_size{};
  size_t event_count{};
};

static int installed_websocket_write(void *user, const uint8_t *data, size_t size) {
  auto *probe = static_cast<InstalledWebSocketProbe *>(user);
  if (probe == nullptr || data == nullptr || size > sizeof(probe->frame)) return SALTS_ENOSPC;
  std::memcpy(probe->frame, data, size);
  probe->frame_size = size;
  return SALTS_OK;
}

static void installed_websocket_event(void *user, cnet_websocket *,
                                      const cnet_websocket_event *event) {
  auto *probe = static_cast<InstalledWebSocketProbe *>(user);
  if (probe != nullptr && event != nullptr) ++probe->event_count;
}

int main() {
  static const uint8_t inbound_text[] = {0x81u, 0x01u, 'x'};
  crpc_client client{};
  crpc_async_client async_client{};
  crpc_request request{};
  crpc_options rpc_options{};
  crpc_server rpc_server{};
  chttp_server server{};
  chttp_session session{};
  chttp_websocket http_websocket{};
  chttp_websocket_client http_websocket_client{};
  chttp_websocket_pool http_websocket_pool{};
  chttp_websocket_session http_websocket_session{};
  chttp_tls_profile http_tls_profile{};
  chttp_options http_options{};
  chttp_client_config http_config{};
  chttp_server_config http_server_config{};
  chttp_body_source http_body_source{};
  chttp_body_sink http_body_sink{};
  chttp_websocket_client_config http_websocket_config{};
  chttp_websocket_connect_options http_websocket_options{};
  chttp_websocket_pool_config http_websocket_pool_config{};
  auto *http_response_source = &chttp_server_response_source;
  auto *http_response_file = &chttp_server_response_file;
  auto *http_deferred_cancel = &chttp_server_deferred_cancel;
  cnet_tls_client tls_client{};
  cnet_tls_server tls_server{};
  cnet_tls_client_config tls_client_config{};
  cnet_websocket websocket{};
  InstalledWebSocketProbe probe{};
  cnet_websocket_config websocket_config{};
  http_config.h2_input_buffer_bytes = 64u * 1024u;
  http_server_config.enable_http2 = 1;
  http_server_config.h2_stream_capacity = 32u;
  http_websocket_config.h2_input_buffer_bytes = 128u * 1024u;
  http_websocket_config.h2_hpack_dynamic_table_bytes = 4096u;
  http_websocket_config.h2_max_settings_count = 16u;
  (void)http_response_source;
  (void)http_response_file;
  (void)http_deferred_cancel;

  if (chttp_server_response_source(nullptr, 0u, nullptr, nullptr) != SALTS_EINVAL ||
      chttp_server_response_file(nullptr, 0u, nullptr, nullptr) != SALTS_EINVAL ||
      chttp_post_file(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) !=
          SALTS_EINVAL ||
      chttp_put_file(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) !=
          SALTS_EINVAL ||
      chttp_download_file(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) !=
          SALTS_EINVAL ||
      chttp_server_websocket_with(&server, nullptr) != SALTS_EINVAL ||
      chttp_websocket_state_get(&http_websocket, nullptr) != SALTS_EINVAL ||
      chttp_websocket_client_destroy(&http_websocket_client, 0u) != SALTS_OK ||
      chttp_websocket_pool_destroy(&http_websocket_pool, 0u) != SALTS_OK ||
      client.impl != nullptr || async_client.impl != nullptr || request.slot != 0u ||
      request.generation != 0u || rpc_options.tls != nullptr ||
      rpc_options.protocol != CHTTP_HTTP_1_1 || rpc_server.impl != nullptr ||
      crpc_server_destroy(&rpc_server) != SALTS_OK || server.impl != nullptr ||
      session.impl != nullptr || http_tls_profile.impl != nullptr ||
      http_options.protocol != CHTTP_HTTP_1_1 || CHTTP_HTTP_2 == CHTTP_HTTP_1_1 ||
      http_config.h2_input_buffer_bytes != 64u * 1024u || http_server_config.enable_http2 != 1 ||
      http_server_config.h2_stream_capacity != 32u || http_body_source.read != nullptr ||
      http_body_sink.write != nullptr || http_websocket_config.size != 0u ||
      http_websocket_config.h2_input_buffer_bytes != 128u * 1024u ||
      http_websocket_config.h2_hpack_dynamic_table_bytes != 4096u ||
      http_websocket_config.h2_max_settings_count != 16u || http_websocket_options.size != 0u ||
      http_websocket_options.protocol != CHTTP_HTTP_1_1 || http_websocket_pool.impl != nullptr ||
      http_websocket_pool_config.session_capacity != 0u || http_websocket_session.slot != 0u ||
      http_websocket_session.generation != 0u || CHTTP_METHOD_CONNECT == CHTTP_METHOD_OPTIONS ||
      tls_client.impl != nullptr || tls_server.impl != nullptr || tls_client_config.size != 0u ||
      websocket.impl != nullptr)
    return 1;
  websocket_config.size = sizeof(websocket_config);
  websocket_config.role = CNET_WEBSOCKET_CLIENT;
  websocket_config.max_frame_bytes = CNET_WEBSOCKET_MIN_FRAME_BYTES;
  websocket_config.max_message_bytes = CNET_WEBSOCKET_MIN_FRAME_BYTES;
  websocket_config.max_buffered_input_bytes =
      CNET_WEBSOCKET_MAX_HEADER_BYTES + CNET_WEBSOCKET_MIN_FRAME_BYTES;
  websocket_config.write = installed_websocket_write;
  websocket_config.on_event = installed_websocket_event;
  websocket_config.user = &probe;
  if (cnet_websocket_init(&websocket, &websocket_config) != SALTS_OK) return 2;
  if (cnet_websocket_send_text(&websocket, "x", 1u) != SALTS_OK || probe.frame_size == 0u) {
    (void)cnet_websocket_destroy(&websocket);
    return 3;
  }
  if (cnet_websocket_feed(&websocket, inbound_text, sizeof(inbound_text)) != SALTS_OK ||
      probe.event_count != 1u) {
    (void)cnet_websocket_destroy(&websocket);
    return 4;
  }
  return cnet_websocket_destroy(&websocket) == SALTS_OK ? 0 : 5;
}

#else
#error "Select an HTTPServices C++ consumer contract"
#endif
