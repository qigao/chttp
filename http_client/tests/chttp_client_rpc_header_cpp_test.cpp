#include <http_client/rpc_service.h>
#include <type_traits>

static_assert(std::is_standard_layout_v<chttp_client_rpc_limits>);
static_assert(std::is_standard_layout_v<chttp_client_rpc_call_options>);
static_assert(std::is_standard_layout_v<chttp_client_rpc_outcome>);
static_assert(std::is_pointer_v<chttp_async_client_rpc_complete_fn>);
static_assert(std::is_standard_layout_v<chttp_connection_recovery_options>);
static_assert(std::is_standard_layout_v<chttp_retry_options>);

int main() {
  chttp_client_rpc_outcome outcome = CHTTP_CLIENT_RPC_OUTCOME_INIT;
  crpc_error error = {};
  chttp_request request{};
  const int blocking = chttp_client_rpc_call(
      nullptr, nullptr, &outcome, &error);
  const int async = chttp_async_client_rpc_call(
      nullptr, nullptr, nullptr, nullptr, &request);
  const chttp_connection_recovery_options recovery = CHTTP_CONNECTION_RECOVERY_OPTIONS_INIT;
  const int async_recovery = chttp_async_client_set_connection_recovery(nullptr, &recovery);
  const int blocking_recovery = chttp_client_set_connection_recovery(nullptr, &recovery);
  const chttp_retry_options retry = CHTTP_RETRY_OPTIONS_INIT;
  chttp_response response{};
  chttp_error http_error{};
  const int logical_retry = chttp_request_retry(nullptr, CHTTP_METHOD_GET,
      nullptr, &retry, &response, &http_error);
  return blocking == SALTS_EINVAL && async == SALTS_EINVAL &&
      async_recovery == SALTS_EINVAL && blocking_recovery == SALTS_EINVAL &&
      logical_retry == SALTS_EINVAL ? 0 : 1;
}
