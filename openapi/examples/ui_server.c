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
    UI_TEMPLATE_FILE_BYTES = 64 * 1024,
    UI_SEARCH_BYTES = 256,
    UI_SEND_BYTES = UI_FILE_BYTES + UI_HEADER_BYTES + 16 * 1024,
    UI_COMMAND_BUFFER_BYTES = 4 * 1024 * 1024,
    UI_BUFFER_CAPACITY_BYTES = 32 * 1024 * 1024,
    UI_PATH_BYTES = 4096,
    UI_PORT_MAX = 65535
};

static const char *const UI_TEMPLATE_NAMES[] = {
    "layout.html",
    "docs.html",
    "operation_list.html",
    "operation_detail.html",
    "parameter.html",
    "response.html"
};
enum {
    UI_TEMPLATE_COUNT =
        sizeof(UI_TEMPLATE_NAMES) / sizeof(UI_TEMPLATE_NAMES[0])
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

    if ((size != 0u && fread(bytes, 1u, size, file) != size) ||
        fgetc(file) != EOF) {
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

static int response_nosniff(chttp_server_response *response) {
    return chttp_server_response_set_header(
        response, "X-Content-Type-Options", "nosniff");
}

static int serve_file(void *user, const chttp_server_request_view *request,
                      chttp_server_response *response) {
    int status = response_nosniff(response);
    if (status != SALTS_OK) return status;
    return chttp_server_serve_file(
        response, request, (const chttp_server_file_options *)user);
}

static int reply_render_failure(
    chttp_server_response *response,
    oa_ui_renderer_status rendered,
    const oa_ui_renderer_error *error) {
    static const char body[] = "OpenAPI UI render failed\n";
    fprintf(stderr, "OpenAPI UI render failed: status=%d message=%s\n",
            (int)rendered, error ? error->message : "");
    return chttp_server_reply(
        response, 500u, "text/plain; charset=utf-8",
        body, sizeof(body) - 1u);
}

static int reply_named(
    oa_ui_renderer *renderer,
    const char *template_name,
    size_t selected_operation,
    chttp_server_response *response) {
    oa_ui_renderer_error error = OA_UI_RENDERER_ERROR_INIT;
    char *html = NULL;
    size_t html_size = 0u;
    int status = response_nosniff(response);
    if (status != SALTS_OK) return status;

    oa_ui_renderer_status rendered = oa_ui_renderer_render_named(
        renderer, vstr_from_cstr(template_name), selected_operation,
        &html, &html_size, &error);
    if (rendered != OA_UI_RENDERER_OK)
        return reply_render_failure(response, rendered, &error);

    status = chttp_server_reply(
        response, 200u, "text/html; charset=utf-8", html, html_size);
    oa_ui_renderer_output_free(html);
    return status;
}

static int serve_docs(void *user, const chttp_server_request_view *request,
                      chttp_server_response *response) {
    (void)request;
    oa_ui_renderer *renderer = (oa_ui_renderer *)user;
    const size_t selected =
        oa_ui_renderer_operation_count(renderer) != 0u
            ? 0u : OA_UI_RENDERER_NO_SELECTION;
    return reply_named(renderer, "docs.html", selected, response);
}

static int search_hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int parse_search_query(
    const chttp_server_request_view *request,
    char output[UI_SEARCH_BYTES + 1u],
    size_t *out_size) {
    const char *input;
    size_t size = 0u;

    if (!request || !request->target || !output || !out_size)
        return SALTS_EINVAL;
    output[0] = '\0';
    *out_size = 0u;

    input = strchr(request->target, '?');
    if (!input || input[1] == '\0') return SALTS_OK;
    ++input;
    if (input[0] != 'q' || input[1] != '=') return SALTS_EINVAL;
    input += 2;

    while (*input) {
        unsigned char value;
        if (*input == '&') return SALTS_EINVAL;
        if (*input == '+') {
            value = (unsigned char)' ';
            ++input;
        } else if (*input == '%') {
            if (input[1] == '\0' || input[2] == '\0')
                return SALTS_EINVAL;
            const int high = search_hex_value(input[1]);
            const int low = search_hex_value(input[2]);
            if (high < 0 || low < 0) return SALTS_EINVAL;
            value = (unsigned char)((high << 4) | low);
            input += 3;
        } else {
            value = (unsigned char)*input++;
        }

        if (value == 0u) return SALTS_EINVAL;
        if (size == UI_SEARCH_BYTES) return SALTS_EMSGSIZE;
        output[size++] = (char)value;
    }

    output[size] = '\0';
    if (vstr_utf8_invalid_offset(vstr_from_buf(output, size)) != VSTR_NPOS)
        return SALTS_EINVAL;
    *out_size = size;
    return SALTS_OK;
}

static int serve_operation_list(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
    static const char bad_query[] = "Invalid OpenAPI operation search query\n";
    oa_ui_renderer *renderer = (oa_ui_renderer *)user;
    char query[UI_SEARCH_BYTES + 1u];
    size_t query_size = 0u;

    int status = response_nosniff(response);
    if (status != SALTS_OK) return status;
    if (parse_search_query(request, query, &query_size) != SALTS_OK)
        return chttp_server_reply(
            response, 400u, "text/plain; charset=utf-8",
            bad_query, sizeof(bad_query) - 1u);

    oa_ui_renderer_error error = OA_UI_RENDERER_ERROR_INIT;
    char *html = NULL;
    size_t html_size = 0u;
    const oa_ui_renderer_status rendered =
        oa_ui_renderer_render_operation_list(
            renderer, vstr_from_buf(query, query_size),
            &html, &html_size, &error);
    if (rendered != OA_UI_RENDERER_OK)
        return reply_render_failure(response, rendered, &error);

    status = chttp_server_reply(
        response, 200u, "text/html; charset=utf-8", html, html_size);
    oa_ui_renderer_output_free(html);
    return status;
}

static int serve_operation_detail(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
    static const char not_found[] = "OpenAPI operation not found\n";
    oa_ui_renderer *renderer = (oa_ui_renderer *)user;
    const char *key = chttp_server_request_param(request, "key");
    size_t selected = OA_UI_RENDERER_NO_SELECTION;

    int status = response_nosniff(response);
    if (status != SALTS_OK) return status;
    if (!key)
        return chttp_server_reply(
            response, 404u, "text/plain; charset=utf-8",
            not_found, sizeof(not_found) - 1u);

    const oa_ui_renderer_status found = oa_ui_renderer_find_operation(
        renderer, vstr_from_cstr(key), &selected);
    if (found == OA_UI_RENDERER_NOT_FOUND)
        return chttp_server_reply(
            response, 404u, "text/plain; charset=utf-8",
            not_found, sizeof(not_found) - 1u);
    if (found != OA_UI_RENDERER_OK) {
        static const char failed[] = "OpenAPI UI lookup failed\n";
        fprintf(stderr, "OpenAPI UI operation lookup failed: status=%d\n",
                (int)found);
        return chttp_server_reply(
            response, 500u, "text/plain; charset=utf-8",
            failed, sizeof(failed) - 1u);
    }

    oa_ui_renderer_error error = OA_UI_RENDERER_ERROR_INIT;
    char *html = NULL;
    size_t html_size = 0u;
    const oa_ui_renderer_status rendered = oa_ui_renderer_render_named(
        renderer, vstr_from_cstr("operation_detail.html"), selected,
        &html, &html_size, &error);
    if (rendered != OA_UI_RENDERER_OK)
        return reply_render_failure(response, rendered, &error);

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
    size_t document_size = 0u;
    char *template_storage[UI_TEMPLATE_COUNT] = {0};
    oa_ui_renderer_template templates[UI_TEMPLATE_COUNT] = {0};
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

    document_bytes = load_file_bounded(
        argv[1], UI_FILE_BYTES, &document_size);
    if (!document_bytes) {
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
    free(document_bytes);
    document_bytes = NULL;
    if (!model) {
        fprintf(stderr, "OpenAPI UI model failed: %s\n", model_error.message);
        status = SALTS_EINVAL;
        goto cleanup;
    }

    char template_paths[UI_TEMPLATE_COUNT][UI_PATH_BYTES];
    for (size_t i = 0u; i < UI_TEMPLATE_COUNT; ++i) {
        const int path_size = snprintf(
            template_paths[i], sizeof(template_paths[i]),
            "%s/templates/%s", argv[2], UI_TEMPLATE_NAMES[i]);
        if (path_size < 0 || (size_t)path_size >= sizeof(template_paths[i])) {
            fputs("Asset path too long\n", stderr);
            status = SALTS_EINVAL;
            goto cleanup;
        }

        size_t source_size = 0u;
        template_storage[i] = load_file_bounded(
            template_paths[i], UI_TEMPLATE_FILE_BYTES, &source_size);
        if (!template_storage[i]) {
            status = SALTS_EINVAL;
            goto cleanup;
        }
        templates[i] = (oa_ui_renderer_template){
            vstr_from_cstr(UI_TEMPLATE_NAMES[i]),
            vstr_from_buf(template_storage[i], source_size)
        };
    }

    oa_ui_renderer_config renderer_config =
        (oa_ui_renderer_config)OA_UI_RENDERER_CONFIG_INIT;
    renderer_config.max_output_bytes = UI_FILE_BYTES;
    oa_ui_renderer_error renderer_error = OA_UI_RENDERER_ERROR_INIT;
    const oa_ui_renderer_status renderer_status = oa_ui_renderer_init_bundle(
        &renderer, oa_ui_model_view(model),
        templates, UI_TEMPLATE_COUNT, &renderer_config, &renderer_error);
    for (size_t i = 0u; i < UI_TEMPLATE_COUNT; ++i) {
        free(template_storage[i]);
        template_storage[i] = NULL;
    }
    if (renderer_status != OA_UI_RENDERER_OK) {
        fprintf(stderr,
                "OpenAPI UI renderer startup failed: status=%d message=%s\n",
                (int)renderer_status, renderer_error.message);
        status = SALTS_EINVAL;
        goto cleanup;
    }

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
    config.network.max_send_bytes = UI_SEND_BYTES;
    config.network.command_buffer_bytes = UI_COMMAND_BUFFER_BYTES;
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
    config.max_buffered_response_body_bytes = UI_FILE_BYTES;
    config.buffer_capacity_bytes = UI_BUFFER_CAPACITY_BYTES;
    config.poll_slice_ms = 1u;

    const char *routes[] = {
        "/docs/style.css", "/docs/htmx.js",
        "/docs/tryit.js", "/openapi.json"
    };
    const char *files[] = {
        "style.css", "vendor/htmx-4.0.0.min.js",
        "tryit.js", NULL
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
        const int path_size = i < UI_STATIC_ROUTES - 1u
            ? snprintf(paths[i], sizeof(paths[i]), "%s/%s", argv[2], files[i])
            : snprintf(paths[i], sizeof(paths[i]), "%s", argv[1]);
        if (path_size < 0 || (size_t)path_size >= sizeof(paths[i])) {
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
    if (status == SALTS_OK)
        status = chttp_server_get(
            &server, "/docs/operations", serve_operation_list, &renderer);
    if (status == SALTS_OK)
        status = chttp_server_get(
            &server, "/docs/operations/:key", serve_operation_detail, &renderer);
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
        const int stopped = chttp_server_stop(&server, UI_TIMEOUT_MS);
        if (status == SALTS_OK) status = stopped;
        if (stopped == SALTS_OK) {
            const int destroyed = chttp_server_destroy(&server);
            if (status == SALTS_OK) status = destroyed;
        }
    }

    oa_ui_renderer_destroy(&renderer);
    oa_ui_model_free(model);
    json_free(root);
    free(document_bytes);
    for (size_t i = 0u; i < UI_TEMPLATE_COUNT; ++i)
        free(template_storage[i]);

    if (status != SALTS_OK)
        fprintf(stderr, "OpenAPI UI server failed: %d\n", status);
    return status == SALTS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
