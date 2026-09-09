#include "s3_test_support.h"

#include <stdio.h>

native_io_backend_kind s3_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

cnet_client_config s3_test_network_config(size_t connection_capacity) {
  const cnet_client_config config = {.backend = s3_test_backend(),
                                     .connection_capacity = connection_capacity,
                                     .command_capacity = 16u,
                                     .request_capacity = 16u,
                                     .completion_batch_capacity = 8u,
                                     .event_capacity = 32u,
                                     .max_send_bytes = 64u * 1024u,
                                     .receive_buffer_bytes = 4096u,
                                     .connect_timeout_ms = S3_TEST_TIMEOUT_MS,
                                     .read_timeout_ms = S3_TEST_TIMEOUT_MS,
                                     .write_timeout_ms = S3_TEST_TIMEOUT_MS};
  return config;
}

chttp_server_config s3_test_server_config(void) {
  const chttp_server_config config = {.host = "127.0.0.1",
                                      .port = 0u,
                                      .backlog = 8u,
                                      .network = s3_test_network_config(4u),
                                      .route_capacity = 16u,
                                      .middleware_capacity = 4u,
                                      .max_route_middleware_count = 4u,
                                      .max_route_param_count = 4u,
                                      .max_route_param_bytes = 128u,
                                      .max_target_bytes = 2048u,
                                      .max_header_count = 32u,
                                      .max_header_bytes = 16u * 1024u,
                                      .max_request_body_bytes = 1024u * 1024u,
                                      .max_response_header_count = 16u,
                                      .max_response_header_bytes = 4096u,
                                      .max_response_body_bytes = 1024u * 1024u,
                                      .poll_slice_ms = 2u,
                                      .enable_http2 = 1,
                                      .h2_stream_capacity = 8u,
                                      .h2_input_buffer_bytes = 64u * 1024u,
                                      .h2_output_buffer_bytes = 64u * 1024u,
                                      .h2_hpack_dynamic_table_bytes = 4096u,
                                      .h2_max_settings_count = 16u};
  return config;
}

chttp_client_config s3_test_client_config(void) {
  const chttp_client_config config = {.network = s3_test_network_config(4u),
                                      .request_capacity = 8u,
                                      .max_start_line_bytes = 2048u,
                                      .max_header_count = 32u,
                                      .max_header_bytes = 16u * 1024u,
                                      .max_request_body_bytes = 1024u * 1024u,
                                      .max_response_body_bytes = 1024u * 1024u,
                                      .max_informational_responses = 2u,
                                      .stream_chunk_bytes = 16u * 1024u,
                                      .h2_input_buffer_bytes = 64u * 1024u,
                                      .h2_hpack_dynamic_table_bytes = 4096u,
                                      .h2_max_settings_count = 16u};
  return config;
}

int s3_test_endpoint(uint16_t port, char *connection_uri, size_t uri_capacity, char *authority,
                     size_t authority_capacity) {
  const int uri_size =
      snprintf(connection_uri, uri_capacity, "tcp://127.0.0.1:%u", (unsigned int)port);
  const int authority_size =
      snprintf(authority, authority_capacity, "127.0.0.1:%u", (unsigned int)port);
  return uri_size > 0 && (size_t)uri_size < uri_capacity && authority_size > 0 &&
                 (size_t)authority_size < authority_capacity
             ? SALTS_OK
             : SALTS_EMSGSIZE;
}
