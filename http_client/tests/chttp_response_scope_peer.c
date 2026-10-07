#include <http_client/response_scope.h>

const cmeta_type_desc *chttp_response_scope_peer_type(void) {
  return cmeta_reflected_storage(chttp_response);
}
