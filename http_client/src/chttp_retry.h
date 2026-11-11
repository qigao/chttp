#ifndef CHTTP_RETRY_H
#define CHTTP_RETRY_H
#include <http_client/http.h>
#include <core/tstr.h>

/* One blocking logical call owns this immutable input snapshot until every
 * admitted attempt has settled. Header count/bytes and body bytes are bounded
 * by the existing client config. No other owner or unbounded queue is added. */
typedef struct chttp_retry_request {
  chttp_options options;
  tstr uri, authority, target;
  chttp_header *headers;
  void *body;
} chttp_retry_request;
int chttp_retry_request_copy(chttp_retry_request *out, const chttp_options *options,
    const chttp_client_config *config);
void chttp_retry_request_destroy(chttp_retry_request *request);
#endif
