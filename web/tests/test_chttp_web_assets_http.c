#include <chttp_web/web.h>
#include <http_client/http.h>

#include "tinytest.h"

#include <salts/error_codes.h>
#include <salts_fs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  ASSETS_HTTP_TIMEOUT_MS = 5000,
  ASSETS_HTTP_BODY_BYTES = 64 * 1024,
  ASSETS_HTTP_PATH_BYTES = 1024
};

static native_io_backend_kind assets_http_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config assets_http_network(size_t connections) {
  return (cnet_client_config){
      .backend = assets_http_backend(),
      .connection_capacity = connections,
      .command_capacity = 16u,
      .request_capacity = 16u,
      .completion_batch_capacity = 8u,
      .event_capacity = 32u,
      .max_send_bytes = ASSETS_HTTP_BODY_BYTES + 8192u,
      .receive_buffer_bytes = 4096u,
      .connect_timeout_ms = ASSETS_HTTP_TIMEOUT_MS,
      .read_timeout_ms = ASSETS_HTTP_TIMEOUT_MS,
      .write_timeout_ms = ASSETS_HTTP_TIMEOUT_MS};
}

static chttp_server_config assets_http_server_config(void) {
  return (chttp_server_config){
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = assets_http_network(4u),
      .route_capacity = 4u,
      .middleware_capacity = 2u,
      .max_route_middleware_count = 1u,
      .max_route_param_count = 1u,
      .max_route_param_bytes = 128u,
      .max_target_bytes = 4096u,
      .max_header_count = 16u,
      .max_header_bytes = 4096u,
      .max_request_body_bytes = 4096u,
      .max_response_header_count = 24u,
      .max_response_header_bytes = 8192u,
      .max_response_body_bytes = ASSETS_HTTP_BODY_BYTES,
      .max_buffered_response_body_bytes = 4096u,
      .buffer_capacity_bytes = 2u * 1024u * 1024u,
      .poll_slice_ms = 1u,
      .enable_http2 = 1,
      .h2_stream_capacity = 8u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_output_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
}

static chttp_client_config assets_http_client_config(void) {
  return (chttp_client_config){
      .network = assets_http_network(4u),
      .request_capacity = 4u,
      .max_start_line_bytes = 4096u,
      .max_header_count = 24u,
      .max_header_bytes = 8192u,
      .max_request_body_bytes = 4096u,
      .max_response_body_bytes = ASSETS_HTTP_BODY_BYTES,
      .max_informational_responses = 2u,
      .h2_input_buffer_bytes = 64u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
}

static int assets_http_write(const char *path, const char *bytes) {
  salts_fs_buf_t buffer = {(char *)bytes, strlen(bytes)};
  return salts_fs_write_file(path, &buffer);
}

static int assets_http_path(
    char output[ASSETS_HTTP_PATH_BYTES],
    const char *root,
    const char *relative) {
  const int written = snprintf(
      output, ASSETS_HTTP_PATH_BYTES, "%s/%s", root, relative);
  return written > 0 && written < ASSETS_HTTP_PATH_BYTES
             ? SALTS_OK
             : SALTS_ENAMETOOLONG;
}

static bool assets_http_immutable(void *user, const char *relative_path) {
  (void)user;
  return relative_path != NULL &&
         strcmp(relative_path, "app.01234567.js") == 0;
}

static const char *assets_http_etag(void *user, const char *relative_path) {
  (void)user;
  return relative_path != NULL &&
                 strcmp(relative_path, "app.01234567.js") == 0
             ? "\"asset-v1\""
             : NULL;
}

static int assets_http_api_ping(
    void *user,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
  (void)user;
  (void)request;
  return chttp_server_reply(
      response, 200u, "text/plain", "api", sizeof("api") - 1u);
}

static int assets_http_call(
    chttp_client *client,
    const char *uri,
    chttp_method method,
    const char *target,
    const chttp_header *headers,
    size_t header_count,
    chttp_protocol protocol,
    chttp_response *response) {
  chttp_options options = {
      .connection_uri = uri,
      .authority = "127.0.0.1",
      .target = target,
      .headers = headers,
      .header_count = header_count,
      .timeout_ms = ASSETS_HTTP_TIMEOUT_MS,
      .protocol = protocol};
  chttp_error error = {0};

  switch (method) {
  case CHTTP_METHOD_GET:
    return chttp_get(client, &options, response, &error);
  case CHTTP_METHOD_HEAD:
    return chttp_head(client, &options, response, &error);
  case CHTTP_METHOD_POST:
    return chttp_post(client, &options, response, &error);
  default:
    return SALTS_EINVAL;
  }
}

spec("CHttp::Web safe asset mount") {
  it("serves bounded assets over H1/H2 without swallowing API or asset misses") {
    static const char css[] = "body{}";
    static const char js[] = "console.log(1);";
    static const char shell[] = "<main>shell</main>";
    static const char secret[] = "outside-secret";
    static const chttp_header range_header[] = {
        {"Range", "bytes=0-3"}};
    static const chttp_header conditional_header[] = {
        {"If-None-Match", "\"asset-v1\""}};

    char *root = tt_make_temp_dir("chttp-web-assets-");
    char *outside = tt_make_temp_dir("chttp-web-assets-outside-");
    char css_dir[ASSETS_HTTP_PATH_BYTES];
    char css_path[ASSETS_HTTP_PATH_BYTES];
    char js_path[ASSETS_HTTP_PATH_BYTES];
    char shell_path[ASSETS_HTTP_PATH_BYTES];
    char outside_path[ASSETS_HTTP_PATH_BYTES];
    char link_path[ASSETS_HTTP_PATH_BYTES];
    chttp_web_asset_mount mount =
        (chttp_web_asset_mount)CHTTP_WEB_ASSET_MOUNT_INIT;
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = assets_http_server_config();
    chttp_client_config client_config = assets_http_client_config();
    chttp_response css_h1 = {0};
    chttp_response css_h2 = {0};
    chttp_response head = {0};
    chttp_response ranged = {0};
    chttp_response immutable = {0};
    chttp_response conditional = {0};
    chttp_response traversal = {0};
    chttp_response encoded_traversal = {0};
    chttp_response encoded_separator = {0};
    chttp_response invalid_utf8 = {0};
    chttp_response missing = {0};
    chttp_response symlink_escape = {0};
    chttp_response shell_route = {0};
    chttp_response api = {0};
    chttp_response api_missing = {0};
    chttp_response wrong_method = {0};
    char uri[64];
    uint16_t port = 0u;
    int symlink_status;

    check_not_null(root);
    check_not_null(outside);
    check_equal(assets_http_path(css_dir, root, "css"), SALTS_OK);
    check_equal(assets_http_path(css_path, root, "css/app.css"), SALTS_OK);
    check_equal(assets_http_path(js_path, root, "app.01234567.js"), SALTS_OK);
    check_equal(assets_http_path(shell_path, root, "index.html"), SALTS_OK);
    check_equal(assets_http_path(outside_path, outside, "secret.txt"), SALTS_OK);
    check_equal(assets_http_path(link_path, root, "escape.txt"), SALTS_OK);
    check_equal(salts_fs_mkdir(css_dir, 0700), SALTS_OK);
    check_equal(assets_http_write(css_path, css), SALTS_OK);
    check_equal(assets_http_write(js_path, js), SALTS_OK);
    check_equal(assets_http_write(shell_path, shell), SALTS_OK);
    check_equal(assets_http_write(outside_path, secret), SALTS_OK);
    symlink_status = salts_fs_symlink(outside_path, link_path, 0);

    mount.url_prefix = "/assets";
    mount.filesystem_root = root;
    mount.max_relative_path_bytes = 256u;
    mount.cache_control = "public, max-age=60";
    mount.immutable_cache_control =
        "public, max-age=31536000, immutable";
    mount.immutable = assets_http_immutable;
    mount.etag = assets_http_etag;
    mount.spa_url_prefix = "/app";
    mount.spa_fallback_relative_path = "index.html";

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_web_assets_use(&server, &mount), SALTS_OK);
    check_equal(
        chttp_server_get(&server, "/api/ping", assets_http_api_ping, NULL),
        SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(
        uri, sizeof(uri), "tcp://127.0.0.1:%u",
        (unsigned int)port) > 0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/css/app.css",
        NULL, 0u, CHTTP_HTTP_1_1, &css_h1), SALTS_OK);
    check_equal(css_h1.status_code, 200u);
    check_equal(css_h1.body, css, sizeof(css) - 1u);
    check_equal(
        chttp_response_header(&css_h1, "Content-Type"),
        "text/css; charset=utf-8",
        strlen("text/css; charset=utf-8"));
    check_equal(
        chttp_response_header(&css_h1, "Cache-Control"),
        "public, max-age=60", strlen("public, max-age=60"));

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/css/app.css",
        NULL, 0u, CHTTP_HTTP_2, &css_h2), SALTS_OK);
    check_equal(css_h2.status_code, 200u);
    check_equal(css_h2.body, css, sizeof(css) - 1u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_HEAD, "/assets/css/app.css",
        NULL, 0u, CHTTP_HTTP_1_1, &head), SALTS_OK);
    check_equal(head.status_code, 200u);
    check_equal(head.body_size, (size_t)0u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/css/app.css",
        range_header, 1u, CHTTP_HTTP_1_1, &ranged), SALTS_OK);
    check_equal(ranged.status_code, 206u);
    check_equal(ranged.body, "body", 4u);
    check_not_null(chttp_response_header(&ranged, "Content-Range"));

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/app.01234567.js",
        NULL, 0u, CHTTP_HTTP_1_1, &immutable), SALTS_OK);
    check_equal(immutable.status_code, 200u);
    check_equal(
        chttp_response_header(&immutable, "ETag"),
        "\"asset-v1\"", strlen("\"asset-v1\""));
    check_equal(
        chttp_response_header(&immutable, "Cache-Control"),
        "public, max-age=31536000, immutable",
        strlen("public, max-age=31536000, immutable"));

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/app.01234567.js",
        conditional_header, 1u, CHTTP_HTTP_1_1, &conditional), SALTS_OK);
    check_equal(conditional.status_code, 304u);
    check_equal(conditional.body_size, (size_t)0u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/../index.html",
        NULL, 0u, CHTTP_HTTP_1_1, &traversal), SALTS_OK);
    check_equal(traversal.status_code, 404u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/%2e%2e/index.html",
        NULL, 0u, CHTTP_HTTP_1_1, &encoded_traversal), SALTS_OK);
    check_equal(encoded_traversal.status_code, 404u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/css%2fapp.css",
        NULL, 0u, CHTTP_HTTP_1_1, &encoded_separator), SALTS_OK);
    check_equal(encoded_separator.status_code, 404u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/%ff.js",
        NULL, 0u, CHTTP_HTTP_1_1, &invalid_utf8), SALTS_OK);
    check_equal(invalid_utf8.status_code, 404u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/assets/missing.js",
        NULL, 0u, CHTTP_HTTP_1_1, &missing), SALTS_OK);
    check_equal(missing.status_code, 404u);

    if (symlink_status == SALTS_OK) {
      check_equal(assets_http_call(
          &client, uri, CHTTP_METHOD_GET, "/assets/escape.txt",
          NULL, 0u, CHTTP_HTTP_1_1, &symlink_escape), SALTS_OK);
      check_equal(symlink_escape.status_code, 404u);
    } else {
      info("symlink creation unavailable at runtime: %d", symlink_status);
    }

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/app/dashboard",
        NULL, 0u, CHTTP_HTTP_2, &shell_route), SALTS_OK);
    check_equal(shell_route.status_code, 200u);
    check_equal(shell_route.body, shell, sizeof(shell) - 1u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/api/ping",
        NULL, 0u, CHTTP_HTTP_1_1, &api), SALTS_OK);
    check_equal(api.status_code, 200u);
    check_equal(api.body, "api", 3u);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_GET, "/api/missing",
        NULL, 0u, CHTTP_HTTP_1_1, &api_missing), SALTS_OK);
    check_equal(api_missing.status_code, 404u);
    check_true(api_missing.body_size != sizeof(shell) - 1u ||
               memcmp(api_missing.body, shell, sizeof(shell) - 1u) != 0);

    check_equal(assets_http_call(
        &client, uri, CHTTP_METHOD_POST, "/assets/css/app.css",
        NULL, 0u, CHTTP_HTTP_1_1, &wrong_method), SALTS_OK);
    check_equal(wrong_method.status_code, 405u);
    check_equal(
        chttp_response_header(&wrong_method, "Allow"),
        "GET, HEAD", strlen("GET, HEAD"));

    chttp_response_destroy(&wrong_method);
    chttp_response_destroy(&api_missing);
    chttp_response_destroy(&api);
    chttp_response_destroy(&shell_route);
    chttp_response_destroy(&symlink_escape);
    chttp_response_destroy(&missing);
    chttp_response_destroy(&invalid_utf8);
    chttp_response_destroy(&encoded_separator);
    chttp_response_destroy(&encoded_traversal);
    chttp_response_destroy(&traversal);
    chttp_response_destroy(&conditional);
    chttp_response_destroy(&immutable);
    chttp_response_destroy(&ranged);
    chttp_response_destroy(&head);
    chttp_response_destroy(&css_h2);
    chttp_response_destroy(&css_h1);

    check_equal(
        chttp_client_destroy(&client, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
    check_equal(
        chttp_server_stop(&server, ASSETS_HTTP_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(tt_remove_tree(root), 0);
    check_equal(tt_remove_tree(outside), 0);
    free(root);
    free(outside);
  }
}
