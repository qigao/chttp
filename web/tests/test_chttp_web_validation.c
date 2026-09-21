#include <chttp_web/web.h>

#include "tinytest.h"

#include <string.h>

static void web_validation_init_or_fail(
    chttp_web_validation *validation,
    chttp_web_validation_error *errors,
    size_t error_capacity,
    char *bytes,
    size_t byte_capacity) {
  chttp_web_error error = CHTTP_WEB_ERROR_INIT;
  check_equal(
      chttp_web_validation_init(
          validation, errors, error_capacity,
          bytes, byte_capacity, &error),
      CHTTP_WEB_OK);
}

spec("CHttp::Web structured validation") {
  it("stores field and global errors deterministically with duplicate lookup") {
    chttp_web_validation validation = CHTTP_WEB_VALIDATION_INIT;
    chttp_web_validation_error errors[4];
    char bytes[128];
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    const chttp_web_validation_error *first;
    const chttp_web_validation_error *second;

    web_validation_init_or_fail(
        &validation, errors, 4u, bytes, sizeof(bytes));

    check_true(validation.valid);
    check_equal(validation.errors.count, (size_t)0u);

    check_equal(
        chttp_web_validation_add_field(
            &validation, "name", "required", &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_validation_add_global(
            &validation, "save failed", &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_validation_add_field(
            &validation, "name", "too short", &error),
        CHTTP_WEB_OK);

    check_false(validation.valid);
    check_equal(validation.errors.count, (size_t)3u);
    check_equal(
        chttp_web_validation_field_count(&validation, "name"),
        (size_t)2u);
    check_equal(
        chttp_web_validation_field_count(&validation, "missing"),
        (size_t)0u);
    check_equal(
        chttp_web_validation_global_count(&validation),
        (size_t)1u);

    first = chttp_web_validation_field_get(
        &validation, "name", 0u);
    second = chttp_web_validation_field_get(
        &validation, "name", 1u);
    check_not_null(first);
    check_not_null(second);
    check_equal(first->field.data, "name", 4u);
    check_equal(first->message.data, "required", 8u);
    check_false(first->global);
    check_equal(second->message.data, "too short", 9u);
    check_null(
        chttp_web_validation_field_get(
            &validation, "name", 2u));

    check_false(errors[1].field.data != NULL);
    check_equal(errors[1].field.size, (size_t)0u);
    check_equal(errors[1].message.data, "save failed", 11u);
    check_true(errors[1].global);
  }

  it("fails transactionally at entry and byte capacity") {
    chttp_web_validation validation = CHTTP_WEB_VALIDATION_INIT;
    chttp_web_validation_error one_error[1];
    char exact_bytes[12];
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    size_t used;

    web_validation_init_or_fail(
        &validation, one_error, 1u,
        exact_bytes, sizeof(exact_bytes));

    check_equal(
        chttp_web_validation_add_field(
            &validation, "name", "required", &error),
        CHTTP_WEB_OK);
    check_equal(validation.errors.count, (size_t)1u);
    check_equal(validation.byte_used, sizeof(exact_bytes));
    used = validation.byte_used;

    check_equal(
        chttp_web_validation_add_global(
            &validation, "second", &error),
        CHTTP_WEB_CAPACITY);
    check_equal(validation.errors.count, (size_t)1u);
    check_equal(validation.byte_used, used);
    check_false(validation.valid);

    {
      chttp_web_validation small = CHTTP_WEB_VALIDATION_INIT;
      chttp_web_validation_error storage[1];
      char bytes[11];

      web_validation_init_or_fail(
          &small, storage, 1u, bytes, sizeof(bytes));
      check_equal(
          chttp_web_validation_add_field(
              &small, "name", "required", &error),
          CHTTP_WEB_CAPACITY);
      check_true(small.valid);
      check_equal(small.errors.count, (size_t)0u);
      check_equal(small.byte_used, (size_t)0u);
    }
  }

  it("resets and reuses caller storage without request leakage") {
    chttp_web_validation validation = CHTTP_WEB_VALIDATION_INIT;
    chttp_web_validation_error errors[2];
    char bytes[64];
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    web_validation_init_or_fail(
        &validation, errors, 2u, bytes, sizeof(bytes));
    check_equal(
        chttp_web_validation_add_field(
            &validation, "old", "old error", &error),
        CHTTP_WEB_OK);

    check_equal(
        chttp_web_validation_reset(&validation, &error),
        CHTTP_WEB_OK);
    check_true(validation.valid);
    check_equal(validation.errors.count, (size_t)0u);
    check_equal(validation.byte_used, (size_t)0u);
    check_equal(
        chttp_web_validation_field_count(&validation, "old"),
        (size_t)0u);

    check_equal(
        chttp_web_validation_add_field(
            &validation, "new", "new error", &error),
        CHTTP_WEB_OK);
    check_equal(validation.errors.count, (size_t)1u);
    check_equal(errors[0].field.data, "new", 3u);
    check_equal(errors[0].message.data, "new error", 9u);
    check_equal(
        chttp_web_validation_field_count(&validation, "old"),
        (size_t)0u);
    check_equal(
        chttp_web_validation_field_count(&validation, "new"),
        (size_t)1u);
  }

  it("renders typed validation with HTML autoescape") {
    static const char page[] =
        "{% if valid %}valid{% else %}"
        "{% for item in errors %}"
        "[{% if item.global %}global{% else %}{{ item.field }}{% endif %}:"
        "{{ item.message }}]"
        "{% endfor %}{% endif %}";
    static const chttp_web_template templates[] = {
        {"validation.html", page, sizeof(page) - 1u}};
    static const char expected[] =
        "[name&lt;id&gt;:&lt;script&gt;alert(1)&lt;/script&gt;]"
        "[global:save &amp; retry]";

    chttp_web_renderer renderer = {0};
    chttp_web_renderer_config config =
        (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
    chttp_web_validation validation = CHTTP_WEB_VALIDATION_INIT;
    chttp_web_validation_error errors[4];
    char bytes[256];
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    char *html = NULL;
    size_t html_size = 0u;

    check_true(cmeta_data_desc_valid(chttp_web_validation_error_data()));
    check_true(cmeta_data_desc_valid(chttp_web_validation_data()));
    web_validation_init_or_fail(
        &validation, errors, 4u, bytes, sizeof(bytes));

    check_equal(
        chttp_web_validation_add_field(
            &validation,
            "name<id>",
            "<script>alert(1)</script>",
            &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_validation_add_global(
            &validation, "save & retry", &error),
        CHTTP_WEB_OK);

    check_equal(
        chttp_web_renderer_init(
            &renderer, templates, 1u, &config, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_render(
            &renderer,
            "validation.html",
            chttp_web_validation_data(),
            &validation,
            &html,
            &html_size,
            &error),
        CHTTP_WEB_OK);
    check_not_null(html);
    check_equal(html_size, sizeof(expected) - 1u);
    check_equal(html, expected, sizeof(expected) - 1u);

    chttp_web_output_free(html);
    chttp_web_renderer_destroy(&renderer);
  }
}
