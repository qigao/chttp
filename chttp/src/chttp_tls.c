#include "chttp_tls.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct chttp_tls_profile_impl {
  cnet_tls_client client;
  atomic_size_t references;
  chttp_protocol protocol;
};

static int chttp_tls_alpn_validate(const char *const *protocols, size_t count,
                                   chttp_protocol *out_protocol) {
  static const char http1[] = "http/1.1";
  static const char http2[] = "h2";
  size_t size;
  if (out_protocol == NULL) return SALTS_EINVAL;
  *out_protocol = CHTTP_HTTP_1_1;
  if (count == 0u) return protocols == NULL ? SALTS_OK : SALTS_EINVAL;
  if (protocols == NULL || count != 1u || protocols[0] == NULL) return SALTS_ENOTSUP;
  for (size = 0u; size <= CNET_TLS_ALPN_NAME_MAX_BYTES; ++size) {
    if (protocols[0][size] != '\0') continue;
    if (size == sizeof(http1) - 1u && memcmp(protocols[0], http1, size) == 0) return SALTS_OK;
    if (size == sizeof(http2) - 1u && memcmp(protocols[0], http2, size) == 0) {
      *out_protocol = CHTTP_HTTP_2;
      return SALTS_OK;
    }
    return SALTS_ENOTSUP;
  }
  return SALTS_EINVAL;
}

int chttp_tls_http1_alpn_validate(const char *const *protocols, size_t count) {
  chttp_protocol protocol = CHTTP_HTTP_1_1;
  const int status = chttp_tls_alpn_validate(protocols, count, &protocol);
  if (status != SALTS_OK) return status;
  return protocol == CHTTP_HTTP_1_1 ? SALTS_OK : SALTS_ENOTSUP;
}

int chttp_tls_server_alpn_validate(const char *const *protocols, size_t count, int enable_http2) {
  bool seen_http1 = false;
  bool seen_http2 = false;
  size_t index;
  if ((enable_http2 != 0 && enable_http2 != 1) || count > 2u) return SALTS_ENOTSUP;
  if (count == 0u) return protocols == NULL ? SALTS_OK : SALTS_EINVAL;
  if (protocols == NULL) return SALTS_ENOTSUP;
  for (index = 0u; index < count; ++index) {
    chttp_protocol protocol = CHTTP_HTTP_1_1;
    const int status = chttp_tls_alpn_validate(&protocols[index], 1u, &protocol);
    if (status != SALTS_OK) return status;
    if (protocol == CHTTP_HTTP_2) {
      if (!enable_http2 || seen_http2) return SALTS_ENOTSUP;
      seen_http2 = true;
    } else {
      if (seen_http1) return SALTS_ENOTSUP;
      seen_http1 = true;
    }
  }
  return SALTS_OK;
}

int chttp_tls_profile_init(chttp_tls_profile *profile, const cnet_tls_client_config *config) {
  chttp_tls_profile_impl *impl;
  int status;
  if (profile == NULL || config == NULL) return SALTS_EINVAL;
  if (profile->impl != NULL) return SALTS_EALREADY;
  if (config->size != sizeof(*config)) return SALTS_EINVAL;
  chttp_protocol protocol;
  status = chttp_tls_alpn_validate(config->alpn_protocols, config->alpn_protocol_count, &protocol);
  if (status != SALTS_OK) return status;
  impl = (chttp_tls_profile_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  status = cnet_tls_client_init(&impl->client, config);
  if (status != SALTS_OK) {
    free(impl);
    return status;
  }
  atomic_init(&impl->references, 1u);
  impl->protocol = protocol;
  profile->impl = impl;
  return SALTS_OK;
}

void chttp_tls_profile_release(chttp_tls_profile_impl *impl) {
  if (impl == NULL) return;
  if (atomic_fetch_sub_explicit(&impl->references, 1u, memory_order_acq_rel) == 1u) {
    (void)cnet_tls_client_destroy(&impl->client);
    free(impl);
  }
}

int chttp_tls_profile_destroy(chttp_tls_profile *profile) {
  chttp_tls_profile_impl *impl;
  if (profile == NULL) return SALTS_EINVAL;
  impl = (chttp_tls_profile_impl *)profile->impl;
  if (impl == NULL) return SALTS_OK;
  profile->impl = NULL;
  chttp_tls_profile_release(impl);
  return SALTS_OK;
}

int chttp_tls_profile_acquire(const chttp_tls_profile *profile, chttp_tls_profile_impl **out_impl) {
  chttp_tls_profile_impl *impl;
  if (out_impl == NULL) return SALTS_EINVAL;
  *out_impl = NULL;
  if (profile == NULL) return SALTS_OK;
  impl = (chttp_tls_profile_impl *)profile->impl;
  if (impl == NULL) return SALTS_EINVAL;
  (void)atomic_fetch_add_explicit(&impl->references, 1u, memory_order_relaxed);
  *out_impl = impl;
  return SALTS_OK;
}

const cnet_tls_client *chttp_tls_profile_client(const chttp_tls_profile_impl *impl) {
  return impl != NULL ? &impl->client : NULL;
}

chttp_protocol chttp_tls_profile_protocol(const chttp_tls_profile_impl *impl) {
  return impl != NULL ? impl->protocol : CHTTP_HTTP_1_1;
}
