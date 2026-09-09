#ifndef HTTP_SERVER_RATE_LIMIT_H
#define HTTP_SERVER_RATE_LIMIT_H

#include <http_server/http.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { CHTTP_RATE_LIMIT_KEY_BYTES = 256 };
typedef enum chttp_rate_limit_scope {
  CHTTP_RATE_LIMIT_GLOBAL,
  CHTTP_RATE_LIMIT_PEER_IP,
  CHTTP_RATE_LIMIT_JWT_SUBJECT
} chttp_rate_limit_scope;

typedef struct chttp_rate_limit_config {
  chttp_rate_limit_scope scope;
  size_t group_capacity; /**< Positive; global scope requires exactly one. */
  uint32_t burst; /**< Positive initial and maximum token count. */
  uint32_t refill_tokens; /**< Positive tokens replenished per refill_period_ms. */
  uint32_t refill_period_ms; /**< Positive; fractional tokens are preserved. */
  size_t key_bytes; /**< JWT issuer + subject bytes including both NULs; 2..256. */
} chttp_rate_limit_config;

typedef struct chttp_rate_limiter { void *impl; } chttp_rate_limiter;

/**
 * Initializes a zeroed handle, copies config and reserves all group storage.
 * Returns SALTS_OK, SALTS_EINVAL for invalid config, SALTS_EALREADY for a live
 * handle, SALTS_ERANGE for storage overflow or SALTS_ENOMEM on allocation failure.
 * One owner thread only, including direct admit calls; never share between server owners.
 */
int chttp_rate_limiter_init(chttp_rate_limiter *limiter, const chttp_rate_limit_config *config);

/** Releases storage. Unbind or destroy the server first; no concurrent calls are allowed. */
void chttp_rate_limiter_destroy(chttp_rate_limiter *limiter);

/**
 * Admission callback: bind with chttp_server_set_admission(server, chttp_rate_limiter_admit, limiter).
 * Charges one token per admitted request, with no refund after later failures. Exhaustion
 * rejects with 429 and rounded-up Retry-After seconds. Full group capacity rejects new
 * identities with 503; only fully replenished groups can be reused. No request-time allocation.
 * Peer scope ignores ports and forwarded headers; IPv6 scope IDs remain distinct.
 * JWT scope requires validated nonempty subject (otherwise 403); issuer namespaces subjects.
 * Oversized JWT keys reject with 431; missing/unsupported peer returns SALTS_EINVAL (500).
 * Result is overwritten; SALTS_OK means a valid allow/reject decision. Request pointers
 * are borrowed only for this call. O(group_capacity * key_bytes) time, fixed reserved storage.
 */
int chttp_rate_limiter_admit(void *limiter, const chttp_server_request_view *request,
                           chttp_server_admission_result *result);

#ifdef __cplusplus
}
#endif
#endif
