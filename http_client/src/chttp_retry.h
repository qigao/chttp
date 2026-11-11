#ifndef CHTTP_RETRY_H
#define CHTTP_RETRY_H
#include <http_client/http.h>
#include <core/tstr.h>
#include <cnet/recovery_policy.h>
#include "chttp_attempt.h"

/* One owner, one fixed deadline. The host supplies monotonic time; this pure
 * admission state never reads a clock, sleeps, polls or retains protocol data.
 * prepare follows actual attempt settlement; advance consumes a retry credit
 * exactly once when its backoff and all CNet gates permit admission. */
typedef struct chttp_retry_budget {
  cnet_retry_input gate;
  chttp_retry_options options;
  uint64_t exponential_ms;
  uint64_t random;
  bool waiting;
} chttp_retry_budget;
void chttp_retry_budget_init(chttp_retry_budget *budget, const chttp_retry_options *options,
    chttp_method method, size_t body_bytes, uint64_t deadline_ms);
int chttp_retry_budget_prepare(chttp_retry_budget *budget, const chttp_attempt_facts *facts,
    int failure_status, uint64_t now_ms, cnet_retry_result *out);
int chttp_retry_budget_advance(chttp_retry_budget *budget, uint64_t now_ms,
    cnet_retry_result *out);

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
