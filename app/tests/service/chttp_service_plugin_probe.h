#ifndef CHTTP_SERVICE_PLUGIN_PROBE_H
#define CHTTP_SERVICE_PLUGIN_PROBE_H
#include <stdbool.h>

/* Shared only by the host test and its generated DSO fixture. The callback
 * borrows host state until the Application drains; it can gate actual DSO code.
 * Existing engine fixtures leave it unset and retain their normal behavior. */
typedef bool (*chttp_service_plugin_probe_fn)(void *);
void chttp_service_plugin_probe_set(chttp_service_plugin_probe_fn, void *);
bool chttp_service_plugin_probe_enter(void);
#endif
