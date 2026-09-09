#ifndef CHTTP_RATE_LIMIT_INTERNAL_H
#define CHTTP_RATE_LIMIT_INTERNAL_H
#include <http_server/rate_limit.h>
/* Internal deterministic clock boundary; timestamps must never decrease. */
int chttp_rate_limiter_admit_at(chttp_rate_limiter *limiter,
                              const chttp_server_request_view *request,
                              chttp_server_admission_result *result, uint64_t now_ms);
#endif
