#include <chttp_web/web.h>
#include "tinytest.h"

#include <cmeta/struct.h>
#include <salts_cmeta_data.h>
#include <vstr.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>


static const cmeta_data_buffer_shape WEB_VSTR_SHAPE = {
  CMETA_DATA_BUFFER_BORROWED
};

static const cmeta_data_desc WEB_VSTR_DATA = {
  .struct_size = sizeof(cmeta_data_desc),
  .abi_version = CMETA_DATA_DESC_ABI_VERSION,
  .stable_id = "chttp.web.vstr.data",
  .display_name = "CHttp Web borrowed vstr",
  .kind = CMETA_DATA_STRING,
  .storage_type = &salts_vstr_cmeta_type,
  .shape = &WEB_VSTR_SHAPE,
  .buffer_ops = &salts_vstr_cmeta_buffer_ops
};

typedef struct web_test_model {
  vstr title;
} web_test_model;

static const cmeta_type_identity WEB_TEST_MODEL_ID =
    CMETA_TYPE_ID_ATOM_INIT("chttp.web.test.model");

static const cmeta_type_desc WEB_TEST_MODEL_TYPE = {
    "web_test_model", sizeof(web_test_model), _Alignof(web_test_model),
    CMETA_T_OBJECT, NULL, NULL, &WEB_TEST_MODEL_ID};

static const cmeta_field_desc WEB_TEST_MODEL_LAYOUT_FIELDS[] = {
    {"title", "vstr", offsetof(web_test_model, title),
     sizeof(((web_test_model *)0)->title), _Alignof(vstr), NULL, NULL}};

static const cmeta_struct_desc WEB_TEST_MODEL_LAYOUT = {
    "web_test_model", sizeof(web_test_model), _Alignof(web_test_model),
    WEB_TEST_MODEL_LAYOUT_FIELDS, 1u};

static cmeta_data_desc web_test_model_desc(
    cmeta_data_field_desc fields[1], cmeta_data_struct_shape *shape) {
  fields[0] = (cmeta_data_field_desc){
      "chttp.web.test.model.title", "title",
      offsetof(web_test_model, title), &WEB_VSTR_DATA};
  *shape = (cmeta_data_struct_shape){
      &WEB_TEST_MODEL_LAYOUT, fields, 1u};
  return (cmeta_data_desc){
      .struct_size = sizeof(cmeta_data_desc),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "chttp.web.test.model.data",
      .display_name = "CHttp Web test model",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &WEB_TEST_MODEL_TYPE,
      .shape = shape};
}

