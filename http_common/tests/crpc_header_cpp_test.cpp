#include <http_client/rpc.h>
#include <http_server/rpc.h>

#include <type_traits>

static_assert(std::is_standard_layout<chttp_client>::value,
              "HTTP Client owns JSON-RPC transport");
static_assert(std::is_standard_layout<chttp_async_client>::value,
              "HTTP async Client owns JSON-RPC request slots");
static_assert(std::is_standard_layout<chttp_request>::value,
              "generation-checked request handle is an HTTP-owned C ABI value");
static_assert(std::is_standard_layout<crpc_response>::value,
              "owning JSON-RPC result remains a C ABI value");
static_assert(std::is_standard_layout<crpc_response_view>::value,
              "response view remains a C ABI value");
static_assert(std::is_standard_layout<crpc_options>::value,
              "RPC call options remain a C ABI value");
static_assert(std::is_standard_layout<crpc_server_request_view>::value,
              "callback-scoped server request remains a C ABI value");
static_assert(std::is_standard_layout<crpc_server_response>::value,
              "callback-scoped server response remains a C ABI value");
static_assert(std::is_standard_layout<crpc_server_param_reader>::value,
              "server params reader lease remains a C ABI value");
static_assert(std::is_standard_layout<chttp_server_rpc_config>::value,
              "RPC config is owned by the HTTP Server");

int main() {
  chttp_client client = {};
  chttp_async_client async_client = {};
  chttp_request request = {};
  crpc_options options = {};
  chttp_server server = {};
  chttp_server_rpc_config config = CHTTP_SERVER_RPC_CONFIG_INIT;
  crpc_server_param_reader param_reader = CRPC_SERVER_PARAM_READER_INIT;
  return client.impl == nullptr && async_client.impl == nullptr && request.slot == 0u &&
                 options.tls == nullptr && options.protocol == CHTTP_HTTP_1_1 &&
                 server.impl == nullptr && config.size == sizeof(config) &&
                 param_reader.reader == nullptr
             ? 0
             : 1;
}
