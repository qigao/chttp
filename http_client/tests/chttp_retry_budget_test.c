#include "chttp_retry.h"
#include "tinytest.h"

static chttp_retry_options budget_options(void) {
  chttp_retry_options o = CHTTP_RETRY_OPTIONS_INIT;
  o.max_attempts = 3u;
  o.retry_body_byte_budget = 6u;
  o.initial_backoff_ms = 10u;
  o.maximum_backoff_ms = 40u;
  o.jitter_seed = 7u;
  return o;
}
static const chttp_attempt_facts transient = {.retryable_failure = true};

spec("HTTP retry budget with caller-supplied clock") {
  it("uses seeded backoff and consumes an attempt and body credit only once") {
    const chttp_retry_options o = budget_options();
    chttp_retry_budget b;
    cnet_retry_result decision;
    chttp_retry_budget_init(&b, &o, CHTTP_METHOD_GET, 3u, 1100u);
    check_equal(chttp_retry_budget_prepare(&b, &transient, SALTS_ECONNRESET, 1000u, &decision), SALTS_OK);
    check_equal(b.gate.backoff_not_before_ms, UINT64_C(1008));
    check_equal(chttp_retry_budget_advance(&b, 1007u, &decision), SALTS_OK);
    check_equal(decision.reason, CNET_RETRY_WAIT_BACKOFF);
    check_equal(b.gate.attempts_used, (uint32_t)1u);
    check_equal(b.gate.remaining_retry_byte_budget, (size_t)6u);
    check_equal(chttp_retry_budget_prepare(&b, &transient, SALTS_ECONNRESET, 1007u, &decision), SALTS_EBUSY);
    check_equal(chttp_retry_budget_advance(&b, 1008u, &decision), SALTS_OK);
    check_true(decision.allowed);
    check_equal(b.gate.attempts_used, (uint32_t)2u);
    check_equal(b.gate.remaining_retry_byte_budget, (size_t)3u);
    check_equal(chttp_retry_budget_advance(&b, 1008u, &decision), SALTS_EALREADY);
    check_equal(b.gate.remaining_retry_byte_budget, (size_t)3u);
    check_equal(chttp_retry_budget_prepare(&b, &transient, SALTS_ECONNRESET, 1010u, &decision), SALTS_OK);
    check_equal(b.gate.backoff_not_before_ms, UINT64_C(1023));
    check_equal(chttp_retry_budget_advance(&b, 1023u, &decision), SALTS_OK);
    check_true(decision.allowed);
    check_equal(b.gate.attempts_used, (uint32_t)3u);
    check_equal(b.gate.remaining_retry_byte_budget, (size_t)0u);
    check_equal(chttp_retry_budget_prepare(&b, &transient, SALTS_ECONNRESET, 1030u, &decision), SALTS_OK);
    check_equal(decision.reason, CNET_RETRY_BUDGET);
    check_equal(b.gate.deadline_ms, UINT64_C(1100));
  }
  it("keeps the original deadline when the next seeded backoff cannot fit") {
    chttp_retry_options o = budget_options();
    chttp_retry_budget b;
    cnet_retry_result decision;
    o.max_attempts = 4u; o.retry_body_byte_budget = 9u;
    chttp_retry_budget_init(&b, &o, CHTTP_METHOD_GET, 3u, 1020u);
    check_equal(chttp_retry_budget_prepare(&b, &transient, SALTS_ECONNRESET, 1000u, &decision), SALTS_OK);
    check_equal(chttp_retry_budget_advance(&b, 1008u, &decision), SALTS_OK);
    check_true(decision.allowed);
    check_equal(chttp_retry_budget_prepare(&b, &transient, SALTS_ECONNRESET, 1010u, &decision), SALTS_OK);
    check_equal(chttp_retry_budget_advance(&b, 1010u, &decision), SALTS_OK);
    check_equal(decision.reason, CNET_RETRY_DEADLINE);
    check_equal(b.gate.deadline_ms, UINT64_C(1020));
    check_equal(b.gate.attempts_used, (uint32_t)2u);
    check_equal(b.gate.remaining_retry_byte_budget, (size_t)6u);
  }
  it("expires at the exact absolute deadline without granting a late retry") {
    const chttp_retry_options o = budget_options();
    chttp_retry_budget b;
    cnet_retry_result decision;
    chttp_retry_budget_init(&b, &o, CHTTP_METHOD_GET, 3u, 1100u);
    check_equal(chttp_retry_budget_prepare(&b, &transient, SALTS_ECONNRESET, 1000u, &decision), SALTS_OK);
    check_equal(chttp_retry_budget_advance(&b, 1100u, &decision), SALTS_OK);
    check_equal(decision.reason, CNET_RETRY_DEADLINE);
    check_equal(b.gate.attempts_used, (uint32_t)1u);
    check_equal(b.gate.remaining_retry_byte_budget, (size_t)6u);
  }
  it("does not schedule an unknown POST outcome without application permission") {
    const chttp_retry_options o = budget_options();
    chttp_retry_budget b;
    cnet_retry_result decision;
    chttp_retry_budget_init(&b, &o, CHTTP_METHOD_POST, 3u, 1100u);
    check_equal(chttp_retry_budget_prepare(&b, &transient, SALTS_ECONNRESET, 1000u, &decision), SALTS_OK);
    check_equal(decision.reason, CNET_RETRY_UNAUTHORIZED);
    check_false(b.waiting);
    check_equal(b.random, UINT64_C(7));
    check_equal(b.gate.attempts_used, (uint32_t)1u);
  }
  it("admits a protocol-proven unprocessed POST under the same fixed budget") {
    const chttp_retry_options o = budget_options();
    const chttp_attempt_facts unprocessed = {.retryable_failure = true, .protocol_not_executed = true};
    chttp_retry_budget b;
    cnet_retry_result decision;
    chttp_retry_budget_init(&b, &o, CHTTP_METHOD_POST, 3u, 1100u);
    check_equal(chttp_retry_budget_prepare(&b, &unprocessed, SALTS_ECONNRESET, 1000u, &decision), SALTS_OK);
    check_equal(chttp_retry_budget_advance(&b, 1008u, &decision), SALTS_OK);
    check_true(decision.allowed);
  }
  it("refuses response output even when the application declares idempotence") {
    chttp_retry_options o = budget_options();
    const chttp_attempt_facts response = {.retryable_failure = true, .response_started = true};
    chttp_retry_budget b;
    cnet_retry_result decision;
    o.application_declares_idempotent = true;
    chttp_retry_budget_init(&b, &o, CHTTP_METHOD_POST, 3u, 1100u);
    check_equal(chttp_retry_budget_prepare(&b, &response, SALTS_ECONNRESET, 1000u, &decision), SALTS_OK);
    check_equal(decision.reason, CNET_RETRY_UNREPLAYABLE);
    check_false(b.waiting);
  }
  it("keeps cancellation, stop and authentication failures permanent") {
    const int failures[] = {SALTS_ECANCELED, SALTS_ESHUTDOWN, SALTS_EAUTH};
    const chttp_retry_options o = budget_options();
    for (size_t i = 0u; i < sizeof(failures) / sizeof(failures[0]); ++i) {
      chttp_retry_budget b;
      cnet_retry_result decision;
      chttp_retry_budget_init(&b, &o, CHTTP_METHOD_GET, 3u, 1100u);
      check_equal(chttp_retry_budget_prepare(&b, &transient, failures[i], 1000u, &decision), SALTS_OK);
      check_false(decision.allowed);
      check_false(b.waiting);
      check_equal(b.gate.attempts_used, (uint32_t)1u);
      check_equal(b.gate.remaining_retry_byte_budget, (size_t)6u);
    }
  }
  it("rejects byte exhaustion before scheduling and never charges the first body") {
    chttp_retry_options o = budget_options();
    chttp_retry_budget b;
    cnet_retry_result decision;
    o.retry_body_byte_budget = 2u;
    chttp_retry_budget_init(&b, &o, CHTTP_METHOD_GET, 3u, 1100u);
    check_equal(chttp_retry_budget_prepare(&b, &transient, SALTS_ECONNRESET, 1000u, &decision), SALTS_OK);
    check_equal(decision.reason, CNET_RETRY_BUDGET);
    check_false(b.waiting);
    check_equal(b.gate.remaining_retry_byte_budget, (size_t)2u);
    check_equal(b.random, UINT64_C(7));
  }
  it("bounds zero-body retries by attempts even with zero delay and byte budget") {
    chttp_retry_options o = budget_options();
    chttp_retry_budget b;
    cnet_retry_result decision;
    o.initial_backoff_ms = 0u; o.maximum_backoff_ms = 0u; o.retry_body_byte_budget = 0u;
    chttp_retry_budget_init(&b, &o, CHTTP_METHOD_GET, 0u, 1100u);
    for (uint32_t i = 1u; i < o.max_attempts; ++i) {
      check_equal(chttp_retry_budget_prepare(&b, &transient, SALTS_ECONNRESET, 1000u, &decision), SALTS_OK);
      check_equal(chttp_retry_budget_advance(&b, 1000u, &decision), SALTS_OK);
      check_true(decision.allowed);
    }
    check_equal(chttp_retry_budget_prepare(&b, &transient, SALTS_ECONNRESET, 1000u, &decision), SALTS_OK);
    check_equal(decision.reason, CNET_RETRY_BUDGET);
    check_equal(b.gate.attempts_used, o.max_attempts);
  }
  it("fails deadline arithmetic without consuming a retry or scheduling storage") {
    const chttp_retry_options o = budget_options();
    chttp_retry_budget b;
    cnet_retry_result decision;
    chttp_retry_budget_init(&b, &o, CHTTP_METHOD_GET, 3u, UINT64_MAX);
    check_equal(chttp_retry_budget_prepare(&b, &transient, SALTS_ECONNRESET,
        UINT64_MAX - 4u, &decision), SALTS_ERANGE);
    check_false(b.waiting);
    check_equal(b.gate.attempts_used, (uint32_t)1u);
    check_equal(b.gate.remaining_retry_byte_budget, (size_t)6u);
    check_equal(b.random, UINT64_C(7));
  }
}