spec("CHttp::Web typed Jinja rendering core") {
  it("renders a fixed bundle deterministically with HTML autoescape") {
    static const char layout[] =
        "<!doctype html><html><body>{% block body %}{% endblock %}</body></html>";
    static const char page[] =
        "{% extends \"layout.html\" %}"
        "{% block body %}<h1>{{ title }}</h1>{% endblock %}";
    static const chttp_web_template templates[] = {
        {"layout.html", layout, sizeof(layout) - 1u},
        {"page.html", page, sizeof(page) - 1u}};
    static const char expected[] =
        "<!doctype html><html><body><h1>"
        "&lt;script&gt;alert(1)&lt;/script&gt; &amp; ok"
        "</h1></body></html>";

    chttp_web_renderer renderer = {0};
    chttp_web_renderer_config config =
        (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    cmeta_data_field_desc fields[1];
    cmeta_data_struct_shape shape;
    cmeta_data_desc desc = web_test_model_desc(fields, &shape);
    web_test_model model = {
        .title = vstr_from_cstr("<script>alert(1)</script> & ok")};
    char *html = NULL;
    size_t html_size = 0u;

    check(cmeta_data_desc_valid(&desc));
    check_equal(chttp_web_renderer_init(
                    &renderer, templates, 2u, &config, &error),
                CHTTP_WEB_OK);

    check_equal(chttp_web_render(
                    &renderer, "page.html", &desc, &model,
                    &html, &html_size, &error),
                CHTTP_WEB_OK);
    check_not_null(html);
    check_equal(html_size, sizeof(expected) - 1u);
    check_equal(html, expected, sizeof(expected) - 1u);
    chttp_web_output_free(html);
    html = NULL;

    check_equal(chttp_web_render(
                    &renderer, "page.html", &desc, &model,
                    &html, &html_size, &error),
                CHTTP_WEB_OK);
    check_equal(html_size, sizeof(expected) - 1u);
    check_equal(html, expected, sizeof(expected) - 1u);
    chttp_web_output_free(html);

    chttp_web_renderer_destroy(&renderer);
    chttp_web_renderer_destroy(&renderer);
  }

  it("fails closed for unknown, invalid, dependency-missing, and oversized templates") {
    static const char valid[] = "<p>{{ title }}</p>";
    static const char invalid[] = "{% if %}";
    static const char missing_dependency[] = "{% include \"missing.html\" %}";
    chttp_web_renderer_config config =
        (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    cmeta_data_field_desc fields[1];
    cmeta_data_struct_shape shape;
    cmeta_data_desc desc = web_test_model_desc(fields, &shape);
    web_test_model model = {.title = vstr_from_cstr("safe")};
    char *html = NULL;
    size_t html_size = 99u;

    {
      const chttp_web_template templates[] = {
          {"page.html", valid, sizeof(valid) - 1u}};
      chttp_web_renderer renderer = {0};
      check_equal(chttp_web_renderer_init(
                      &renderer, templates, 1u, &config, &error),
                  CHTTP_WEB_OK);
      check_equal(chttp_web_render(
                      &renderer, "missing.html", &desc, &model,
                      &html, &html_size, &error),
                  CHTTP_WEB_NOT_FOUND);
      check_null(html);
      check_equal(html_size, 0u);
      chttp_web_renderer_destroy(&renderer);
    }

    {
      const chttp_web_template templates[] = {
          {"bad.html", invalid, sizeof(invalid) - 1u}};
      chttp_web_renderer renderer = {0};
      error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
      check_equal(chttp_web_renderer_init(
                      &renderer, templates, 1u, &config, &error),
                  CHTTP_WEB_TEMPLATE);
      check_null(renderer.impl);
    }

    {
      const chttp_web_template templates[] = {
          {"page.html", missing_dependency, sizeof(missing_dependency) - 1u}};
      chttp_web_renderer renderer = {0};
      error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
      check_equal(chttp_web_renderer_init(
                      &renderer, templates, 1u, &config, &error),
                  CHTTP_WEB_OK);
      html_size = 99u;
      check_equal(chttp_web_render(
                      &renderer, "page.html", &desc, &model,
                      &html, &html_size, &error),
                  CHTTP_WEB_NOT_FOUND);
      check_null(html);
      check_equal(html_size, 0u);
      chttp_web_renderer_destroy(&renderer);
    }

    {
      const chttp_web_template templates[] = {
          {"page.html", valid, sizeof(valid) - 1u}};
      chttp_web_renderer renderer = {0};
      chttp_web_renderer_config tiny = config;
      tiny.max_template_bytes = 4u;
      error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
      check_equal(chttp_web_renderer_init(
                      &renderer, templates, 1u, &tiny, &error),
                  CHTTP_WEB_CAPACITY);
      check_null(renderer.impl);
    }
  }

  it("discards partial output when the output bound is exceeded") {
    static const char page[] =
        "<p>{{ title }} -- {{ title }} -- {{ title }}</p>";
    static const chttp_web_template templates[] = {
        {"page.html", page, sizeof(page) - 1u}};
    chttp_web_renderer renderer = {0};
    chttp_web_renderer_config config =
        (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    cmeta_data_field_desc fields[1];
    cmeta_data_struct_shape shape;
    cmeta_data_desc desc = web_test_model_desc(fields, &shape);
    web_test_model model = {.title = vstr_from_cstr("0123456789")};
    char *html = NULL;
    size_t html_size = 99u;

    config.max_output_bytes = 12u;
    check_equal(chttp_web_renderer_init(
                    &renderer, templates, 1u, &config, &error),
                CHTTP_WEB_OK);
    check_equal(chttp_web_render(
                    &renderer, "page.html", &desc, &model,
                    &html, &html_size, &error),
                CHTTP_WEB_CAPACITY);
    check_null(html);
    check_equal(html_size, 0u);
    chttp_web_renderer_destroy(&renderer);
  }

  it("renders hostile browser principal fields through the standalone descriptor") {
    static const char page[] =
        "<p>{{ subject }}</p><p>{{ display_name }}</p>";
    static const char expected[] =
        "<p>alice</p><p>&lt;Admin &amp; Alice&gt;</p>";
    static const chttp_web_template templates[] = {
        {"principal.html", page, sizeof(page) - 1u}};
    static const char subject[] = "alice";
    static const char display_name[] = "<Admin & Alice>";

    chttp_web_renderer renderer = {0};
    chttp_web_renderer_config config =
        (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    chttp_web_principal principal = CHTTP_WEB_PRINCIPAL_INIT;
    char *html = NULL;
    size_t html_size = 0u;

    principal.authenticated = true;
    principal.subject =
        (chttp_web_string_view){subject, sizeof(subject) - 1u};
    principal.role =
        (chttp_web_string_view){"admin", sizeof("admin") - 1u};
    principal.display_name =
        (chttp_web_string_view){
            display_name, sizeof(display_name) - 1u};

    check(cmeta_data_desc_valid(chttp_web_principal_data()));
    check_equal(chttp_web_renderer_init(
                    &renderer, templates, 1u, &config, &error),
                CHTTP_WEB_OK);
    check_equal(chttp_web_render(
                    &renderer, "principal.html",
                    chttp_web_principal_data(), &principal,
                    &html, &html_size, &error),
                CHTTP_WEB_OK);
    check_equal(html_size, sizeof(expected) - 1u);
    check_equal(html, expected, sizeof(expected) - 1u);
    chttp_web_output_free(html);
    chttp_web_renderer_destroy(&renderer);
  }

}
