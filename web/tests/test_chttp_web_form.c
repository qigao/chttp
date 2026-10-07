#include <chttp_web/web.h>

#include "tinytest.h"

#include <string.h>

static chttp_web_status parse_form(
    const char *body, size_t max_input, size_t max_pairs,
    size_t max_decoded, chttp_web_form_pair *pairs, size_t pair_capacity,
    char *bytes, size_t byte_capacity, chttp_web_form *form,
    chttp_web_error *error) {
  chttp_web_form_parse_options options =
      (chttp_web_form_parse_options)CHTTP_WEB_FORM_PARSE_OPTIONS_INIT;
  options.max_input_bytes = max_input;
  options.max_pairs = max_pairs;
  options.max_decoded_bytes = max_decoded;
  options.pair_storage = pairs;
  options.pair_capacity = pair_capacity;
  options.byte_storage = bytes;
  options.byte_capacity = byte_capacity;
  return chttp_web_form_parse(
      body, body ? strlen(body) : 0u, &options, form, error);
}

static int view_equals(chttp_web_string_view value, const char *expected) {
  const size_t size = strlen(expected);
  return value.size == size &&
         (size == 0u || memcmp(value.data, expected, size) == 0);
}

spec("CHttp::Web bounded form parsing") {
  it("percent-decodes plus, escapes, repeated keys, and exact bounds") {
    static const char body[] =
        "name=Alice+Smith&tag=A%26B%3DC&tag=two";
    chttp_web_form_pair pairs[3];
    char bytes[30];
    chttp_web_form form = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    const size_t decoded = 29u;

    check_equal(
        parse_form(body, sizeof(body) - 1u, 3u, decoded,
                   pairs, 3u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_OK);
    check_equal(form.pair_count, 3u);
    check_equal(form.decoded_bytes, decoded);
    check_true(view_equals(form.pairs[0].name, "name"));
    check_true(view_equals(form.pairs[0].value, "Alice Smith"));
    check_equal(chttp_web_form_count(&form, "tag"), 2u);
    check_true(view_equals(
        chttp_web_form_get(&form, "tag", 0u)->value, "A&B=C"));
    check_true(view_equals(
        chttp_web_form_get(&form, "tag", 1u)->value, "two"));

    form = (chttp_web_form){0};
    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    check_equal(
        parse_form(body, sizeof(body) - 1u, 3u, decoded - 1u,
                   pairs, 3u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_CAPACITY);
    check_equal(form.pair_count, 0u);

    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    check_equal(
        parse_form(body, sizeof(body) - 2u, 3u, decoded,
                   pairs, 3u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_CAPACITY);
  }

  it("rejects malformed encoding, empty segments, and one-pair-over") {
    chttp_web_form_pair pairs[2];
    char bytes[32];
    chttp_web_form form = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    check_equal(
        parse_form("a=%GG", 5u, 2u, 32u,
                   pairs, 2u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_FORM);
    check_equal(form.pair_count, 0u);

    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    check_equal(
        parse_form("a=1&&b=2", 8u, 2u, 32u,
                   pairs, 2u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_FORM);

    error = (chttp_web_error)CHTTP_WEB_ERROR_INIT;
    check_equal(
        parse_form("a=1&b=2", 7u, 1u, 32u,
                   pairs, 2u, bytes, sizeof(bytes), &form, &error),
        CHTTP_WEB_CAPACITY);
  }
}
