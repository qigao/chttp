#include "http_example_incompatible_native.h"
#include <salts/error_codes.h>

/* A real 32-bit provider whose operation ID matches the 64-bit host contract. */
int databind_11_HttpExample_10_Calculator_3_Add(
    const SumRequest_t *request, SumResponse_t *response) {
  if (request == NULL || response == NULL || request->left == 0u ||
      request->right > UINT32_MAX - request->left)
    return SALTS_EINVAL;
  response->sum = request->left + request->right;
  return SALTS_OK;
}
