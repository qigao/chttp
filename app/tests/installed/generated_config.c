#include <chttp_app/application.h>
/* Deliberately fail before listening to verify CONFIGURE wiring in generated main. */
int installed_configure(chttp_application_options *options) {
  return options->server.port == 8080u ? SALTS_ECANCELED : SALTS_OK;
}
