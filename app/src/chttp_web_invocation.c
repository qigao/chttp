#include <chttp_app/invocation.h>
#include "chttp_web_engine.h"
#include "chttp_web_output.h"
#include "chttp_invocation_internal.h"
#include "../../http_server/src/chttp_server_engine.h"
#include <stdio.h>
static int invocation_reply(const void *ctx, unsigned int code, const char *type, const void *body,
                            size_t size) {
  return chttp_invocation_respond(ctx, code, type, body, size);
}
static int invocation_header(const void *ctx, const char *name, const char *value) {
  return chttp_invocation_set_header(ctx, name, value);
}
static int engine_reply(const void *ctx, unsigned int code, const char *type, const void *body, size_t size) {
  return chttp_server_reply((chttp_server_response *)ctx, code, type, body, size);
}
static int engine_header(const void *ctx, const char *name, const char *value) {
  return chttp_server_response_set_header((chttp_server_response *)ctx, name, value);
}
static int invocation_forbidden(const void *ctx, const chttp_web_auth_policy *policy,
                                const chttp_server_request_view *request, const chttp_web_output *out) {
  (void)ctx;
  return policy->forbidden ? policy->forbidden(policy->forbidden_user, request, out->context)
                           : chttp_web_output_reply(out, 403u, NULL, NULL, 0u);
}
static int engine_forbidden(const void *ctx, const chttp_web_auth_policy *policy,
                            const chttp_server_request_view *request, const chttp_web_output *out) {
  (void)policy;
  const chttp_web_engine_auth_policy *original = ctx;
  return original->forbidden
             ? original->forbidden(original->forbidden_user, request, (chttp_server_response *)out->context)
             : chttp_web_output_reply(out, 403u, NULL, NULL, 0u);
}
static chttp_web_status bridge_error(chttp_web_error *error, chttp_web_status status, int native) {
  if (error) {
    *error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    error->status = status;
    error->native_status = native;
    if (status != CHTTP_WEB_OK)
      snprintf(error->message, sizeof(error->message), "Web response staging failed (%d)", native);
  }
  return status;
}
chttp_web_status chttp_web_render_response(chttp_web_renderer *renderer, const chttp_invocation *response,
                                           const char *template_name, const cmeta_data_desc *model_desc,
                                           const void *model, unsigned int status_code,
                                           const char *content_type, chttp_web_error *error) {
  if (!chttp_invocation_request(response))
    return bridge_error(error, CHTTP_WEB_INVALID_ARGUMENT, SALTS_EINVAL);
  const chttp_web_output output = {response, invocation_reply, invocation_header, NULL, NULL};
  return chttp_web_output_render_response(renderer, &output, template_name, model_desc, model, status_code,
                                          content_type, error);
}
chttp_web_status chttp_web_engine_render_response(chttp_web_renderer *renderer,
                                                  chttp_server_response *response, const char *template_name,
                                                  const cmeta_data_desc *model_desc, const void *model,
                                                  unsigned int status_code, const char *content_type,
                                                  chttp_web_error *error) {
  const chttp_web_output output = {response, engine_reply, engine_header, NULL, NULL};
  return chttp_web_output_render_response(renderer, &output, template_name, model_desc, model, status_code,
                                          content_type, error);
}
chttp_web_status chttp_web_redirect(const chttp_invocation *response, unsigned int status_code,
                                    const char *location, chttp_web_error *error) {
  if (!chttp_invocation_request(response))
    return bridge_error(error, CHTTP_WEB_INVALID_ARGUMENT, SALTS_EINVAL);
  const chttp_web_output output = {response, invocation_reply, invocation_header, NULL, NULL};
  return chttp_web_output_redirect(&output, status_code, location, error);
}
chttp_web_status chttp_web_engine_redirect(chttp_server_response *response, unsigned int status_code,
                                           const char *location, chttp_web_error *error) {
  const chttp_web_output output = {response, engine_reply, engine_header, NULL, NULL};
  return chttp_web_output_redirect(&output, status_code, location, error);
}
chttp_web_status chttp_web_hx_redirect(const chttp_invocation *response, const char *location,
                                       chttp_web_error *error) {
  if (!chttp_invocation_request(response))
    return bridge_error(error, CHTTP_WEB_INVALID_ARGUMENT, SALTS_EINVAL);
  const chttp_web_output output = {response, invocation_reply, invocation_header, NULL, NULL};
  return chttp_web_output_hx_redirect(&output, location, error);
}
chttp_web_status chttp_web_engine_hx_redirect(chttp_server_response *response, const char *location,
                                              chttp_web_error *error) {
  const chttp_web_output output = {response, engine_reply, engine_header, NULL, NULL};
  return chttp_web_output_hx_redirect(&output, location, error);
}
chttp_web_status chttp_web_hx_trigger(const chttp_invocation *response, const char *trigger,
                                      chttp_web_error *error) {
  if (!chttp_invocation_request(response))
    return bridge_error(error, CHTTP_WEB_INVALID_ARGUMENT, SALTS_EINVAL);
  const chttp_web_output output = {response, invocation_reply, invocation_header, NULL, NULL};
  return chttp_web_output_hx_trigger(&output, trigger, error);
}
chttp_web_status chttp_web_engine_hx_trigger(chttp_server_response *response, const char *trigger,
                                             chttp_web_error *error) {
  const chttp_web_output output = {response, engine_reply, engine_header, NULL, NULL};
  return chttp_web_output_hx_trigger(&output, trigger, error);
}
chttp_web_status chttp_web_hx_retarget(const chttp_invocation *response, const char *selector,
                                       chttp_web_error *error) {
  if (!chttp_invocation_request(response))
    return bridge_error(error, CHTTP_WEB_INVALID_ARGUMENT, SALTS_EINVAL);
  const chttp_web_output output = {response, invocation_reply, invocation_header, NULL, NULL};
  return chttp_web_output_hx_retarget(&output, selector, error);
}
chttp_web_status chttp_web_engine_hx_retarget(chttp_server_response *response, const char *selector,
                                              chttp_web_error *error) {
  const chttp_web_output output = {response, engine_reply, engine_header, NULL, NULL};
  return chttp_web_output_hx_retarget(&output, selector, error);
}
chttp_web_status chttp_web_render_error(chttp_web_renderer *renderer, const chttp_invocation *response,
                                        unsigned int status_code, const char *template_name,
                                        const cmeta_data_desc *model_desc, const void *model,
                                        chttp_web_error *error) {
  if (!chttp_invocation_request(response))
    return bridge_error(error, CHTTP_WEB_INVALID_ARGUMENT, SALTS_EINVAL);
  const chttp_web_output output = {response, invocation_reply, invocation_header, NULL, NULL};
  return chttp_web_output_render_error(renderer, &output, status_code, template_name, model_desc, model,
                                       error);
}
chttp_web_status chttp_web_engine_render_error(chttp_web_renderer *renderer, chttp_server_response *response,
                                               unsigned int status_code, const char *template_name,
                                               const cmeta_data_desc *model_desc, const void *model,
                                               chttp_web_error *error) {
  const chttp_web_output output = {response, engine_reply, engine_header, NULL, NULL};
  return chttp_web_output_render_error(renderer, &output, status_code, template_name, model_desc, model,
                                       error);
}
int chttp_web_authorize(const chttp_invocation *invocation, const chttp_web_auth_policy *policy,
                        bool *proceed) {
  const chttp_server_request_view *request = chttp_invocation_request(invocation);
  if (!request || !proceed)
    return SALTS_EINVAL;
  const chttp_web_output output = {invocation, invocation_reply, invocation_header, invocation_forbidden,
                                   NULL};
  bool allowed = false;
  const int status = chttp_web_output_authorize((void *)policy, request, &output, &allowed);
  if (status != SALTS_OK)
    return status;
  if (!allowed)
    return chttp_invocation_mark_rejected(invocation, proceed);
  *proceed = true;
  return SALTS_OK;
}
int chttp_web_engine_auth_middleware(void *user, const chttp_server_request_view *request,
                                     chttp_server_response *response, chttp_server_next *next) {
  const chttp_web_engine_auth_policy *old = user;
  if (!old || old->size < sizeof(*old) || !next)
    return SALTS_EINVAL;
  const chttp_web_auth_policy policy = {sizeof(policy),
                                        old->login_path,
                                        old->include_return_target,
                                        old->max_return_target_bytes,
                                        old->authorize,
                                        old->authorize_user,
                                        NULL,
                                        NULL};
  const chttp_web_output output = {response, engine_reply, engine_header, engine_forbidden, old};
  bool allowed = false;
  const int status = chttp_web_output_authorize((void *)&policy, request, &output, &allowed);
  return status == SALTS_OK && allowed ? chttp_server_next_call(next) : status;
}
chttp_web_status chttp_web_sse_response(const chttp_invocation *invocation, chttp_web_sse_stream *stream,
                                        chttp_web_error *error) {
  if (!chttp_invocation_request(invocation) || !stream || stream->size != sizeof(*stream))
    return bridge_error(error, CHTTP_WEB_INVALID_ARGUMENT, SALTS_EINVAL);
  const int status = chttp_invocation_sse_response(invocation, stream, stream->scratch_capacity);
  return bridge_error(error, status == SALTS_OK ? CHTTP_WEB_OK : CHTTP_WEB_SERVER, status);
}
