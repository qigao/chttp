#if defined(CONSUME_CHTTP)
  #include <chttp/chttp.h>

int main(void) {
  chttp_client client = {0};
  chttp_tls_profile tls_profile = {0};
  chttp_server server = {0};
  chttp_session session = {0};
  chttp_websocket websocket = {0};
  chttp_websocket_client websocket_client = {0};
  chttp_websocket_pool websocket_pool = {0};
  chttp_websocket_session websocket_session = {0};
  chttp_response response = {0};
  chttp_options options = {0};
  chttp_client_config config = {0};
  chttp_server_config server_config = {0};
  chttp_body_source body_source = {0};
  chttp_body_sink body_sink = {0};
  chttp_websocket_client_config websocket_config = {0};
  chttp_websocket_connect_options websocket_options = {0};
  chttp_websocket_pool_config websocket_pool_config = {0};
  int (*response_source_fn)(chttp_server_response *, unsigned int, const char *,
                            const chttp_body_source *) = chttp_server_response_source;
  int (*response_file_fn)(chttp_server_response *, unsigned int, const char *, const char *) =
      chttp_server_response_file;
  int (*deferred_cancel_fn)(chttp_server_deferred *) = chttp_server_deferred_cancel;
  config.h2_input_buffer_bytes = 64u * 1024u;
  server_config.enable_http2 = 1;
  server_config.h2_stream_capacity = 32u;
  websocket_config.h2_input_buffer_bytes = 128u * 1024u;
  websocket_config.h2_hpack_dynamic_table_bytes = 4096u;
  websocket_config.h2_max_settings_count = 16u;
  (void)response_source_fn;
  (void)response_file_fn;
  (void)deferred_cancel_fn;
  chttp_response_destroy(&response);
  return chttp_server_response_source(NULL, 0u, NULL, NULL) == SALTS_EINVAL &&
                 chttp_server_response_file(NULL, 0u, NULL, NULL) == SALTS_EINVAL &&
                 chttp_post_file(NULL, NULL, NULL, NULL, NULL, NULL, NULL) == SALTS_EINVAL &&
                 chttp_put_file(NULL, NULL, NULL, NULL, NULL, NULL, NULL) == SALTS_EINVAL &&
                 chttp_download_file(NULL, NULL, NULL, NULL, NULL, NULL, NULL) == SALTS_EINVAL &&
                 chttp_server_websocket_with(&server, NULL) == SALTS_EINVAL &&
                 chttp_websocket_state_get(&websocket, NULL) == SALTS_EINVAL &&
                 chttp_websocket_client_destroy(&websocket_client, 0u) == SALTS_OK &&
                 chttp_websocket_pool_destroy(&websocket_pool, 0u) == SALTS_OK &&
                 chttp_client_destroy(&client, 0u) == SALTS_OK &&
                 chttp_tls_profile_destroy(&tls_profile) == SALTS_OK &&
                 chttp_server_destroy(&server) == SALTS_OK && session.impl == NULL &&
                 options.protocol == CHTTP_HTTP_1_1 && CHTTP_HTTP_2 != CHTTP_HTTP_1_1 &&
                 config.h2_input_buffer_bytes == 64u * 1024u && server_config.enable_http2 == 1 &&
                 server_config.h2_stream_capacity == 32u && body_source.read == NULL &&
                 body_sink.write == NULL && websocket_config.size == 0u &&
                 websocket_config.h2_input_buffer_bytes == 128u * 1024u &&
                 websocket_config.h2_hpack_dynamic_table_bytes == 4096u &&
                 websocket_config.h2_max_settings_count == 16u && websocket_options.size == 0u &&
                 websocket_options.protocol == CHTTP_HTTP_1_1 && websocket_pool.impl == NULL &&
                 websocket_pool_config.session_capacity == 0u && websocket_session.slot == 0u &&
                 websocket_session.generation == 0u && CHTTP_METHOD_CONNECT != CHTTP_METHOD_OPTIONS
             ? 0
             : 1;
}

#elif defined(CONSUME_CRPC)
  #include <crpc/crpc.h>

int main(void) {
  crpc_client client = {0};
  crpc_async_client async_client = {0};
  crpc_request request = {0};
  crpc_response response = {0};
  crpc_options options = {0};
  crpc_server server = {0};
  crpc_response_destroy(&response);
  return client.impl == NULL && async_client.impl == NULL && request.slot == 0u &&
                 request.generation == 0u && options.tls == NULL &&
                 options.protocol == CHTTP_HTTP_1_1 && server.impl == NULL &&
                 crpc_server_destroy(&server) == SALTS_OK
             ? 0
             : 1;
}

#elif defined(CONSUME_S3)
  #include <s3/s3.h>
  #include <s3/s3_bucket.h>
  #include <s3/s3_bucket_config.h>
  #include <s3/s3_credentials.h>
  #include <s3/s3_multipart.h>
  #include <s3/s3_object.h>
  #include <s3/s3_signer.h>

int main(void) {
  s3_client client = {0};
  s3_async_client async_client = {0};
  s3_response response = {0};
  s3_bucket_list buckets = {0};
  s3_object_list objects = {0};
  s3_multipart multipart = {0};
  s3_signer_result signature = {0};
  s3_response_destroy(&response);
  s3_bucket_list_destroy(&buckets);
  s3_object_list_destroy(&objects);
  s3_signer_result_destroy(&signature);
  return client.impl == NULL && async_client.impl == NULL &&
                 s3_multipart_destroy(&multipart) == SALTS_OK && S3_MULTIPART_MAX_PARTS == 10000
             ? 0
             : 1;
}

#else
#error "Select an HTTPServices consumer contract"
#endif
