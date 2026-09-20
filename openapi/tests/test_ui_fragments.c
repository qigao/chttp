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

static int view_is(vstr value, const char *text) {
    const size_t size = strlen(text);
    return value.len == size &&
           (size == 0u || (value.data && memcmp(value.data, text, size) == 0));
}

static oa_ui_sequence_view select_one(const oa_ui_operation *operation) {
    return (oa_ui_sequence_view){
        operation, operation ? 1u : 0u,
        sizeof(oa_ui_operation), oa_ui_operation_cmeta_data()
    };
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

int main(void) {
    json_value_t *root = json_parse(DOCUMENT, strlen(DOCUMENT));
    REQUIRE(root);
    oa_error model_error = {{0}};
    oa_ui_model *model = oa_ui_model_create_json(root, &model_error);
    json_free(root);
    REQUIRE(model);

    const oa_ui_document *document = oa_ui_model_view(model);
    REQUIRE(document && document->operations.count == 3u);
    const oa_ui_operation *operations =
        (const oa_ui_operation *)document->operations.data;
    REQUIRE(view_is(operations[0].route_key, "listPets"));
    REQUIRE(view_is(operations[1].route_key, "op-1"));
    REQUIRE(view_is(operations[2].route_key, "op-2"));
    REQUIRE(oa_ui_document_find_operation(
        document, vstr_from_cstr("listPets")) == &operations[0]);
    REQUIRE(oa_ui_document_find_operation(
        document, vstr_from_cstr("op-2")) == &operations[2]);
    REQUIRE(oa_ui_document_find_operation(
        document, vstr_from_cstr("missing")) == NULL);

    static const char LAYOUT[] =
        "<html>{% block body %}{% endblock %}</html>";
    static const char DOCS[] =
        "{% extends \"layout.html\" %}{% block body %}{{ title }}|"
        "{% include \"operation_list.html\" %}|"
        "{% include \"operation_detail.html\" %}{% endblock %}";
    static const char LIST[] =
        "{% for op in operations %}{{ op.route_key }}="
        "{{ op.method }} {{ op.path }};{% endfor %}";
    static const char DETAIL[] =
        "{% for op in selected_operations %}DETAIL={{ op.method }} "
        "{{ op.path }}{% endfor %}";
    const chttp_web_template templates[] = {
        {"layout.html", LAYOUT, sizeof(LAYOUT) - 1u},
        {"docs.html", DOCS, sizeof(DOCS) - 1u},
        {"operation_list.html", LIST, sizeof(LIST) - 1u},
        {"operation_detail.html", DETAIL, sizeof(DETAIL) - 1u}
    };
    chttp_web_renderer renderer = {0};
    chttp_web_renderer_config config =
        (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    REQUIRE(chttp_web_renderer_init(
        &renderer, templates, 4u, &config, &error) == CHTTP_WEB_OK);

    oa_ui_document page = *document;
    page.selected_operations = select_one(&operations[0]);
    char *html = NULL;
    size_t html_size = 0u;
    REQUIRE(chttp_web_render(
        &renderer, "docs.html", oa_ui_document_cmeta_data(), &page,
        &html, &html_size, &error) == CHTTP_WEB_OK);
    static const char EXPECTED_DOCS[] =
        "<html>Pets|listPets=get /pets;op-1=post /pets;op-2=get /health;"
        "|DETAIL=get /pets</html>";
    REQUIRE(html_size == sizeof(EXPECTED_DOCS) - 1u);
    REQUIRE(memcmp(html, EXPECTED_DOCS, html_size) == 0);
    chttp_web_output_free(html);
    html = NULL;

    page = *document;
    page.selected_operations = select_one(&operations[2]);
    REQUIRE(chttp_web_render(
        &renderer, "operation_detail.html", oa_ui_document_cmeta_data(), &page,
        &html, &html_size, &error) == CHTTP_WEB_OK);
    static const char EXPECTED_DETAIL[] = "DETAIL=get /health";
    REQUIRE(html_size == sizeof(EXPECTED_DETAIL) - 1u);
    REQUIRE(memcmp(html, EXPECTED_DETAIL, html_size) == 0);
    chttp_web_output_free(html);
    chttp_web_renderer_destroy(&renderer);
    oa_ui_model_free(model);

    root = json_parse(DUPLICATE_IDS, strlen(DUPLICATE_IDS));
    REQUIRE(root);
    model_error = (oa_error){{0}};
    model = oa_ui_model_create_json(root, &model_error);
    json_free(root);
    REQUIRE(model == NULL);
    REQUIRE(strstr(model_error.message, "route keys") != NULL);

    puts("openapi CHttp::Web fragment qualification passed");
    return 0;
}
