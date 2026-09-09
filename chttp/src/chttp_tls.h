#ifndef CHTTP_TLS_H
#define CHTTP_TLS_H

#include <chttp/chttp.h>

typedef struct chttp_tls_profile_impl chttp_tls_profile_impl;

int chttp_tls_http1_alpn_validate(const char *const *protocols, size_t count);
int chttp_tls_server_alpn_validate(const char *const *protocols, size_t count, int enable_http2);
int chttp_tls_profile_acquire(const chttp_tls_profile *profile, chttp_tls_profile_impl **out_impl);
void chttp_tls_profile_release(chttp_tls_profile_impl *impl);
const cnet_tls_client *chttp_tls_profile_client(const chttp_tls_profile_impl *impl);
chttp_protocol chttp_tls_profile_protocol(const chttp_tls_profile_impl *impl);

#endif /* CHTTP_TLS_H */
