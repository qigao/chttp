#include <chttp_web/web.h>
#include <json_parser.h>
#include <openapi/ui_model.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { \
    if (!(x)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
        return 1; \
    } \
} while (0)

static const char DOCUMENT[] =
    "{\"openapi\":\"3.1.0\","
    "\"info\":{\"title\":\"Pets\",\"version\":\"1\"},"
    "\"paths\":{\"/pets\":{"
      "\"get\":{\"operationId\":\"listPets\",\"summary\":\"List pets\","
        "\"tags\":[\"pets\",\"read\"],"
        "\"responses\":{\"200\":{\"description\":\"OK\"}}},"
      "\"post\":{\"operationId\":\"createPet\",\"summary\":\"Create pet\","
        "\"tags\":[\"pets\",\"write\"],"
        "\"responses\":{\"201\":{\"description\":\"Created\"}}}"
    "}}}";

static int render_filter(
    chttp_web_renderer *renderer,
    const oa_ui_document *document,
    vstr query,
    oa_ui_operation *storage,
    size_t capacity,
    char **html,
    size_t *size) {
    oa_ui_sequence_view filtered = {0};
    oa_error filter_error = {{0}};
    if (!oa_ui_document_filter_operations(
            document, query, storage, capacity, &filtered, &filter_error)) {
        fprintf(stderr, "filter failed: %s\n", filter_error.message);
        return 0;
    }
    oa_ui_document page = *document;
    page.operations = filtered;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    return chttp_web_render(
        renderer, "operation_list.html", oa_ui_document_cmeta_data(), &page,
        html, size, &error) == CHTTP_WEB_OK;
}

int main(void) {
    json_value_t *root = json_parse(DOCUMENT, strlen(DOCUMENT));
    REQUIRE(root);
    oa_error model_error = {{0}};
    oa_ui_model *model = oa_ui_model_create_json(root, &model_error);
    json_free(root);
    REQUIRE(model);
    const oa_ui_document *document = oa_ui_model_view(model);
    REQUIRE(document);

    static const char LIST[] =
        "{% for op in operations %}{{ op.route_key }}="
        "{{ op.method }} {{ op.path }} {{ op.summary }};"
        "{% endfor %}";
    const chttp_web_template templates[] = {
        {"operation_list.html", LIST, sizeof(LIST) - 1u}
    };
    chttp_web_renderer renderer = {0};
    chttp_web_renderer_config config =
        (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    REQUIRE(chttp_web_renderer_init(
        &renderer, templates, 1u, &config, &error) == CHTTP_WEB_OK);

    oa_ui_operation storage[2] = {0};
    char *html = NULL;
    size_t size = 0u;

    REQUIRE(render_filter(
        &renderer, document, vstr_from_cstr("CREATE"),
        storage, 2u, &html, &size));
    REQUIRE(strstr(html, "createPet=post /pets Create pet;") != NULL);
    REQUIRE(strstr(html, "listPets=") == NULL);
    chttp_web_output_free(html);
    html = NULL;

    REQUIRE(render_filter(
        &renderer, document, vstr_from_cstr("read"),
        storage, 2u, &html, &size));
    REQUIRE(strstr(html, "listPets=get /pets List pets;") != NULL);
    REQUIRE(strstr(html, "createPet=") == NULL);
    chttp_web_output_free(html);
    html = NULL;

    REQUIRE(render_filter(
        &renderer, document, vstr_from_cstr(""),
        storage, 2u, &html, &size));
    REQUIRE(strstr(html, "listPets=") != NULL);
    REQUIRE(strstr(html, "createPet=") != NULL);
    chttp_web_output_free(html);
    html = NULL;

    char oversized[257];
    memset(oversized, 'x', sizeof(oversized));
    oa_ui_sequence_view filtered = {0};
    oa_error filter_error = {{0}};
    REQUIRE(!oa_ui_document_filter_operations(
        document, vstr_from_buf(oversized, sizeof(oversized)),
        storage, 2u, &filtered, &filter_error));
    REQUIRE(filtered.count == 0u);

    const char invalid_utf8[] = {(char)0xff};
    filter_error = (oa_error){{0}};
    REQUIRE(!oa_ui_document_filter_operations(
        document, vstr_from_buf(invalid_utf8, sizeof(invalid_utf8)),
        storage, 2u, &filtered, &filter_error));
    REQUIRE(filtered.count == 0u);

    chttp_web_renderer_destroy(&renderer);
    oa_ui_model_free(model);
    puts("openapi HTMX filter qualification through CHttp::Web passed");
    return 0;
}
