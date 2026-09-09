#include "chttp_rate_limit_internal.h"
#include <cstl/typed.h>
#include <salts/clock.h>
#include <stdlib.h>
#include <string.h>

typedef struct chttp_rate_bucket {
  uint64_t credit;
  uint64_t updated_ms;
  size_t key_size;
  unsigned char key[CHTTP_RATE_LIMIT_KEY_BYTES];
} chttp_rate_bucket;

static bool rate_bucket_copy(void *destination, const void *source) {
  *(chttp_rate_bucket *)destination = *(const chttp_rate_bucket *)source;
  return true;
}
static void rate_bucket_move(void *destination, void *source) {
  *(chttp_rate_bucket *)destination = *(chttp_rate_bucket *)source;
}
static void rate_bucket_destroy(void *value) {
  memset(value, 0, sizeof(chttp_rate_bucket));
}
static const cmeta_type_traits rate_bucket_traits = {
    CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY |
    CMETA_TRAIT_TRIVIAL_COPY, NULL, NULL, NULL,
    rate_bucket_copy, rate_bucket_move, rate_bucket_destroy};
static const cmeta_type_desc rate_bucket_type = {
    "chttp_rate_bucket", sizeof(chttp_rate_bucket), _Alignof(chttp_rate_bucket),
    CMETA_T_OBJECT, NULL, &rate_bucket_traits};
/* Local container metadata only; the already-included callable ABI is unchanged. */
#undef CMETA_KNOWN_TYPE_LIST
#define CMETA_KNOWN_TYPE_LIST CMETA_BUILTIN_TYPE_LIST, \
    (rate_bucket, chttp_rate_bucket, rate_bucket_type, CMETA_T_OBJECT, rate_bucket_traits)
typed(Vec, chttp_rate_buckets, chttp_rate_bucket);

typedef struct chttp_rate_limit_impl {
  chttp_rate_limit_config config;
  chttp_rate_buckets buckets;
  uint64_t last_ms;
  uint64_t maximum_credit;
  size_t used;
} chttp_rate_limit_impl;

enum { RATE_MS_PER_SECOND = 1000, RATE_IPV4_BYTES = 4, RATE_IPV6_BYTES = 16,
       RATE_LIMITED = 429, RATE_CAPACITY = 503, RATE_NO_SUBJECT = 403, RATE_KEY_TOO_LONG = 431 };

