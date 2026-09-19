#include "renderer.h"

#include <http_server/http.h>
#include <json_parser.h>
#include <openapi/ui_model.h>
#include <salts_fs.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    UI_TIMEOUT_MS = 5000,
    UI_CONNECTIONS = 16,
    UI_COMMANDS = 64,
    UI_HEADERS = 32,
    UI_HEADER_BYTES = 8192,
    UI_CHUNK_BYTES = 65536,
    UI_FILE_BYTES = 2 * 1024 * 1024,
    /* Buffered replies must fit one bounded CHTTP transport send. Keep the
     * current shell below that bound; #6 owns any larger SSR transport design. */
    UI_RENDER_BYTES = 48 * 1024,
    UI_PATH_BYTES = 4096,
    UI_PORT_MAX = 65535
};

static char *load_file_bounded(const char *path, size_t limit, size_t *out_size) {
    salts_fs_stat_t metadata = {0};
    FILE *file = NULL;
    char *bytes = NULL;
    size_t size;

    if (out_size) *out_size = 0u;
    if (!path || !out_size ||
        salts_fs_stat(path, &metadata) != SALTS_OK ||
        !metadata.is_file || metadata.size > limit) {
        fprintf(stderr,
                "Asset must be a readable regular file of at most %zu bytes: %s\n",
                limit, path ? path : "(null)");
        return NULL;
    }

    size = (size_t)metadata.size;
    file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Asset could not be opened: %s\n", path);
        return NULL;
    }

    bytes = (char *)malloc(size + 1u);
    if (!bytes) {
        fclose(file);
        fputs("Out of memory loading OpenAPI UI asset\n", stderr);
        return NULL;
    }

    if ((size != 0u && fread(bytes, 1u, size, file) != size) || fgetc(file) != EOF) {
        fprintf(stderr, "Asset changed or could not be read completely: %s\n", path);
        free(bytes);
        fclose(file);
        return NULL;
    }
    if (ferror(file)) {
        fprintf(stderr, "Asset read failed: %s\n", path);
        free(bytes);
        fclose(file);
        return NULL;
    }

    bytes[size] = '\0';
    fclose(file);
    *out_size = size;
    return bytes;
}

static int serve_file(void *user, const chttp_server_request_view *request,
                      chttp_server_response *response) {
    int status = chttp_server_response_set_header(
        response, "X-Content-Type-Options", "nosniff");
    if (status != SALTS_OK) return status;
    return chttp_server_serve_file(
        response, request, (const chttp_server_file_options *)user);
}

static int serve_docs(void *user, const chttp_server_request_view *request,
                      chttp_server_response *response) {
    (void)request;
    oa_ui_renderer *renderer = (oa_ui_renderer *)user;
    oa_ui_renderer_error error = OA_UI_RENDERER_ERROR_INIT;
    char *html = NULL;
    size_t html_size = 0u;

    int status = chttp_server_response_set_header(
        response, "X-Content-Type-Options", "nosniff");
    if (status != SALTS_OK) return status;

    oa_ui_renderer_status rendered =
        oa_ui_renderer_render(renderer, &html, &html_size, &error);
    if (rendered != OA_UI_RENDERER_OK) {
        static const char body[] = "OpenAPI UI render failed\n";
        fprintf(stderr, "OpenAPI UI render failed: status=%d message=%s\n",
                (int)rendered, error.message);
        return chttp_server_reply(
            response, 500u, "text/plain; charset=utf-8",
            body, sizeof(body) - 1u);
    }

    status = chttp_server_reply(
        response, 200u, "text/html; charset=utf-8", html, html_size);
    oa_ui_renderer_output_free(html);
    return status;
}

