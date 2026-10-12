#include <http_client/rpc_service.h>
#include <type_traits>

static_assert(std::is_standard_layout_v<chttp_client_rpc_limits>);
static_assert(std::is_standard_layout_v<chttp_client_rpc_call_options>);
static_assert(std::is_standard_layout_v<chttp_client_rpc_outcome>);

int main() {
  chttp_client_rpc_outcome outcome = CHTTP_CLIENT_RPC_OUTCOME_INIT;
  crpc_error error = {};
  return chttp_client_rpc_call(nullptr, nullptr, &outcome, &error) ==
                 SALTS_EINVAL ? 0 : 1;
}
