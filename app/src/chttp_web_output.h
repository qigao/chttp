#ifndef CHTTP_WEB_OUTPUT_H
#define CHTTP_WEB_OUTPUT_H
#include <chttp_app/web.h>
/* Two real output owners share Web rendering/redirect/auth validation. Neither
 * adapter fabricates an engine builder or extends a borrowed callback scope. */
typedef struct chttp_web_output {
  const void *context;
  int (*reply)(const void *, unsigned int, const char *, const void *, size_t);
  int (*header)(const void *, const char *, const char *);
  int (*forbidden)(const void *, const chttp_web_auth_policy *, const chttp_server_request_view *,
                   const struct chttp_web_output *);
  const void *forbidden_context;
} chttp_web_output;
static inline int chttp_web_output_reply(const chttp_web_output *out, unsigned int code, const char *type,
                                         const void *body, size_t size) {
  return out && out->context ? out->reply(out->context, code, type, body, size) : SALTS_EINVAL;
}
static inline int chttp_web_output_header(const chttp_web_output *out, const char *name, const char *value) {
  return out && out->context ? out->header(out->context, name, value) : SALTS_EINVAL;
}
chttp_web_status chttp_web_output_render_response(chttp_web_renderer *renderer,
                                                  const chttp_web_output *response, const char *template_name,
                                                  const cmeta_data_desc *model_desc, const void *model,
                                                  unsigned int status_code, const char *content_type,
                                                  chttp_web_error *error);
chttp_web_status chttp_web_output_redirect(const chttp_web_output *response, unsigned int status_code,
                                           const char *location, chttp_web_error *error);
chttp_web_status chttp_web_output_hx_redirect(const chttp_web_output *response, const char *location,
                                              chttp_web_error *error);
chttp_web_status chttp_web_output_hx_trigger(const chttp_web_output *response, const char *trigger,
                                             chttp_web_error *error);
chttp_web_status chttp_web_output_hx_retarget(const chttp_web_output *response, const char *selector,
                                              chttp_web_error *error);
chttp_web_status chttp_web_output_render_error(chttp_web_renderer *renderer, const chttp_web_output *response,
                                               unsigned int status_code, const char *template_name,
                                               const cmeta_data_desc *model_desc, const void *model,
                                               chttp_web_error *error);
int chttp_web_output_authorize(void *, const chttp_server_request_view *, const chttp_web_output *, bool *);
#endif