int chttp_rate_limiter_init(chttp_rate_limiter *limiter, const chttp_rate_limit_config *config) {
  if (limiter == NULL || config == NULL) return SALTS_EINVAL;
  if (limiter->impl != NULL) return SALTS_EALREADY;
  if (config->scope < CHTTP_RATE_LIMIT_GLOBAL || config->scope > CHTTP_RATE_LIMIT_JWT_SUBJECT ||
      config->group_capacity == 0 || config->burst == 0 || config->refill_tokens == 0 ||
      config->refill_period_ms == 0 ||
      (config->scope == CHTTP_RATE_LIMIT_GLOBAL && config->group_capacity != 1) ||
      (config->scope == CHTTP_RATE_LIMIT_JWT_SUBJECT &&
       (config->key_bytes < 2 || config->key_bytes > CHTTP_RATE_LIMIT_KEY_BYTES))) return SALTS_EINVAL;
  if (config->group_capacity > SIZE_MAX / sizeof(chttp_rate_bucket)) return SALTS_ERANGE;
  chttp_rate_limit_impl *impl = calloc(1, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->config = *config;
  impl->maximum_credit = (uint64_t)config->burst * config->refill_period_ms;
  if (chttp_rate_buckets_init(&impl->buckets, config->group_capacity) != STL_OK ||
      chttp_rate_buckets_reserve(&impl->buckets, config->group_capacity) != STL_OK) {
    goto allocation_failed;
  }
  /* Vec push prepares an allocated copy: materialize every slot on the control plane. */
  const chttp_rate_bucket empty = {0};
  for (size_t i = 0; i < config->group_capacity; ++i) {
    if (chttp_rate_buckets_push(&impl->buckets, empty) != STL_OK) goto allocation_failed;
  }
  limiter->impl = impl;
  return SALTS_OK;
allocation_failed:
  chttp_rate_buckets_destroy(&impl->buckets);
  free(impl);
  return SALTS_ENOMEM;
}

void chttp_rate_limiter_destroy(chttp_rate_limiter *limiter) {
  if (limiter == NULL || limiter->impl == NULL) return;
  chttp_rate_limit_impl *impl = limiter->impl;
  chttp_rate_buckets_destroy(&impl->buckets);
  free(impl);
  limiter->impl = NULL;
}

static int rate_key(const chttp_rate_limit_config *config, const chttp_server_request_view *request,
                    chttp_rate_bucket *key, chttp_server_admission_result *result) {
  if (config->scope == CHTTP_RATE_LIMIT_GLOBAL) return SALTS_OK;
  if (config->scope == CHTTP_RATE_LIMIT_PEER_IP) {
    const cnet_stream_peer *peer = request->peer;
    if (peer == NULL) return SALTS_EINVAL;
    if (peer->family != CNET_DATAGRAM_ADDRESS_IPV4 && peer->family != CNET_DATAGRAM_ADDRESS_IPV6)
      return SALTS_EINVAL;
    key->key[0] = (unsigned char)peer->family;
    key->key_size = 1 + (peer->family == CNET_DATAGRAM_ADDRESS_IPV4 ? RATE_IPV4_BYTES : RATE_IPV6_BYTES);
    memcpy(key->key + 1, peer->address, key->key_size - 1);
    if (peer->family == CNET_DATAGRAM_ADDRESS_IPV6) {
      memcpy(key->key + key->key_size, &peer->scope_id, sizeof(peer->scope_id));
      key->key_size += sizeof(peer->scope_id);
    }
    return SALTS_OK;
  }
  const chttp_jwt_claims_view *claims = request->jwt_claims;
  if (claims == NULL || claims->subject == NULL || claims->subject[0] == '\0') {
    result->status_code = RATE_NO_SUBJECT;
    return SALTS_OK;
  }
  const char *parts[] = {claims->issuer == NULL ? "" : claims->issuer, claims->subject};
  for (size_t part = 0; part < sizeof(parts) / sizeof(parts[0]); ++part) {
    const size_t remaining = config->key_bytes - key->key_size;
    const size_t length = strnlen(parts[part], remaining);
    if (length == remaining) {
      result->status_code = RATE_KEY_TOO_LONG;
      return SALTS_OK;
    }
    memcpy(key->key + key->key_size, parts[part], length + 1);
    key->key_size += length + 1;
  }
  return SALTS_OK;
}

static uint64_t rate_ceil_div(uint64_t value, uint64_t divisor) {
  return value / divisor + (value % divisor != 0);
}

static void rate_refill(chttp_rate_limit_impl *impl, chttp_rate_bucket *bucket, uint64_t now_ms) {
  const uint64_t missing = impl->maximum_credit - bucket->credit;
  const uint64_t elapsed = now_ms - bucket->updated_ms;
  /* Compare before multiplication: elapsed may span the entire uint64 clock range. */
  if (elapsed >= rate_ceil_div(missing, impl->config.refill_tokens))
    bucket->credit = impl->maximum_credit;
  else
    bucket->credit += elapsed * impl->config.refill_tokens;
  bucket->updated_ms = now_ms;
}

int chttp_rate_limiter_admit_at(chttp_rate_limiter *limiter, const chttp_server_request_view *request,
                              chttp_server_admission_result *result, uint64_t now_ms) {
  if (result == NULL) return SALTS_EINVAL;
  memset(result, 0, sizeof(*result));
  if (limiter == NULL || limiter->impl == NULL || request == NULL) return SALTS_EINVAL;
  chttp_rate_limit_impl *impl = limiter->impl;
  if (now_ms < impl->last_ms) return SALTS_EINVAL;
  chttp_rate_bucket key = {0};
  const int status = rate_key(&impl->config, request, &key, result);
  if (status != SALTS_OK || result->status_code != 0) return status;
  impl->last_ms = now_ms;
  chttp_rate_bucket *selected = NULL;
  chttp_rate_bucket *reusable = NULL;
  const size_t count = impl->used;
  for (size_t i = 0; i < count; ++i) {
    chttp_rate_bucket *bucket = chttp_rate_buckets_at(&impl->buckets, i);
    rate_refill(impl, bucket, now_ms);
    if (bucket->key_size == key.key_size && memcmp(bucket->key, key.key, key.key_size) == 0) {
      selected = bucket;
      break;
    }
    if (bucket->credit == impl->maximum_credit) reusable = bucket;
  }
  if (selected == NULL) {
    key.credit = impl->maximum_credit;
    key.updated_ms = now_ms;
    if (reusable != NULL) {
      *reusable = key;
      selected = reusable;
    } else if (count < impl->config.group_capacity) {
      selected = chttp_rate_buckets_at(&impl->buckets, count);
      *selected = key;
      ++impl->used;
    } else {
      result->status_code = RATE_CAPACITY;
      return SALTS_OK;
    }
  }
  if (selected->credit < impl->config.refill_period_ms) {
    const uint64_t delay_ms = rate_ceil_div(impl->config.refill_period_ms - selected->credit,
                                          impl->config.refill_tokens);
    result->status_code = RATE_LIMITED;
    result->retry_after_seconds = (uint32_t)rate_ceil_div(delay_ms, RATE_MS_PER_SECOND);
  } else {
    selected->credit -= impl->config.refill_period_ms;
  }
  return SALTS_OK;
}

int chttp_rate_limiter_admit(void *limiter, const chttp_server_request_view *request,
                           chttp_server_admission_result *result) {
  return chttp_rate_limiter_admit_at(limiter, request, result, salts_monotonic_ms());
}
