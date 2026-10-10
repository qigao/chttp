#include "http_example_app.h"

static int example_page(void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  http_example_app *app = user;
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  (void)request;
  const chttp_web_status status = chttp_web_render_response(&app->renderer,
      response, "page.html", app->page_desc, &app->page, 200u, NULL, &error);
  return status == CHTTP_WEB_OK ? SALTS_OK : SALTS_EIO;
}

static int example_ping(void *user, const crpc_server_request_view *request,
    crpc_server_response *response) {
  (void)user;
  (void)request;
  return crpc_server_response_result(response, NULL, NULL);
}

static int example_file(void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  const http_example_app *app = user;
  const chttp_server_file_options options = {.path = app->file_path};
  return chttp_server_serve_file(response, request, &options);
}

int http_example_register_handlers(http_example_app *app) {
  chttp_server *server = crpc_server_http(&app->rpc);
  const chttp_server_middleware policy = {http_example_no_store, NULL};
  const chttp_server_route_options home = {.method = CHTTP_METHOD_GET, .path = "/",
      .middleware = &policy, .middleware_count = 1u, .handler = example_page, .user = app};
  const crpc_method ping = {.service = "example", .name = "ping"};
  int status = chttp_server_route_with(server, &home);
  if (status == SALTS_OK)
    status = crpc_server_register(&app->rpc, "/rpc", &ping, example_ping, NULL);
  if (status == SALTS_OK && app->file_path != NULL)
    status = chttp_server_get(server, "/file", example_file, app);
  return status;
}
