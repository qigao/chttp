#include <chttp/chttp.h>

#include <cjwt/cjwt.h>
#include <openssl/crypto.h>

#include "chttp_jwt_internal.h"
#include "chttp_server_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  CHTTP_JWT_HS256_MIN_KEY_BYTES = 32u,
  CHTTP_JWT_BEARER_SCHEME_SIZE = sizeof("Bearer") - 1u,
  CHTTP_JWT_BEARER_PREFIX_SIZE = sizeof("Bearer ") - 1u
};

typedef struct chttp_jwt_bearer_validator_impl {
  unsigned char *key;
  size_t key_size;
  int64_t clock_skew_seconds;
  int allow_missing_exp;
  char *expected_issuer;
  char *expected_audience;
} chttp_jwt_bearer_validator_impl;

static int chttp_jwt_encode_status(cjwt_code_t status) {
  if (status == CJWTE_OK) return SALTS_OK;
  return status == CJWTE_OUT_OF_MEMORY ? SALTS_ENOMEM : SALTS_EINVAL;
}

static char *chttp_jwt_string_copy(const char *value) {
  const size_t size = value == NULL ? 0u : strlen(value);
  char *copy;
  if (value == NULL) return NULL;
  if (size == SIZE_MAX) return NULL;
  copy = (char *)malloc(size + 1u);
  if (copy != NULL) memcpy(copy, value, size + 1u);
  return copy;
}

static int chttp_jwt_ascii_equal_ci(const char *left, const char *right) {
  size_t index = 0u;
  if (left == NULL || right == NULL) return 0;
  while (left[index] != '\0' && right[index] != '\0') {
    unsigned char left_char = (unsigned char)left[index];
    unsigned char right_char = (unsigned char)right[index];
    if (left_char >= 'A' && left_char <= 'Z') left_char = (unsigned char)(left_char + ('a' - 'A'));
    if (right_char >= 'A' && right_char <= 'Z')
      right_char = (unsigned char)(right_char + ('a' - 'A'));
    if (left_char != right_char) return 0;
    ++index;
  }
  return left[index] == right[index];
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
  for (index = 0u; index < CHTTP_JWT_BEARER_SCHEME_SIZE; ++index) {
    const unsigned char value = (unsigned char)authorization[index];
    const unsigned char expected = (unsigned char)"Bearer"[index];
    const unsigned char lower =
        value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
    const unsigned char expected_lower = expected >= 'A' && expected <= 'Z'
                                             ? (unsigned char)(expected + ('a' - 'A'))
                                             : expected;
    if (lower != expected_lower) return NULL;
  }
  index = CHTTP_JWT_BEARER_SCHEME_SIZE;
  while (index < authorization_size && authorization[index] == ' ') ++index;
  return index == authorization_size ? NULL : authorization + index;
}

static int chttp_jwt_claims_match(const chttp_jwt_bearer_validator_impl *validator,
                                  const cjwt_t *jwt) {
  int index;
  if (validator->expected_issuer != NULL &&
      (jwt->iss == NULL || strcmp(jwt->iss, validator->expected_issuer) != 0))
    return 0;
  if (validator->expected_audience == NULL) return 1;
  for (index = 0; index < jwt->aud.count; ++index)
    if (jwt->aud.names[index] != NULL && strcmp(jwt->aud.names[index], validator->expected_audience) == 0)
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
  impl->expected_issuer = chttp_jwt_string_copy(options->expected_issuer);
  impl->expected_audience = chttp_jwt_string_copy(options->expected_audience);
  if (impl->key == NULL || (options->expected_issuer != NULL && impl->expected_issuer == NULL) ||
      (options->expected_audience != NULL && impl->expected_audience == NULL)) {
    if (impl->key != NULL) OPENSSL_cleanse(impl->key, impl->key_size);
    free(impl->key);
    free(impl->expected_issuer);
    free(impl->expected_audience);
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
    OPENSSL_cleanse(impl->key, impl->key_size);
    free(impl->key);
    free(impl->expected_issuer);
    free(impl->expected_audience);
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
