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

static int expect_bytes(const char *actual, size_t actual_size,
                        const char *expected, size_t expected_size) {
    return actual && actual_size == expected_size &&
           memcmp(actual, expected, expected_size) == 0;
}

static const char DOCUMENT[] =
    "{\"openapi\":\"3.1.0\","
    "\"info\":{\"title\":\"<Pets & Co>\",\"version\":\"1\"},"
    "\"paths\":{\"/pets\":{\"get\":{"
      "\"operationId\":\"listPets\","
      "\"tags\":[\"read\"],"
      "\"parameters\":[{\"name\":\"q\",\"in\":\"query\","
        "\"required\":false,\"schema\":{\"type\":\"string\"}}],"
      "\"responses\":{\"200\":{\"description\":\"OK\"}}"
    "}}}}";

static const char BINARY_TEMPLATE[] = {'A', '\0', 'B'};

static const char TEMPLATE[] =
    "{{ title }}|"
    "{% for op in operations %}"
      "{{ op.method }} {{ op.path }}"
      "{% for tag in op.tags %}[{{ tag }}]{% endfor %}"
      "{% for p in op.parameters %}({{ p.name }}){% endfor %}"
    "{% endfor %}";

int main(void) {
    json_value_t *root = json_parse(DOCUMENT, strlen(DOCUMENT));
    REQUIRE(root);

    oa_error model_error = {{0}};
    oa_ui_model *model = oa_ui_model_create_json(root, &model_error);
    json_free(root);
    REQUIRE(model);
    const oa_ui_document *document = oa_ui_model_view(model);
    REQUIRE(document);

    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    chttp_web_renderer renderer = {0};
    chttp_web_renderer_config config =
        (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
    const chttp_web_template templates[] = {
        {"docs/index.html", TEMPLATE, sizeof(TEMPLATE) - 1u}
    };
    REQUIRE(chttp_web_renderer_init(
        &renderer, templates, 1u, &config, &error) == CHTTP_WEB_OK);

    char *html = NULL;
    size_t html_size = 0u;
    REQUIRE(chttp_web_render(
        &renderer, "docs/index.html", oa_ui_document_cmeta_data(), document,
        &html, &html_size, &error) == CHTTP_WEB_OK);
    static const char EXPECTED[] =
        "&lt;Pets &amp; Co&gt;|get /pets[read](q)";
    REQUIRE(expect_bytes(html, html_size, EXPECTED, sizeof(EXPECTED) - 1u));
    chttp_web_output_free(html);
    html = NULL;

    REQUIRE(chttp_web_render(
        &renderer, "docs/index.html", oa_ui_document_cmeta_data(), document,
        &html, &html_size, &error) == CHTTP_WEB_OK);
    REQUIRE(expect_bytes(html, html_size, EXPECTED, sizeof(EXPECTED) - 1u));
    chttp_web_output_free(html);
    html = NULL;
    chttp_web_renderer_destroy(&renderer);
    chttp_web_renderer_destroy(&renderer);

    const chttp_web_template invalid_templates[] = {
        {"bad.html", "{% if %}", sizeof("{% if %}") - 1u}
    };
    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    REQUIRE(chttp_web_renderer_init(
        &renderer, invalid_templates, 1u, &config, &error) ==
        CHTTP_WEB_TEMPLATE);
    REQUIRE(renderer.impl == NULL);

    chttp_web_renderer_config small = config;
    small.max_output_bytes = 8u;
    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    REQUIRE(chttp_web_renderer_init(
        &renderer, templates, 1u, &small, &error) == CHTTP_WEB_OK);
    html_size = 99u;
    REQUIRE(chttp_web_render(
        &renderer, "docs/index.html", oa_ui_document_cmeta_data(), document,
        &html, &html_size, &error) == CHTTP_WEB_CAPACITY);
    REQUIRE(html == NULL);
    REQUIRE(html_size == 0u);
    chttp_web_renderer_destroy(&renderer);

    chttp_web_renderer_config tiny_template = config;
    tiny_template.max_template_bytes = 4u;
    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    REQUIRE(chttp_web_renderer_init(
        &renderer, templates, 1u, &tiny_template, &error) ==
        CHTTP_WEB_CAPACITY);
    REQUIRE(renderer.impl == NULL);

    const chttp_web_template binary_templates[] = {
        {"binary.html", BINARY_TEMPLATE, sizeof(BINARY_TEMPLATE)}
    };
    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    REQUIRE(chttp_web_renderer_init(
        &renderer, binary_templates, 1u, &config, &error) == CHTTP_WEB_OK);
    html_size = 99u;
    REQUIRE(chttp_web_render(
        &renderer, "binary.html", oa_ui_document_cmeta_data(), document,
        &html, &html_size, &error) == CHTTP_WEB_OK);
    REQUIRE(html != NULL);
    REQUIRE(html_size == sizeof(BINARY_TEMPLATE));
    REQUIRE(memcmp(html, BINARY_TEMPLATE, sizeof(BINARY_TEMPLATE)) == 0);
    chttp_web_output_free(html);
    chttp_web_renderer_destroy(&renderer);

    oa_ui_model_free(model);
    puts("openapi CHttp::Web renderer qualification passed");
    return 0;
}
