#ifndef HTTP_EXAMPLE_APP_H
#define HTTP_EXAMPLE_APP_H

#include <http_server/rpc.h>
#include <http_server/rate_limit.h>
#include <chttp_app/app.h>
#include "http_example_native.h"
#include "http_example.plugin_client.h"

enum {
  HTTP_EXAMPLE_TIMEOUT_MS = 5000,
  HTTP_EXAMPLE_BODY_BYTES = 8192,
  HTTP_EXAMPLE_PLUGIN_WORKERS = 1,
  HTTP_EXAMPLE_PLUGIN_QUEUE_CAPACITY = 8
};

/* Startup only. Title/file path are copied; plugin path is borrowed during configure. */
typedef struct http_example_config {
  uint16_t port;
  const char *title;
  const char *file_path;
  /* NULL selects native; otherwise load exactly this DSO, without fallback. */
  const char *plugin_path;
  uint32_t shutdown_timeout_ms;
  chttp_server_deadlines deadlines;
  chttp_rate_limit_config rate;
} http_example_config;

/* Private example owner; keep at a stable address until close releases it. */
typedef struct http_example_app {
  crpc_server rpc;
  chttp_rate_limiter limiter;
  chttp_service service;
  chttp_web_renderer renderer;
  cmeta_plugin_registry plugins;
  cmeta_plugin_ref plugin_ref;
  databind_plugin_client_11_HttpExample_16_CalculatorPlugin plugin_client;
  cflow_executor executor;
  uint32_t shutdown_timeout_ms;
  DataBind *contract;
  int shutdown_status;
  DataBindHttpMethodPlan *method_plan;
  DataBindNativeTypeBinding request_native;
  DataBindNativeTypeBinding response_native;
  DataBindServiceNativeBinding native;
  const cmeta_data_desc *page_desc;
  HomePage_t page;
  tstr file_path;
} http_example_app;

http_example_config http_example_config_default(void);
/* Requires a zero-initialized owner. Failure rolls back before returning. */
int http_example_configure(http_example_app *app, const http_example_config *config);
/* Incomplete stop/cleanup retains ownership for retry. A stopped runtime's
 * terminal error is returned after cleanup, with the owner reset to zero. */
int http_example_close(http_example_app *app);
int http_example_register_handlers(http_example_app *app);
int http_example_intercept(void *user, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_server_next *next);
int http_example_no_store(void *user, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_server_next *next);

#endif
