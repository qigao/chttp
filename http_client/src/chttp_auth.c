#include <http_client/http.h>

#include <string.h>

enum { CHTTP_JWT_BEARER_PREFIX_SIZE = sizeof("Bearer ") - 1u };

int chttp_jwt_bearer_header(const char *token, char *buffer, size_t buffer_size,
                             chttp_header *out_header) {
  const size_t token_size = token == NULL ? 0u : strlen(token);
  size_t required;
  if (token == NULL || token_size == 0u || buffer == NULL || out_header == NULL) return SALTS_EINVAL;
  if (token_size > SIZE_MAX - CHTTP_JWT_BEARER_PREFIX_SIZE - 1u) return SALTS_ERANGE;
  required = CHTTP_JWT_BEARER_PREFIX_SIZE + token_size + 1u;
  if (required > buffer_size) return SALTS_ENOBUFS;
  memcpy(buffer, "Bearer ", CHTTP_JWT_BEARER_PREFIX_SIZE);
  memcpy(buffer + CHTTP_JWT_BEARER_PREFIX_SIZE, token, token_size + 1u);
  *out_header = (chttp_header){.name = "Authorization", .value = buffer};
  return SALTS_OK;
}
