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

static int bytes_equal(vstr value, const char *text) {
    size_t size = strlen(text);
    return value.len == size &&
           (size == 0u || (value.data && memcmp(value.data, text, size) == 0));
}

static const char DOCUMENT[] =
    "{\"openapi\":\"3.1.0\","
    "\"info\":{\"title\":\"Pets\",\"version\":\"1.2\"},"
    "\"paths\":{"
      "\"/pets\":{"
        "\"get\":{\"operationId\":\"listPets\","
          "\"summary\":\"List pets\","
          "\"responses\":{\"200\":{\"description\":\"OK\"}}},"
        "\"post\":{\"operationId\":\"unsafe/id\","
          "\"summary\":\"Create pet\","
          "\"responses\":{\"201\":{\"description\":\"Created\"}}}"
      "},"
      "\"/health\":{\"get\":{"
        "\"summary\":\"Health\","
        "\"responses\":{\"204\":{\"description\":\"Healthy\"}}"
      "}}"
    "}}";

static const char DUPLICATE_IDS[] =
    "{\"openapi\":\"3.1.0\","
    "\"info\":{\"title\":\"Dup\",\"version\":\"1\"},"
    "\"paths\":{"
      "\"/a\":{\"get\":{\"operationId\":\"same\","
        "\"responses\":{\"200\":{\"description\":\"OK\"}}}},"
      "\"/b\":{\"get\":{\"operationId\":\"same\","
        "\"responses\":{\"200\":{\"description\":\"OK\"}}}}"
    "}}";

static const oa_ui_renderer_template TEMPLATES[] = {
    {{"layout.html", 11u},
     {"<html>{% block body %}{% endblock %}</html>", 45u}},
    {{"docs.html", 9u},
     {"{% extends \"layout.html\" %}{% block body %}{{ title }}|"
      "{% include \"operation_list.html\" %}|"
      "{% include \"operation_detail.html\" %}{% endblock %}", 152u}},
    {{"operation_list.html", 19u},
     {"{% for op in operations %}{{ operation_keys[loop.index0] }}="
      "{{ op.method }} {{ op.path }};{% endfor %}", 105u}},
    {{"operation_detail.html", 21u},
     {"{% for op in selected_operations %}DETAIL={{ op.method }} "
      "{{ op.path }}{% endfor %}", 86u}}
};

int main(void) {
    oa_ui_renderer_error error = OA_UI_RENDERER_ERROR_INIT;
    json_value_t *root = json_parse(DOCUMENT, strlen(DOCUMENT));
    REQUIRE(root);

    oa_error model_error = {{0}};
    oa_ui_model *model = oa_ui_model_create_json(root, &model_error);
    json_free(root);
    REQUIRE(model);

    oa_ui_renderer renderer = {0};
    oa_ui_renderer_config config =
        (oa_ui_renderer_config)OA_UI_RENDERER_CONFIG_INIT;
    REQUIRE(oa_ui_renderer_init_bundle(
        &renderer, oa_ui_model_view(model),
        TEMPLATES, sizeof(TEMPLATES) / sizeof(TEMPLATES[0]),
        &config, &error) == OA_UI_RENDERER_OK);

    REQUIRE(oa_ui_renderer_operation_count(&renderer) == 3u);
    REQUIRE(bytes_equal(oa_ui_renderer_operation_key(&renderer, 0u), "listPets"));
    REQUIRE(bytes_equal(oa_ui_renderer_operation_key(&renderer, 1u), "op-1"));
    REQUIRE(bytes_equal(oa_ui_renderer_operation_key(&renderer, 2u), "op-2"));

    size_t selected = OA_UI_RENDERER_NO_SELECTION;
    REQUIRE(oa_ui_renderer_find_operation(
        &renderer, vstr_from_cstr("listPets"), &selected) ==
        OA_UI_RENDERER_OK);
    REQUIRE(selected == 0u);
    REQUIRE(oa_ui_renderer_find_operation(
        &renderer, vstr_from_cstr("op-2"), &selected) ==
        OA_UI_RENDERER_OK);
    REQUIRE(selected == 2u);

    char *html = NULL;
    size_t html_size = 0u;
    REQUIRE(oa_ui_renderer_render_named(
        &renderer, vstr_from_cstr("docs.html"), 0u,
        &html, &html_size, &error) == OA_UI_RENDERER_OK);
    static const char EXPECTED_DOCS[] =
        "<html>Pets|listPets=get /pets;op-1=post /pets;op-2=get /health;"
        "|DETAIL=get /pets</html>";
    REQUIRE(html_size == sizeof(EXPECTED_DOCS) - 1u);
    REQUIRE(memcmp(html, EXPECTED_DOCS, html_size) == 0);
    oa_ui_renderer_output_free(html);
    html = NULL;

    REQUIRE(oa_ui_renderer_render_named(
        &renderer, vstr_from_cstr("operation_detail.html"), 2u,
        &html, &html_size, &error) == OA_UI_RENDERER_OK);
    static const char EXPECTED_DETAIL[] = "DETAIL=get /health";
    REQUIRE(html_size == sizeof(EXPECTED_DETAIL) - 1u);
    REQUIRE(memcmp(html, EXPECTED_DETAIL, html_size) == 0);
    oa_ui_renderer_output_free(html);
    html = NULL;

    error = (oa_ui_renderer_error)OA_UI_RENDERER_ERROR_INIT;
    REQUIRE(oa_ui_renderer_render_named(
        &renderer, vstr_from_cstr("operation_detail.html"), 99u,
        &html, &html_size, &error) == OA_UI_RENDERER_INVALID_ARGUMENT);
    REQUIRE(html == NULL && html_size == 0u);

    oa_ui_renderer_destroy(&renderer);
    oa_ui_model_free(model);

    root = json_parse(DUPLICATE_IDS, strlen(DUPLICATE_IDS));
    REQUIRE(root);
    model = oa_ui_model_create_json(root, &model_error);
    json_free(root);
    REQUIRE(model);
    error = (oa_ui_renderer_error)OA_UI_RENDERER_ERROR_INIT;
    REQUIRE(oa_ui_renderer_init_bundle(
        &renderer, oa_ui_model_view(model),
        TEMPLATES, sizeof(TEMPLATES) / sizeof(TEMPLATES[0]),
        &config, &error) == OA_UI_RENDERER_INVALID_ARGUMENT);
    REQUIRE(renderer.impl == NULL);
    oa_ui_model_free(model);

    puts("openapi Jinja fragment renderer passed");
    return 0;
}
