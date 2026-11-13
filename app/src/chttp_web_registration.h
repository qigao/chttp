#ifndef CHTTP_WEB_REGISTRATION_H
#define CHTTP_WEB_REGISTRATION_H
#include <chttp_app/web.h>
#include "chttp_web_engine.h"
enum { CHTTP_WEB_ASSET_FS_PATH_HARD_MAX =
    CHTTP_WEB_ASSET_ROOT_HARD_MAX + CHTTP_WEB_ASSET_RELATIVE_HARD_MAX + 2 };
typedef enum chttp_web_asset_selection_kind {
  CHTTP_WEB_ASSET_NEXT, CHTTP_WEB_ASSET_NOT_FOUND,
  CHTTP_WEB_ASSET_METHOD_NOT_ALLOWED, CHTTP_WEB_ASSET_FILE
} chttp_web_asset_selection_kind;
/* Value-only namespace/path selection, shared by owner and worker adapters.
 * No user policy callback or file response runtime is invoked here. */
typedef struct chttp_web_asset_selection {
  chttp_web_asset_selection_kind kind;
  char relative[CHTTP_WEB_ASSET_RELATIVE_HARD_MAX + 1u];
  char path[CHTTP_WEB_ASSET_FS_PATH_HARD_MAX + 1u];
} chttp_web_asset_selection;
#ifdef __cplusplus
extern "C" {
#endif
int chttp_web_assets_validate(const chttp_web_asset_mount *);
int chttp_web_sse_source_open(chttp_web_sse_stream *, chttp_body_source *);
void chttp_web_sse_source_cleanup(void *, int);
bool chttp_web_assets_matches(const chttp_web_asset_mount *, const char *);
int chttp_web_assets_select(const chttp_web_asset_mount *, const chttp_server_request_view *,
    chttp_web_asset_selection *);
int chttp_web_assets_use(
    chttp_server *server,
    const chttp_web_engine_asset_mount *mount);

int chttp_web_security_use(
    chttp_server *server,
    const chttp_web_security_policy *policy);
#ifdef __cplusplus
}
#endif
#endif
