#include "chttp_service_plugin_probe.h"

static chttp_service_plugin_probe_fn observe;
static void *context;

/* Control thread sets/clears exclusively before start/after joined close.
 * Concurrent provider entries only read these immutable borrowed values. */
void chttp_service_plugin_probe_set(chttp_service_plugin_probe_fn fn, void *user) {
  observe = fn; context = user;
}
bool chttp_service_plugin_probe_enter(void) {
  return !observe || observe(context);
}
