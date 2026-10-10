#include "http_example.service_native.h"
#include <salts/error_codes.h>

/* The generated adapter and page handler call the same typed operation. */
int databind_11_HttpExample_10_Calculator_3_Add(
    const SumRequest_t *request, SumResponse_t *response) {
  if (request == NULL || response == NULL || request->left == 0u)
    return SALTS_EINVAL;
  response->sum = (uint64_t)request->left + request->right;
  return SALTS_OK;
}
