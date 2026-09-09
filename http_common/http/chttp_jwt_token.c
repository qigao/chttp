#include <http_common/http.h>
#include <cjwt/cjwt.h>

#include "chttp_jwt_policy.h"

#include <stdlib.h>

static int chttp_jwt_encode_status(cjwt_code_t status) {
  if (status == CJWTE_OK) return SALTS_OK;
  return status == CJWTE_OUT_OF_MEMORY ? SALTS_ENOMEM : SALTS_EINVAL;
}

int chttp_jwt_hs256_token_create(const chttp_jwt_claims *claims, const void *key, size_t key_size,
                                  char **out_token) {
  cjwt_t jwt;
  char *audience[1];
  int64_t issued_at;
  int64_t not_before;
  int64_t expires_at;
  if (claims == NULL || key == NULL || key_size < CHTTP_JWT_HS256_MIN_KEY_BYTES ||
      out_token == NULL)
    return SALTS_EINVAL;
  *out_token = NULL;
  audience[0] = (char *)claims->audience;
  issued_at = claims->issued_at;
  not_before = claims->not_before;
  expires_at = claims->expires_at;
  jwt = (cjwt_t){
      .header = {.alg = alg_hs256},
      .iss = (char *)claims->issuer,
      .sub = (char *)claims->subject,
      .jti = (char *)claims->jwt_id,
      .aud = {.count = claims->audience == NULL ? 0 : 1, .names = audience},
      .iat = claims->issued_at == 0 ? NULL : &issued_at,
      .nbf = claims->not_before == 0 ? NULL : &not_before,
      .exp = claims->expires_at == 0 ? NULL : &expires_at,
  };
  return chttp_jwt_encode_status(
      cjwt_encode(&jwt, (const uint8_t *)key, key_size, out_token));
}

void chttp_jwt_token_destroy(char *token) { free(token); }
