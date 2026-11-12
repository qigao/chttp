#include "chttp_retry.h"
#include <stdlib.h>
#include <string.h>

void chttp_retry_budget_init(chttp_retry_budget *b, const chttp_retry_options *options,
    chttp_method method, size_t body_bytes, uint64_t deadline_ms) {
  *b = (chttp_retry_budget){.options = *options,
      .exponential_ms = options->initial_backoff_ms,
      .random = options->jitter_seed != 0u ? options->jitter_seed : UINT64_C(0x9e3779b97f4a7c15),
      .gate = {.size = sizeof(b->gate), .version = CNET_RECOVERY_POLICY_VERSION,
          .attempts_used = 1u, .max_attempts = options->max_attempts,
          .deadline_ms = deadline_ms, .request_body_bytes = body_bytes,
          .remaining_retry_byte_budget = options->retry_body_byte_budget,
          .application_declares_idempotent = options->application_declares_idempotent ||
              method == CHTTP_METHOD_GET || method == CHTTP_METHOD_HEAD ||
              method == CHTTP_METHOD_PUT || method == CHTTP_METHOD_DELETE ||
              method == CHTTP_METHOD_OPTIONS}};
}

int chttp_retry_budget_prepare(chttp_retry_budget *b, const chttp_attempt_facts *facts,
    int failure_status, uint64_t now_ms, cnet_retry_result *out) {
  uint64_t delay = 0u, random = b->random;
  int status;
  if (b->waiting) return SALTS_EBUSY;
  b->gate.now_ms = now_ms;
  b->gate.backoff_not_before_ms = 0u;
  b->gate.explicitly_enabled = b->options.max_attempts > 1u && facts->retryable_failure;
  b->gate.cancelled = failure_status == SALTS_ECANCELED || failure_status == SALTS_ESHUTDOWN;
  b->gate.security_failure = facts->security_failure || failure_status == SALTS_EAUTH;
  b->gate.owned_replayable_body = !facts->response_started;
  b->gate.protocol_proves_not_executed = facts->protocol_not_executed;
  status = cnet_retry_evaluate(&b->gate, out);
  if (status != SALTS_OK || !out->allowed) return status;
  random ^= random << 13u; random ^= random >> 7u; random ^= random << 17u;
  {
    const cnet_backoff_input backoff = {.attempts = b->gate.attempts_used,
        .initial_ms = b->options.initial_backoff_ms, .maximum_ms = b->options.maximum_backoff_ms,
        .exponential_ms = b->exponential_ms, .random_value = random};
    status = cnet_backoff_exponential_jitter(NULL, &backoff, &delay);
  }
  if (status != SALTS_OK) return status;
  if (delay > UINT64_MAX - now_ms) return SALTS_ERANGE;
  b->gate.backoff_not_before_ms = now_ms + delay;
  b->random = random;
  b->waiting = true;
  return SALTS_OK;
}

int chttp_retry_budget_advance(chttp_retry_budget *b, uint64_t now_ms, cnet_retry_result *out) {
  int status;
  if (!b->waiting) return SALTS_EALREADY;
  b->gate.now_ms = now_ms;
  status = cnet_retry_evaluate(&b->gate, out);
  if (status != SALTS_OK) return status;
  if (out->reason != CNET_RETRY_WAIT_BACKOFF) b->waiting = false;
  if (out->allowed) {
    /* CNet proved both subtractions/admission limits before this commit. */
    b->gate.remaining_retry_byte_budget -= b->gate.request_body_bytes;
    ++b->gate.attempts_used;
    b->exponential_ms = b->exponential_ms > b->options.maximum_backoff_ms / 2u
        ? b->options.maximum_backoff_ms : b->exponential_ms * 2u;
  }
  return SALTS_OK;
}

static int retry_copy_string(tstr *out, const char *source, size_t limit, size_t *bytes) {
  size_t size = 0u;
  if (source == NULL) return SALTS_EINVAL;
  while (size < limit && source[size] != '\0') ++size;
  if (source[size] != '\0') return SALTS_EMSGSIZE;
  *out = tstr_dup_len(source, size);
  if (*out == NULL) return SALTS_ENOMEM;
  if (bytes != NULL) *bytes = size;
  return SALTS_OK;
}

void chttp_retry_request_destroy(chttp_retry_request *r) {
  if (r->headers != NULL) {
    for (size_t i = 0u; i < r->options.header_count; ++i) {
      tstr_free((tstr)r->headers[i].name);
      tstr_free((tstr)r->headers[i].value);
    }
  }
  free(r->headers);
  free(r->body);
  tstr_free(r->uri); tstr_free(r->authority); tstr_free(r->target);
  *r = (chttp_retry_request){0};
}

int chttp_retry_request_copy(chttp_retry_request *r, const chttp_options *o,
    const chttp_client_config *config) {
  size_t header_bytes = 0u;
  int status;
  *r = (chttp_retry_request){.options = *o};
  if (o->header_count > config->max_header_count ||
      o->body_size > config->max_request_body_bytes) return SALTS_EMSGSIZE;
  if ((o->header_count != 0u && o->headers == NULL) ||
      (o->body_size != 0u && o->body == NULL)) return SALTS_EINVAL;
  status = retry_copy_string(&r->uri, o->connection_uri, config->max_header_bytes, NULL);
  if (status == SALTS_OK)
    status = retry_copy_string(&r->authority, o->authority, config->max_header_bytes, NULL);
  if (status == SALTS_OK)
    status = retry_copy_string(&r->target, o->target, config->max_start_line_bytes, NULL);
  if (status != SALTS_OK) return status;
  if (o->header_count > SIZE_MAX / sizeof(*r->headers)) return SALTS_ERANGE;
  if (o->header_count != 0u) {
    r->headers = calloc(o->header_count, sizeof(*r->headers));
    if (r->headers == NULL) return SALTS_ENOMEM;
  }
  for (size_t i = 0u; i < o->header_count; ++i) {
    tstr name = NULL, value = NULL;
    size_t size = 0u;
    status = retry_copy_string(&name, o->headers[i].name,
        config->max_header_bytes - header_bytes, &size);
    r->headers[i].name = name;
    if (status != SALTS_OK) return status;
    header_bytes += size;
    status = retry_copy_string(&value, o->headers[i].value,
        config->max_header_bytes - header_bytes, &size);
    r->headers[i].value = value;
    if (status != SALTS_OK) return status;
    header_bytes += size;
  }
  if (o->body_size != 0u) {
    r->body = malloc(o->body_size);
    if (r->body == NULL) return SALTS_ENOMEM;
    memcpy(r->body, o->body, o->body_size);
  }
  r->options.connection_uri = r->uri;
  r->options.authority = r->authority;
  r->options.target = r->target;
  r->options.headers = r->headers;
  r->options.body = r->body;
  return SALTS_OK;
}
