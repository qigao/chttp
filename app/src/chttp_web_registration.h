#ifndef CHTTP_WEB_REGISTRATION_H
#define CHTTP_WEB_REGISTRATION_H
#include <chttp_app/web.h>
#ifdef __cplusplus
extern "C" {
#endif
int chttp_web_assets_use(
    chttp_server *server,
    const chttp_web_asset_mount *mount);

int chttp_web_security_use(
    chttp_server *server,
    const chttp_web_security_policy *policy);
#ifdef __cplusplus
}
#endif
#endif
