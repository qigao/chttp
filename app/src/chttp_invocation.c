#include "chttp_invocation_internal.h"
#include "chttp_web_registration.h"

static const chttp_invocation_state *invocation_state(const chttp_invocation *invocation) {
  const chttp_invocation_state *state = invocation ? invocation->impl : NULL;
  return state && state->thread_token == sync_thread_current_token() &&
      chttp_application_work_reply_current(state->reply) ? state : NULL;
}
const chttp_server_request_view *chttp_invocation_request(const chttp_invocation *invocation) {
  const chttp_invocation_state *state = invocation_state(invocation);
  return state ? &state->request : NULL;
}
chttp_session *chttp_invocation_session(const chttp_invocation *invocation) {
  const chttp_invocation_state *state = invocation_state(invocation);
  return state ? state->session : NULL;
}
const cmeta_function_desc *chttp_invocation_operation(const chttp_invocation *invocation) {
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  return request ? request->operation : NULL;
}
int chttp_invocation_cancelled(const chttp_invocation *invocation, bool *out) {
  const chttp_invocation_state *state = invocation_state(invocation);
  return state ? chttp_application_work_cancelled(state->reply, out) : SALTS_EINVAL;
}
int chttp_invocation_respond(const chttp_invocation *invocation, unsigned int code,
    const char *type, const void *body, size_t size) {
  const chttp_invocation_state *state = invocation_state(invocation);
  return state ? chttp_application_work_respond(state->reply, code, type, body, size) : SALTS_EINVAL;
}
int chttp_invocation_set_header(const chttp_invocation *invocation, const char *name, const char *value) {
  const chttp_invocation_state *state = invocation_state(invocation);
  return state ? chttp_application_work_set_header(state->reply, name, value) : SALTS_EINVAL;
}
int chttp_invocation_response_file(const chttp_invocation *invocation, unsigned int code,
    const char *type, const char *path) {
  const chttp_invocation_state *state = invocation_state(invocation);
  return state ? chttp_application_work_response_file(state->reply, code, type, path) : SALTS_EINVAL;
}
int chttp_invocation_serve_file(const chttp_invocation *invocation, const chttp_server_file_options *options) {
  const chttp_invocation_state *state = invocation_state(invocation);
  return state ? chttp_application_work_serve_file(state->reply, options) : SALTS_EINVAL;
}
int chttp_invocation_response_source(const chttp_invocation *invocation, unsigned int code,
    const char *type, const chttp_body_source *source, void (*cleanup)(void *, int), size_t chunk_bytes) {
  const chttp_invocation_state *state = invocation_state(invocation);
  return state ? chttp_application_work_response_source(state->reply, code, type, source, cleanup, chunk_bytes) : SALTS_EINVAL;
}
int chttp_invocation_sse_response(const chttp_invocation *invocation, chttp_web_sse_stream *stream, size_t chunk_bytes) {
  if (!invocation_state(invocation)) return SALTS_EINVAL;
  chttp_body_source source;
  int status = chttp_web_sse_source_open(stream, &source);
  if (status != SALTS_OK) return status;
  status = chttp_invocation_set_header(invocation, "Cache-Control", "no-cache");
  if (status == SALTS_OK) status = chttp_invocation_response_source(invocation, 200u,
      "text/event-stream", &source, chttp_web_sse_source_cleanup, chunk_bytes);
  if (status != SALTS_OK) stream->active = false;
  return status;
}
int chttp_invocation_reject(const chttp_invocation *invocation, bool *proceed, unsigned int code,
    const char *type, const void *body, size_t size) {
  const chttp_invocation_state *state = invocation_state(invocation);
  if (!state || !state->before_result || !proceed) return SALTS_EINVAL;
  const chttp_application_work_exchange exchange = {&state->request, state->reply, state->before_result};
  const int status = chttp_application_work_reject(&exchange, code, type, body, size);
  if (status == SALTS_OK) *proceed = false;
  return status;
}
