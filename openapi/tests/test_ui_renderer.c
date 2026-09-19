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

static int expect_bytes(const char *actual, size_t actual_size,
                        const char *expected, size_t expected_size) {
    if (actual && actual_size == expected_size &&
        memcmp(actual, expected, expected_size) == 0)
        return 1;
    fprintf(stderr, "rendered bytes mismatch: actual=%zu expected=%zu\n",
            actual_size, expected_size);
    fputs("actual hex:", stderr);
    if (actual) {
        for (size_t i = 0u; i < actual_size; ++i)
            fprintf(stderr, " %02x", (unsigned char)actual[i]);
    }
    fputs("\nexpected hex:", stderr);
    for (size_t i = 0u; i < expected_size; ++i)
        fprintf(stderr, " %02x", (unsigned char)expected[i]);
    fputc('\n', stderr);
    return 0;
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
    oa_ui_renderer_error error = OA_UI_RENDERER_ERROR_INIT;
    json_value_t *root = json_parse(DOCUMENT, strlen(DOCUMENT));
    REQUIRE(root);

    oa_error model_error = {{0}};
    oa_ui_model *model = oa_ui_model_create_json(root, &model_error);
    json_free(root);
    REQUIRE(model);
    const oa_ui_document *document = oa_ui_model_view(model);
    REQUIRE(document);

    oa_ui_renderer renderer = {0};
    oa_ui_renderer_config config = (oa_ui_renderer_config)OA_UI_RENDERER_CONFIG_INIT;
    REQUIRE(oa_ui_renderer_init(&renderer, document,
        vstr_from_cstr("docs/index.html"), vstr_from_cstr(TEMPLATE),
        &config, &error) == OA_UI_RENDERER_OK);

    char *html = NULL;
    size_t html_size = 0u;
    REQUIRE(oa_ui_renderer_render(&renderer, &html, &html_size, &error) ==
            OA_UI_RENDERER_OK);
    REQUIRE(html);
    static const char EXPECTED[] =
        "&lt;Pets &amp; Co&gt;|get /pets[read](q)";
    if (html_size != sizeof(EXPECTED) - 1u ||
        memcmp(html, EXPECTED,
               html_size < sizeof(EXPECTED) - 1u ? html_size : sizeof(EXPECTED) - 1u) != 0) {
        fprintf(stderr, "renderer output size=%zu expected=%zu bytes=", html_size,
                sizeof(EXPECTED) - 1u);
        for (size_t i = 0u; i < html_size; ++i)
            fprintf(stderr, "%02x", (unsigned)(unsigned char)html[i]);
        fputc('\n', stderr);
    }
    REQUIRE(expect_bytes(html, html_size, EXPECTED, sizeof(EXPECTED) - 1u));
    oa_ui_renderer_output_free(html);
    html = NULL;

    /* A compiled renderer is reusable for sequential owner-thread requests. */
    REQUIRE(oa_ui_renderer_render(&renderer, &html, &html_size, &error) ==
            OA_UI_RENDERER_OK);
    REQUIRE(expect_bytes(html, html_size, EXPECTED, sizeof(EXPECTED) - 1u));
    oa_ui_renderer_output_free(html);
    html = NULL;

    oa_ui_renderer_destroy(&renderer);
    oa_ui_renderer_destroy(&renderer);

    oa_ui_renderer invalid = {0};
    error = (oa_ui_renderer_error)OA_UI_RENDERER_ERROR_INIT;
    REQUIRE(oa_ui_renderer_init(&invalid, document,
        vstr_from_cstr("bad.html"), vstr_from_cstr("{% if %}"),
        &config, &error) == OA_UI_RENDERER_TEMPLATE);
    REQUIRE(invalid.impl == NULL);
    oa_ui_renderer_destroy(&invalid);

    oa_ui_renderer bounded = {0};
    oa_ui_renderer_config small = config;
    small.max_output_bytes = 8u;
    error = (oa_ui_renderer_error)OA_UI_RENDERER_ERROR_INIT;
    REQUIRE(oa_ui_renderer_init(&bounded, document,
        vstr_from_cstr("small.html"), vstr_from_cstr(TEMPLATE),
        &small, &error) == OA_UI_RENDERER_OK);
    html_size = 99u;
    REQUIRE(oa_ui_renderer_render(&bounded, &html, &html_size, &error) ==
            OA_UI_RENDERER_CAPACITY);
    REQUIRE(html == NULL);
    REQUIRE(html_size == 0u);
    oa_ui_renderer_destroy(&bounded);

    oa_ui_renderer oversized = {0};
    oa_ui_renderer_config tiny_template = config;
    tiny_template.max_template_bytes = 4u;
    error = (oa_ui_renderer_error)OA_UI_RENDERER_ERROR_INIT;
    REQUIRE(oa_ui_renderer_init(&oversized, document,
        vstr_from_cstr("oversized.html"), vstr_from_cstr(TEMPLATE),
        &tiny_template, &error) == OA_UI_RENDERER_CAPACITY);
    REQUIRE(oversized.impl == NULL);
    oa_ui_renderer_destroy(&oversized);

    oa_ui_renderer binary = {0};
    error = (oa_ui_renderer_error)OA_UI_RENDERER_ERROR_INIT;
    REQUIRE(oa_ui_renderer_init(&binary, document,
        vstr_from_cstr("binary.html"),
        vstr_from_buf(BINARY_TEMPLATE, sizeof(BINARY_TEMPLATE)),
        &config, &error) == OA_UI_RENDERER_OK);
    html_size = 99u;
    REQUIRE(oa_ui_renderer_render(&binary, &html, &html_size, &error) ==
            OA_UI_RENDERER_OK);
    REQUIRE(html != NULL);
    REQUIRE(html_size == sizeof(BINARY_TEMPLATE));
    REQUIRE(memcmp(html, BINARY_TEMPLATE, sizeof(BINARY_TEMPLATE)) == 0);
    oa_ui_renderer_output_free(html);
    html = NULL;
    oa_ui_renderer_destroy(&binary);

    oa_ui_model_free(model);
    puts("openapi Jinja renderer passed");
    return 0;
}
