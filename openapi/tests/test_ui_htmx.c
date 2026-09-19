#include "renderer.h"

#include <json_parser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { \
    if (!(x)) { \
        fprintf(stderr, "line %d: %s (status=%d message=%s)\n", \
                __LINE__, #x, (int)error.status, error.message); \
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

int main(void) {
    oa_ui_renderer_error error = OA_UI_RENDERER_ERROR_INIT;
    json_value_t *root = json_parse(DOCUMENT, strlen(DOCUMENT));
    REQUIRE(root);

    oa_error model_error = {{0}};
    oa_ui_model *model = oa_ui_model_create_json(root, &model_error);
    json_free(root);
    REQUIRE(model);

    const oa_ui_renderer_template templates[] = {
        {vstr_from_cstr("operation_list.html"),
         vstr_from_cstr(
             "{% for op in operations %}{{ operation_keys[loop.index0] }}="
             "{{ op.method }} {{ op.path }} {{ op.summary }};"
             "{% endfor %}")}
    };
    oa_ui_renderer renderer = {0};
    oa_ui_renderer_config config =
        (oa_ui_renderer_config)OA_UI_RENDERER_CONFIG_INIT;
    REQUIRE(oa_ui_renderer_init_bundle(
        &renderer, oa_ui_model_view(model),
        templates, sizeof(templates) / sizeof(templates[0]),
        &config, &error) == OA_UI_RENDERER_OK);

    char *html = NULL;
    size_t size = 0u;
    REQUIRE(oa_ui_renderer_render_operation_list(
        &renderer, vstr_from_cstr("CREATE"),
        &html, &size, &error) == OA_UI_RENDERER_OK);
    REQUIRE(strstr(html, "createPet=post /pets Create pet;") != NULL);
    REQUIRE(strstr(html, "listPets=") == NULL);
    oa_ui_renderer_output_free(html);
    html = NULL;

    REQUIRE(oa_ui_renderer_render_operation_list(
        &renderer, vstr_from_cstr("read"),
        &html, &size, &error) == OA_UI_RENDERER_OK);
    REQUIRE(strstr(html, "listPets=get /pets List pets;") != NULL);
    REQUIRE(strstr(html, "createPet=") == NULL);
    oa_ui_renderer_output_free(html);
    html = NULL;

    REQUIRE(oa_ui_renderer_render_operation_list(
        &renderer, vstr_from_cstr(""),
        &html, &size, &error) == OA_UI_RENDERER_OK);
    REQUIRE(strstr(html, "listPets=") != NULL);
    REQUIRE(strstr(html, "createPet=") != NULL);
    oa_ui_renderer_output_free(html);

    oa_ui_renderer_destroy(&renderer);
    oa_ui_model_free(model);
    puts("openapi HTMX filtering contract passed");
    return 0;
}
