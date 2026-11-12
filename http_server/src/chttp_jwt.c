#include <http_server/http.h>

#include <cjwt/cjwt.h>
#include <core/tstr.h>
#include <core/vstr.h>
#include "chttp_digest.h"

#include "chttp_jwt_internal.h"
#include "chttp_jwt_policy.h"
#include "chttp_server_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  CHTTP_JWT_BEARER_SCHEME_SIZE = sizeof("Bearer") - 1u
};

typedef struct chttp_jwt_bearer_validator_impl {
  unsigned char *key;
  size_t key_size;
  int64_t clock_skew_seconds;
  int allow_missing_exp;
  tstr expected_issuer;
  tstr expected_audience;
} chttp_jwt_bearer_validator_impl;

static int chttp_jwt_ascii_equal_ci(const char *left, const char *right) {
  return left != NULL && right != NULL &&
         vstr_ieq(vstr_from_cstr(left), vstr_from_cstr(right));
}

static const char *chttp_jwt_bearer_token(const chttp_server_request_view *request) {
  const char *authorization = NULL;
  size_t authorization_size;
  size_t index;
  if (request == NULL || request->headers == NULL) return NULL;
  for (index = 0u; index < request->header_count; ++index) {
    const chttp_header *header = &request->headers[index];
    if (chttp_jwt_ascii_equal_ci(header->name, "Authorization")) {
      if (authorization != NULL || header->value == NULL) return NULL;
      authorization = header->value;
    }
  }
  if (authorization == NULL) return NULL;
  authorization_size = strlen(authorization);
  if (authorization_size <= CHTTP_JWT_BEARER_SCHEME_SIZE ||
      authorization[CHTTP_JWT_BEARER_SCHEME_SIZE] != ' ')
    return NULL;
  if (!vstr_ieq(vstr_from_buf(authorization, CHTTP_JWT_BEARER_SCHEME_SIZE),
                vstr_from_buf("Bearer", CHTTP_JWT_BEARER_SCHEME_SIZE)))
    return NULL;
  index = CHTTP_JWT_BEARER_SCHEME_SIZE;
  while (index < authorization_size && authorization[index] == ' ') ++index;
  return index == authorization_size ? NULL : authorization + index;
}

static int chttp_jwt_claims_match(const chttp_jwt_bearer_validator_impl *validator,
                                  const cjwt_t *jwt) {
  int index;
  if (validator->expected_issuer != NULL &&
      (jwt->iss == NULL || !vstr_eq(vstr_from_cstr(jwt->iss), tstr_to_v(validator->expected_issuer))))
    return 0;
  if (validator->expected_audience == NULL) return 1;
  for (index = 0; index < jwt->aud.count; ++index)
    if (jwt->aud.names[index] != NULL &&
        vstr_eq(vstr_from_cstr(jwt->aud.names[index]), tstr_to_v(validator->expected_audience)))
      return 1;
  return 0;
}

static int chttp_jwt_expired_strict(int64_t now_seconds, int64_t expires_at,
                                    int64_t clock_skew_seconds) {
  if (clock_skew_seconds > 0 && expires_at > INT64_MAX - clock_skew_seconds) return 0;
  return now_seconds >= expires_at + clock_skew_seconds;
}

int chttp_jwt_bearer_unauthorized_response(chttp_server_response *response) {
  int status = chttp_server_response_set_header(response, "WWW-Authenticate", "Bearer");
  if (status != SALTS_OK) return status;
  return chttp_server_reply(response, 401u, "text/plain", "Unauthorized", 12u);
}

void chttp_jwt_request_state_reset(chttp_server_request_state *state) {
  if (state == NULL) return;
  cjwt_destroy((cjwt_t *)state->jwt_owner);
  state->jwt_owner = NULL;
  state->jwt_claims = (chttp_jwt_claims_view){0};
}