int main(int argc, char **argv) {
    chttp_server server = {0};
    oa_ui_renderer renderer = {0};
    oa_ui_model *model = NULL;
    json_value_t *root = NULL;
    char *document_bytes = NULL;
    char *template_bytes = NULL;
    size_t document_size = 0u;
    size_t template_size = 0u;
    int status = SALTS_OK;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s openapi.json ui-directory [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    unsigned long port = 0u;
    if (argc == 4) {
        char *end;
        if (!argv[3][0] ||
            strspn(argv[3], "0123456789") != strlen(argv[3]))
            return EXIT_FAILURE;
        port = strtoul(argv[3], &end, 10);
        if (*end || port > UI_PORT_MAX) return EXIT_FAILURE;
    }

    char index_path[UI_PATH_BYTES];
    int index_length = snprintf(
        index_path, sizeof(index_path), "%s/index.html", argv[2]);
    if (index_length < 0 || (size_t)index_length >= sizeof(index_path)) {
        fputs("Asset path too long\n", stderr);
        return EXIT_FAILURE;
    }

    document_bytes =
        load_file_bounded(argv[1], UI_FILE_BYTES, &document_size);
    template_bytes =
        load_file_bounded(index_path, UI_FILE_BYTES, &template_size);
    if (!document_bytes || !template_bytes) {
        status = SALTS_EINVAL;
        goto cleanup;
    }

    root = json_parse(document_bytes, document_size);
    if (!root) {
        fprintf(stderr, "OpenAPI document parse failed: %s\n", json_get_error());
        status = SALTS_EINVAL;
        goto cleanup;
    }

    oa_error model_error = {{0}};
    model = oa_ui_model_create_json(root, &model_error);
    json_free(root);
    root = NULL;
    if (!model) {
        fprintf(stderr, "OpenAPI UI model failed: %s\n", model_error.message);
        status = SALTS_EINVAL;
        goto cleanup;
    }

    oa_ui_renderer_config renderer_config =
        (oa_ui_renderer_config)OA_UI_RENDERER_CONFIG_INIT;
    renderer_config.max_output_bytes = UI_RENDER_BYTES;
    oa_ui_renderer_error renderer_error = OA_UI_RENDERER_ERROR_INIT;
    oa_ui_renderer_status renderer_status = oa_ui_renderer_init(
        &renderer, oa_ui_model_view(model),
        vstr_from_cstr("docs/index.html"),
        vstr_from_buf(template_bytes, template_size),
        &renderer_config, &renderer_error);
    free(template_bytes);
    template_bytes = NULL;
    if (renderer_status != OA_UI_RENDERER_OK) {
        fprintf(stderr, "OpenAPI UI renderer startup failed: status=%d message=%s\n",
                (int)renderer_status, renderer_error.message);
        status = SALTS_EINVAL;
        goto cleanup;
    }

    /* Renderer compilation copied the template source; the model owns its
     * immutable JSON snapshot. Neither input file buffer is retained. */
    free(document_bytes);
    document_bytes = NULL;

    chttp_server_config config = {0};
    config.host = "127.0.0.1";
    config.port = (uint16_t)port;
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
    config.middleware_capacity = 1u;
    config.max_route_middleware_count = 1u;
    config.max_route_param_count = 1u;
    config.max_route_param_bytes = UI_PATH_BYTES;
    config.max_target_bytes = UI_PATH_BYTES;
    config.max_header_count = UI_HEADERS;
    config.max_header_bytes = UI_HEADER_BYTES;
    config.max_request_body_bytes = UI_CHUNK_BYTES;
    config.max_response_header_count = UI_HEADERS;
    config.max_response_header_bytes = UI_HEADER_BYTES;
    config.max_response_body_bytes = UI_FILE_BYTES;
    /* Zero lets CHTTP derive the largest body that still fits max_send_bytes. */
    config.max_buffered_response_body_bytes = 0u;
    config.poll_slice_ms = 1u;

    const char *routes[] = {
        "/docs/style.css", "/docs/app.js", "/docs/alpine.js", "/openapi.json"
    };
    const char *files[] = {
        "style.css", "app.js", "vendor/alpine-3.14.9.min.js", NULL
    };
    const char *types[] = {
        "text/css; charset=utf-8",
        "text/javascript; charset=utf-8",
        "text/javascript; charset=utf-8",
        "application/json"
    };
    enum { UI_STATIC_ROUTES = sizeof(routes) / sizeof(routes[0]) };
    char paths[UI_STATIC_ROUTES][UI_PATH_BYTES];
    chttp_server_file_options options[UI_STATIC_ROUTES] = {0};

    /* Fixed route-to-file mappings stay alive until successful shutdown. No
     * request-derived filesystem paths; assets must remain immutable while serving. */
    for (size_t i = 0u; i < UI_STATIC_ROUTES; ++i) {
        int size = i < UI_STATIC_ROUTES - 1u
            ? snprintf(paths[i], sizeof(paths[i]), "%s/%s", argv[2], files[i])
            : snprintf(paths[i], sizeof(paths[i]), "%s", argv[1]);
        if (size < 0 || (size_t)size >= sizeof(paths[i])) {
            fputs("Asset path too long\n", stderr);
            status = SALTS_EINVAL;
            goto cleanup;
        }
        options[i].path = paths[i];
        options[i].content_type = types[i];

        salts_fs_stat_t metadata = {0};
        if (salts_fs_stat(paths[i], &metadata) != SALTS_OK ||
            !metadata.is_file || metadata.size > UI_FILE_BYTES) {
            fprintf(stderr,
                    "Asset must be a readable regular file of at most 2 MiB: %s\n",
                    paths[i]);
            status = SALTS_EINVAL;
            goto cleanup;
        }
    }

    status = chttp_server_init(&server, &config);
    if (status == SALTS_OK)
        status = chttp_server_get(&server, "/docs", serve_docs, &renderer);
    if (status == SALTS_OK)
        status = chttp_server_get(&server, "/docs/", serve_docs, &renderer);
    for (size_t i = 0u; status == SALTS_OK && i < UI_STATIC_ROUTES; ++i)
        status = chttp_server_get(&server, routes[i], serve_file, &options[i]);

    if (status == SALTS_OK) status = chttp_server_start(&server);
    uint16_t bound_port = 0u;
    if (status == SALTS_OK) status = chttp_server_port(&server, &bound_port);
    if (status == SALTS_OK) {
        printf("OpenAPI UI: http://127.0.0.1:%u/docs\nPress Enter to stop.\n",
               (unsigned)bound_port);
        fflush(stdout);
        (void)getchar();
    }

cleanup:
    if (server.impl) {
        int stopped = chttp_server_stop(&server, UI_TIMEOUT_MS);
        if (status == SALTS_OK) status = stopped;
        if (stopped == SALTS_OK) {
            int destroyed = chttp_server_destroy(&server);
            if (status == SALTS_OK) status = destroyed;
        }
    }

    oa_ui_renderer_destroy(&renderer);
    oa_ui_model_free(model);
    json_free(root);
    free(document_bytes);
    free(template_bytes);

    if (status != SALTS_OK)
        fprintf(stderr, "OpenAPI UI server failed: %d\n", status);
    return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
