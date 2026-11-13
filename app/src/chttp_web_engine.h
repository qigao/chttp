#ifndef CHTTP_WEB_ENGINE_H
#define CHTTP_WEB_ENGINE_H
/* Native engine fixture/adapter contracts. Not installed or Application APIs. */
#include <chttp_app/web.h>
typedef bool (*chttp_web_asset_immutable_fn)(void *user, const char *relative_path);
typedef const char *(*chttp_web_asset_etag_fn)(void *user, const char *relative_path);

typedef struct chttp_web_engine_asset_mount {
  size_t size;
  const char *url_prefix;
  const char *filesystem_root;
  size_t max_relative_path_bytes;
  const char *cache_control;
  const char *immutable_cache_control;
  chttp_web_asset_immutable_fn immutable;
  chttp_web_asset_etag_fn etag;
  void *user;
  const char *spa_url_prefix;
  const char *spa_fallback_relative_path;
} chttp_web_engine_asset_mount;

#define CHTTP_WEB_ENGINE_ASSET_MOUNT_INIT                                                                    \
  {sizeof(chttp_web_engine_asset_mount), NULL, NULL, 0u, NULL, NULL, NULL, NULL, NULL, NULL, NULL}

typedef struct chttp_web_engine_auth_policy {
  size_t size;
  const char *login_path;
  bool include_return_target;
  size_t max_return_target_bytes;
  chttp_web_authorize_fn authorize;
  void *authorize_user;
  chttp_server_handler_fn forbidden;
  void *forbidden_user;
} chttp_web_engine_auth_policy;

#define CHTTP_WEB_ENGINE_AUTH_POLICY_INIT                                                                    \
  {sizeof(chttp_web_engine_auth_policy), NULL, false, 0u, NULL, NULL, NULL, NULL}

chttp_web_status chttp_web_engine_render_response(chttp_web_renderer *renderer,
                                                  chttp_server_response *response, const char *template_name,
                                                  const cmeta_data_desc *model_desc, const void *model,
                                                  unsigned int status_code, const char *content_type,
                                                  chttp_web_error *error);
chttp_web_status chttp_web_engine_redirect(chttp_server_response *response, unsigned int status_code,
                                           const char *location, chttp_web_error *error);
chttp_web_status chttp_web_engine_hx_redirect(chttp_server_response *response, const char *location,
                                              chttp_web_error *error);
chttp_web_status chttp_web_engine_hx_trigger(chttp_server_response *response, const char *trigger,
                                             chttp_web_error *error);
chttp_web_status chttp_web_engine_hx_retarget(chttp_server_response *response, const char *selector,
                                              chttp_web_error *error);
chttp_web_status chttp_web_engine_render_error(chttp_web_renderer *renderer, chttp_server_response *response,
                                               unsigned int status_code, const char *template_name,
                                               const cmeta_data_desc *model_desc, const void *model,
                                               chttp_web_error *error);
chttp_web_status chttp_web_engine_sse_response(chttp_server_response *response, chttp_web_sse_stream *stream,
                                               chttp_web_error *error);
chttp_web_status chttp_web_engine_deferred_render_reply(chttp_web_renderer *renderer,
                                                        chttp_server_deferred *deferred,
                                                        const char *template_name,
                                                        const cmeta_data_desc *model_desc, const void *model,
                                                        unsigned int status_code, const char *content_type,
                                                        chttp_web_error *error);
int chttp_web_engine_auth_middleware(void *, const chttp_server_request_view *, chttp_server_response *,
                                     chttp_server_next *);
#endif
