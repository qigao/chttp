#include "http_example_app.h"

/* Prepare headers before continuing: handlers may commit their replies. */
int http_example_intercept(void *user, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_server_next *next) {
  (void)user;
  (void)request;
  int status = chttp_server_response_set_header(response, "X-Example", "rpc-middleware");
  if (status == SALTS_OK)
    status = chttp_server_response_set_header(response, "X-Content-Type-Options", "nosniff");
  return status == SALTS_OK ? chttp_server_next_call(next) : status;
}

/* Route-local policy leaves /file's conditional caching intact. */
int http_example_no_store(void *user, const chttp_server_request_view *request,
    chttp_server_response *response, chttp_server_next *next) {
  (void)user;
  (void)request;
  const int status = chttp_server_response_set_header(response, "Cache-Control", "no-store");
  return status == SALTS_OK ? chttp_server_next_call(next) : status;
}
