#include <http_server/http.h>
#include <salts_fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { UI_TIMEOUT_MS = 5000, UI_CONNECTIONS = 16, UI_COMMANDS = 64,
       UI_HEADERS = 32, UI_HEADER_BYTES = 8192, UI_CHUNK_BYTES = 65536,
       UI_FILE_BYTES = 2 * 1024 * 1024, UI_PATH_BYTES = 4096, UI_PORT_MAX = 65535 };

static int serve(void *user, const chttp_server_request_view *request,
                 chttp_server_response *response) {
    int status = chttp_server_response_set_header(response, "X-Content-Type-Options", "nosniff");
    if (status != SALTS_OK) return status;
    return chttp_server_serve_file(response, request, user);
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s openapi.json ui-directory [port]\n", argv[0]);
        return EXIT_FAILURE;
    }
    unsigned long port = 0;
    if (argc == 4) {
        char *end;
        if (!argv[3][0] || strspn(argv[3], "0123456789") != strlen(argv[3])) return EXIT_FAILURE;
        port = strtoul(argv[3], &end, 10);
        if (*end || port > UI_PORT_MAX) return EXIT_FAILURE;
    }
    chttp_server_config config = {0};
    config.host = "127.0.0.1"; config.port = (uint16_t)port;
    config.backlog = UI_CONNECTIONS;
#if defined(_WIN32)
    config.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    config.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
    config.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
    config.network.connection_capacity = UI_CONNECTIONS;
    config.network.command_capacity = UI_COMMANDS;
    config.network.request_capacity = UI_COMMANDS;
    config.network.completion_batch_capacity = UI_CONNECTIONS;
    config.network.event_capacity = UI_COMMANDS;
    config.network.max_send_bytes = UI_CHUNK_BYTES;
    config.network.receive_buffer_bytes = UI_HEADER_BYTES;
    config.network.connect_timeout_ms = UI_TIMEOUT_MS;
    config.network.read_timeout_ms = UI_TIMEOUT_MS;
    config.network.write_timeout_ms = UI_TIMEOUT_MS;
    config.route_capacity = UI_CONNECTIONS;
    config.middleware_capacity = 1;
    config.max_route_middleware_count = 1;
    config.max_route_param_count = 1;
    config.max_route_param_bytes = UI_PATH_BYTES;
    config.max_target_bytes = UI_PATH_BYTES;
    config.max_header_count = UI_HEADERS;
    config.max_header_bytes = UI_HEADER_BYTES;
    config.max_request_body_bytes = UI_CHUNK_BYTES;
    config.max_response_header_count = UI_HEADERS;
    config.max_response_header_bytes = UI_HEADER_BYTES;
    config.max_response_body_bytes = UI_FILE_BYTES;
    config.max_buffered_response_body_bytes = UI_HEADER_BYTES;
    config.poll_slice_ms = 1;

    const char *routes[] = {"/docs", "/docs/", "/docs/style.css", "/docs/app.js", "/docs/alpine.js", "/openapi.json"};
    const char *files[] = {"index.html", "index.html", "style.css", "app.js", "vendor/alpine-3.14.9.min.js"};
    const char *types[] = {"text/html; charset=utf-8", "text/html; charset=utf-8", "text/css; charset=utf-8", "text/javascript; charset=utf-8", "text/javascript; charset=utf-8", "application/json"};
    enum { UI_ROUTES = sizeof(routes) / sizeof(routes[0]) };
    char paths[UI_ROUTES][UI_PATH_BYTES];
    chttp_server_file_options options[UI_ROUTES] = {0};
    /* Fixed route-to-file mappings stay alive until successful shutdown. No
     * request-derived filesystem paths; assets must remain immutable while serving. */
    for (size_t i = 0; i < UI_ROUTES; ++i) {
        int size = i < UI_ROUTES - 1 ? snprintf(paths[i], sizeof(paths[i]), "%s/%s", argv[2], files[i])
                                    : snprintf(paths[i], sizeof(paths[i]), "%s", argv[1]);
        if (size < 0 || (size_t)size >= sizeof(paths[i])) { fputs("Asset path too long\n", stderr); return EXIT_FAILURE; }
        options[i].path = paths[i]; options[i].content_type = types[i];
        salts_fs_stat_t metadata = {0};
        if (salts_fs_stat(paths[i], &metadata) != SALTS_OK || !metadata.is_file || metadata.size > UI_FILE_BYTES) {
            fprintf(stderr, "Asset must be a readable regular file of at most 2 MiB: %s\n", paths[i]);
            return EXIT_FAILURE;
        }
    }
    chttp_server server = {0};
    int status = chttp_server_init(&server, &config);
    for (size_t i = 0; status == SALTS_OK && i < UI_ROUTES; ++i)
        status = chttp_server_get(&server, routes[i], serve, &options[i]);
    if (status == SALTS_OK) status = chttp_server_start(&server);
    uint16_t bound_port = 0;
    if (status == SALTS_OK) status = chttp_server_port(&server, &bound_port);
    if (status == SALTS_OK) {
        printf("OpenAPI UI: http://127.0.0.1:%u/docs\nPress Enter to stop.\n", (unsigned)bound_port);
        fflush(stdout);
        (void)getchar();
    }
    if (server.impl) {
        int stopped = chttp_server_stop(&server, UI_TIMEOUT_MS);
        if (status == SALTS_OK) status = stopped;
        if (stopped == SALTS_OK) {
            int destroyed = chttp_server_destroy(&server);
            if (status == SALTS_OK) status = destroyed;
        }
    }
    if (status != SALTS_OK) fprintf(stderr, "OpenAPI UI server failed: %d\n", status);
    return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
