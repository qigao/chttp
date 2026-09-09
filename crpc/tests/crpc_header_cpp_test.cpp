#include <crpc/crpc.h>

#include <type_traits>

static_assert(std::is_standard_layout<crpc_client>::value,
              "request/reply client must be C ABI data");
static_assert(std::is_standard_layout<crpc_async_client>::value, "async client must be C ABI data");
static_assert(std::is_standard_layout<crpc_response>::value, "owning response must be C ABI data");
static_assert(std::is_standard_layout<crpc_response_view>::value,
              "response view must be C ABI data");
static_assert(std::is_standard_layout<crpc_options>::value, "call options must be C ABI data");
static_assert(std::is_standard_layout<crpc_server>::value, "server must be C ABI data");
static_assert(std::is_standard_layout<crpc_server_request_view>::value,
              "server request view must be C ABI data");
static_assert(std::is_standard_layout<crpc_server_response>::value,
              "server response must be C ABI data");

int main() {
  crpc_client client = {};
  crpc_async_client async_client = {};
  crpc_request request = {};
  crpc_options options = {};
  crpc_server server = {};
  return client.impl == nullptr && async_client.impl == nullptr && request.slot == 0u &&
                 options.tls == nullptr && options.protocol == CHTTP_HTTP_1_1 &&
                 server.impl == nullptr
             ? 0
             : 1;
}
