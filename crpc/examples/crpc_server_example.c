#include <crpc/crpc.h>

#include <stdio.h>
#include <stdlib.h>

enum {
  EXAMPLE_TIMEOUT_MS = 5000,
  EXAMPLE_BUFFER_BYTES = 64u * 1024u,
};

static native_io_backend_kind example_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static crpc_server_config example_config(void) {
  crpc_server_config config = {0};
  config.http.host = "127.0.0.1";
  config.http.backlog = 8u;
  config.http.network.backend = example_backend();
  config.http.network.connection_capacity = 8u;
  config.http.network.command_capacity = 32u;
  config.http.network.request_capacity = 16u;
  config.http.network.completion_batch_capacity = 8u;
  config.http.network.event_capacity = 32u;
  config.http.network.max_send_bytes = EXAMPLE_BUFFER_BYTES;
  config.http.network.receive_buffer_bytes = 4096u;
  config.http.network.connect_timeout_ms = EXAMPLE_TIMEOUT_MS;
  config.http.network.read_timeout_ms = EXAMPLE_TIMEOUT_MS;
  config.http.network.write_timeout_ms = EXAMPLE_TIMEOUT_MS;
  config.http.route_capacity = 4u;
  config.http.middleware_capacity = 4u;
  config.http.max_route_middleware_count = 4u;
  config.http.max_route_param_count = 4u;
  config.http.max_route_param_bytes = 128u;
  config.http.max_target_bytes = 256u;
  config.http.max_header_count = 16u;
  config.http.max_header_bytes = 4096u;
  config.http.max_request_body_bytes = 8192u;
  config.http.max_response_header_count = 16u;
  config.http.max_response_header_bytes = 4096u;
  config.http.max_response_body_bytes = 8192u;
  config.http.poll_slice_ms = 2u;
  config.http.max_buffered_response_body_bytes = 8192u;
  config.method_capacity = 8u;
  config.max_method_bytes = 64u;
  config.max_json_depth = 8u;
  config.max_batch_items = 4u;
  return config;
}

static int example_ping(void *user, const crpc_server_request_view *request,
                        crpc_server_response *response) {
  (void)user;
  (void)request;
  return crpc_server_response_result(response, NULL, NULL);
}

int main(void) {
  crpc_server server = {0};
  crpc_server_config config = example_config();
  crpc_method method = {.service = "example", .name = "ping"};
  uint16_t port = 0u;
  int status = crpc_server_init(&server, &config);

  if (status == SALTS_OK)
    status = crpc_server_register(&server, "/rpc", &method, example_ping, NULL);
  if (status == SALTS_OK) status = crpc_server_start(&server);
  if (status == SALTS_OK) status = crpc_server_port(&server, &port);
  if (status == SALTS_OK) {
    printf("JSON-RPC endpoint: http://127.0.0.1:%u/rpc\n", (unsigned int)port);
    puts("Press Enter to stop.");
    (void)getchar();
  }
  if (server.impl != NULL) {
    const int stop_status = crpc_server_stop(&server, EXAMPLE_TIMEOUT_MS);
    if (status == SALTS_OK && stop_status != SALTS_OK) status = stop_status;
    if (stop_status == SALTS_OK) {
      const int destroy_status = crpc_server_destroy(&server);
      if (status == SALTS_OK && destroy_status != SALTS_OK) status = destroy_status;
    }
  }
  if (status != SALTS_OK) fprintf(stderr, "CRPC server failed: %d\n", status);
  return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
