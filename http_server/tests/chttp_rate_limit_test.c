#include "chttp_rate_limit_internal.h"
#include "tinytest.h"
#include <string.h>

static chttp_rate_limit_config rate_config(chttp_rate_limit_scope scope) {
  const chttp_rate_limit_config config = {scope, 1, 2, 3, 2000, CHTTP_RATE_LIMIT_KEY_BYTES};
  return config;
}

spec("CHTTP bounded rate limiter") {
  it("preserves fractional credit, rounds Retry-After up and rejects backward clocks") {
    chttp_rate_limiter limiter = {0};
    chttp_rate_limit_config config = rate_config(CHTTP_RATE_LIMIT_GLOBAL);
    chttp_server_request_view request = {0};
    chttp_server_admission_result result = {0};
    check_equal(chttp_rate_limiter_init(&limiter, &config), SALTS_OK);
    for (int i = 0; i < 2; ++i) {
      check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
      check_equal(result.status_code, 0u);
    }
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    check_equal(result.status_code, 429u);
    check_equal(result.retry_after_seconds, 1u);
    for (uint64_t now = 1; now <= 666; ++now) {
      check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, now), SALTS_OK);
      check_equal(result.status_code, 429u);
    }
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 665), SALTS_EINVAL);
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 667), SALTS_OK);
    check_equal(result.status_code, 0u);
    check_equal(result.retry_after_seconds, 0u);
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, UINT64_MAX), SALTS_OK);
    check_equal(result.status_code, 0u);
    chttp_rate_limiter_destroy(&limiter);
    chttp_rate_limiter_destroy(&limiter);
  }

  it("shares peer ports, preserves IPv6 scopes, and never evicts a bucket with debt") {
    chttp_rate_limiter limiter = {0};
    chttp_rate_limit_config config = rate_config(CHTTP_RATE_LIMIT_PEER_IP);
    cnet_stream_peer peer = {.family = CNET_DATAGRAM_ADDRESS_IPV6, .port = 100, .scope_id = 1};
    chttp_server_request_view request = {.peer = &peer};
    chttp_server_admission_result result = {0};
    check_equal(chttp_rate_limiter_init(&limiter, &config), SALTS_OK);
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    peer.port++;
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    check_equal(result.status_code, 0u);
    peer.port++;
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    check_equal(result.status_code, 429u);
    peer.scope_id++;
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 667), SALTS_OK);
    check_equal(result.status_code, 503u);
    check_equal(result.retry_after_seconds, 0u);
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 1334), SALTS_OK);
    check_equal(result.status_code, 0u);
    request.peer = NULL;
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 1334), SALTS_EINVAL);
    chttp_rate_limiter_destroy(&limiter);
  }

  it("namespaces verified subjects by issuer and copies bounded keys") {
    chttp_rate_limiter limiter = {0};
    chttp_rate_limit_config config = rate_config(CHTTP_RATE_LIMIT_JWT_SUBJECT);
    config.group_capacity = 2;
    config.burst = 1;
    config.key_bytes = 6;
    char subject[] = "bc";
    chttp_jwt_claims_view claims = {.issuer = "a", .subject = subject};
    chttp_server_request_view request = {.jwt_claims = &claims};
    chttp_server_admission_result result = {0};
    check_equal(chttp_rate_limiter_init(&limiter, &config), SALTS_OK);
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    check_equal(result.status_code, 0u);
    subject[0] = 'x';
    claims.issuer = "ab";
    claims.subject = "c";
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    check_equal(result.status_code, 0u);
    claims.issuer = "a";
    claims.subject = "bc";
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    check_equal(result.status_code, 429u);
    claims.subject = "longer";
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    check_equal(result.status_code, 431u);
    claims.subject = "";
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    check_equal(result.status_code, 403u);
    request.jwt_claims = NULL;
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    check_equal(result.status_code, 403u);
    chttp_rate_limiter_destroy(&limiter);
  }

  it("validates configuration and handles maximum uint32 arithmetic") {
    chttp_rate_limiter limiter = {0};
    chttp_rate_limit_config config = rate_config(CHTTP_RATE_LIMIT_GLOBAL);
    chttp_server_request_view request = {0};
    chttp_server_admission_result result = {0};
    config.burst = 0;
    check_equal(chttp_rate_limiter_init(&limiter, &config), SALTS_EINVAL);
    config.burst = UINT32_MAX;
    config.refill_period_ms = UINT32_MAX;
    config.refill_tokens = UINT32_MAX;
    check_equal(chttp_rate_limiter_init(&limiter, &config), SALTS_OK);
    check_equal(chttp_rate_limiter_init(&limiter, &config), SALTS_EALREADY);
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    check_equal(result.status_code, 0u);
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, UINT64_MAX), SALTS_OK);
    check_equal(result.status_code, 0u);
    chttp_rate_limiter_destroy(&limiter);
    config.burst = 1;
    config.refill_tokens = 1;
    check_equal(chttp_rate_limiter_init(&limiter, &config), SALTS_OK);
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    check_equal(chttp_rate_limiter_admit_at(&limiter, &request, &result, 0), SALTS_OK);
    check_equal(result.status_code, 429u);
    check_equal(result.retry_after_seconds, 4294968u);
    chttp_rate_limiter_destroy(&limiter);
    config.scope = CHTTP_RATE_LIMIT_PEER_IP;
    config.group_capacity = SIZE_MAX;
    check_equal(chttp_rate_limiter_init(&limiter, &config), SALTS_ERANGE);
  }
}