int chttp_jwt_bearer_validator_init(chttp_jwt_bearer_validator *validator,
                                    const chttp_jwt_bearer_validator_options *options) {
  chttp_jwt_bearer_validator_impl *impl;
  if (validator == NULL || options == NULL || options->size != sizeof(*options) ||
      options->key == NULL || options->key_size < CHTTP_JWT_HS256_MIN_KEY_BYTES ||
      options->clock_skew_seconds < 0)
    return SALTS_EINVAL;
  if (validator->impl != NULL) return SALTS_EALREADY;
  if ((options->expected_issuer != NULL && options->expected_issuer[0] == '\0') ||
      (options->expected_audience != NULL && options->expected_audience[0] == '\0'))
    return SALTS_EINVAL;
  impl = (chttp_jwt_bearer_validator_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->key = (unsigned char *)malloc(options->key_size);
  impl->key_size = options->key_size;
  impl->clock_skew_seconds = options->clock_skew_seconds;
  impl->allow_missing_exp = options->allow_missing_exp != 0;
  /* NULL disables a restriction; tstr_dup(NULL) would instead create an empty string. */
  impl->expected_issuer = options->expected_issuer != NULL ? tstr_dup(options->expected_issuer) : NULL;
  impl->expected_audience =
      options->expected_audience != NULL ? tstr_dup(options->expected_audience) : NULL;
  if (impl->key == NULL || (options->expected_issuer != NULL && impl->expected_issuer == NULL) ||
      (options->expected_audience != NULL && impl->expected_audience == NULL)) {
    if (impl->key != NULL) chttp_crypto_clear(impl->key, impl->key_size);
    free(impl->key);
    tstr_free(impl->expected_issuer);
    tstr_free(impl->expected_audience);
    free(impl);
    return SALTS_ENOMEM;
  }
  memcpy(impl->key, options->key, impl->key_size);
  validator->impl = impl;
  return SALTS_OK;
}

int chttp_jwt_bearer_validator_destroy(chttp_jwt_bearer_validator *validator) {
  chttp_jwt_bearer_validator_impl *impl;
  if (validator == NULL) return SALTS_EINVAL;
  impl = (chttp_jwt_bearer_validator_impl *)validator->impl;
  if (impl != NULL) {
    chttp_crypto_clear(impl->key, impl->key_size);
    free(impl->key);
    tstr_free(impl->expected_issuer);
    tstr_free(impl->expected_audience);
    free(impl);
  }
  validator->impl = NULL;
  return SALTS_OK;
}

int chttp_jwt_bearer_request_validate_at(chttp_server_request_state *state,
                                         const chttp_server_request_view *request,
                                         chttp_jwt_bearer_validator *handle,
                                         int64_t now_seconds) {
  chttp_jwt_bearer_validator_impl *validator;
  const char *token;
  cjwt_t *jwt = NULL;
  if (handle == NULL || request == NULL || state == NULL) return SALTS_EINVAL;
  validator = (chttp_jwt_bearer_validator_impl *)handle->impl;
  if (validator == NULL) return SALTS_EINVAL;
  token = chttp_jwt_bearer_token(request);
  if (token == NULL) return SALTS_EPERM;
  if (cjwt_decode(token, strlen(token), OPT_ALLOW_ONLY_HS_ALG, validator->key, validator->key_size,
                  now_seconds, validator->clock_skew_seconds, &jwt) != CJWTE_OK ||
      jwt == NULL || jwt->header.alg != alg_hs256 || !chttp_jwt_claims_match(validator, jwt)) {
    cjwt_destroy(jwt);
    return SALTS_EPERM;
  }
  if ((!validator->allow_missing_exp && jwt->exp == NULL) ||
      (jwt->exp != NULL && chttp_jwt_expired_strict(now_seconds, *jwt->exp,
                                                    validator->clock_skew_seconds))) {
    cjwt_destroy(jwt);
    return SALTS_EPERM;
  }
  chttp_jwt_request_state_reset(state);
  state->jwt_owner = jwt;
  state->jwt_claims = (chttp_jwt_claims_view){.issuer = jwt->iss,
                                               .subject = jwt->sub,
                                               .jwt_id = jwt->jti,
                                               .audiences = (const char *const *)jwt->aud.names,
                                               .audience_count = (size_t)jwt->aud.count,
                                               .issued_at = jwt->iat,
                                               .not_before = jwt->nbf,
                                               .expires_at = jwt->exp};
  return SALTS_OK;
}

int chttp_jwt_bearer_request_validate(chttp_server_request_state *state,
                                      const chttp_server_request_view *request,
                                      chttp_jwt_bearer_validator *handle) {
  const time_t now = time(NULL);
  if (now == (time_t)-1) return SALTS_EPERM;
  return chttp_jwt_bearer_request_validate_at(state, request, handle, (int64_t)now);
}
