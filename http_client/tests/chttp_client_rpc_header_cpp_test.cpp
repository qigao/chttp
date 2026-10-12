#include <http_client/rpc_service.h>
#include <type_traits>

static_assert(std::is_standard_layout_v<chttp_client_rpc_limits>);
static_assert(std::is_standard_layout_v<chttp_client_rpc_call_options>);
static_assert(std::is_standard_layout_v<chttp_client_rpc_outcome>);
static_assert(std::is_pointer_v<chttp_async_client_rpc_complete_fn>);

int main() {
  chttp_client_rpc_outcome outcome = CHTTP_CLIENT_RPC_OUTCOME_INIT;
  crpc_error error = {};
  chttp_request request{};
  const int blocking = chttp_client_rpc_call(
      nullptr, nullptr, &outcome, &error);
  const int async = chttp_async_client_rpc_call(
      nullptr, nullptr, nullptr, nullptr, &request);
  return blocking == SALTS_EINVAL && async == SALTS_EINVAL ? 0 : 1;
}
