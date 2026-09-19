#include <chttp_web/web.h>
#include "tinytest.h"

#include <string.h>

static chttp_web_renderer_config test_renderer_config(void) {
  return (chttp_web_renderer_config)CHTTP_WEB_RENDERER_CONFIG_INIT;
}

spec("CHttp::Web request context") {
  it("builds bounded borrowed views and resets cleanly between requests") {
    static const char *const header_names[] = {"X-Request-ID", "X-Missing"};
    static const char *const session_keys[] = {"theme"};
    static const chttp_header headers_a[] = {
        {"HX-Request", "true"},
        {"X-Request-ID", "req-a"}};
    static const chttp_server_param params_a[] = {{"id", "42"}};
    static const chttp_header headers_b[] = {
        {"X-Request-ID", "req-b"}};
    static const chttp_server_param params_b[] = {{"id", "7"}};

    chttp_web_named_value param_storage[1];
    chttp_web_named_value header_storage[2];
    chttp_web_named_value session_storage[1];
    chttp_web_request_context context;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    chttp_web_request_context_options options =
        (chttp_web_request_context_options)
            CHTTP_WEB_REQUEST_CONTEXT_OPTIONS_INIT;
    chttp_server_request_view request = {0};

    options.header_names = header_names;
    options.header_name_count = 2u;
    options.session_keys = session_keys;
    options.session_key_count = 1u;
    options.param_storage = param_storage;
    options.param_capacity = 1u;
    options.header_storage = header_storage;
    options.header_capacity = 2u;
    options.session_storage = session_storage;
    options.session_capacity = 1u;

    request.method = CHTTP_METHOD_GET;
    request.target = "/users/42?tab=summary";
    request.path = "/users/42";
    request.headers = headers_a;
    request.header_count = 2u;
    request.params = params_a;
    request.param_count = 1u;

    check_true(chttp_web_request_is_htmx(&request));
    check_equal(chttp_web_request_context_init(
                    &context, &request, &options, &error),
                CHTTP_WEB_OK);
    check_equal(context.method.size, (size_t)3u);
    check_equal(context.method.data, "GET", 3u);
    check_equal(context.path.data, "/users/42", 9u);
    check_true(context.htmx);
    check_false(context.session_available);
    check_equal(context.params.count, (size_t)1u);
    check_equal(param_storage[0].name.data, "id", 2u);
    check_equal(param_storage[0].value.data, "42", 2u);
    check_true(param_storage[0].present);
    check_true(header_storage[0].present);
    check_equal(header_storage[0].value.data, "req-a", 5u);
    check_false(header_storage[1].present);
    check_equal(header_storage[1].value.size, (size_t)0u);
    check_false(session_storage[0].present);

    request.target = "/users/7";
    request.path = "/users/7";
    request.headers = headers_b;
    request.header_count = 1u;
    request.params = params_b;
    request.param_count = 1u;

    check_false(chttp_web_request_is_htmx(&request));
    check_equal(chttp_web_request_context_init(
                    &context, &request, &options, &error),
                CHTTP_WEB_OK);
    check_false(context.htmx);
    check_equal(context.path.data, "/users/7", 8u);
    check_equal(param_storage[0].value.data, "7", 1u);
    check_equal(header_storage[0].value.data, "req-b", 5u);
    check_false(header_storage[1].present);
  }

  it("fails closed when caller storage cannot hold the bounded request") {
    static const chttp_server_param params[] = {{"id", "42"}};
    chttp_web_request_context context;
    chttp_web_request_context_options options =
        (chttp_web_request_context_options)
            CHTTP_WEB_REQUEST_CONTEXT_OPTIONS_INIT;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    chttp_server_request_view request = {0};

    request.method = CHTTP_METHOD_GET;
    request.target = "/users/42";
    request.path = "/users/42";
    request.params = params;
    request.param_count = 1u;

    check_equal(chttp_web_request_context_init(
                    &context, &request, &options, &error),
                CHTTP_WEB_CAPACITY);
    check_null(context.path.data);
    check_equal(context.params.count, (size_t)0u);
  }

  it("renders the typed request context without leaking Jinja types") {
    static const char page[] =
        "{{ method }}|{{ path }}|"
        "{% if htmx %}hx{% else %}full{% endif %}|"
        "{{ params[0].name }}={{ params[0].value }}|"
        "{% if headers[0].present %}rid={{ headers[0].value }}"
        "{% else %}no-rid{% endif %}|"
        "{% if session_available %}session{% else %}no-session{% endif %}";
    static const chttp_web_template templates[] = {
        {"page.html", page, sizeof(page) - 1u}};
    static const chttp_header headers[] = {
        {"HX-Request", "true"}, {"X-Request-ID", "req-a"}};
    static const chttp_server_param params[] = {{"id", "42"}};
    static const char *const header_names[] = {"X-Request-ID"};
    static const char expected[] =
        "GET|/users/42|hx|id=42|rid=req-a|no-session";

    chttp_web_renderer renderer = {0};
    chttp_web_renderer_config config = test_renderer_config();
    chttp_web_named_value param_storage[1];
    chttp_web_named_value header_storage[1];
    chttp_web_request_context_options options =
        (chttp_web_request_context_options)
            CHTTP_WEB_REQUEST_CONTEXT_OPTIONS_INIT;
    chttp_web_request_context context;
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    chttp_server_request_view request = {0};
    char *html = NULL;
    size_t html_size = 0u;

    options.header_names = header_names;
    options.header_name_count = 1u;
    options.param_storage = param_storage;
    options.param_capacity = 1u;
    options.header_storage = header_storage;
    options.header_capacity = 1u;

    request.method = CHTTP_METHOD_GET;
    request.target = "/users/42";
    request.path = "/users/42";
    request.headers = headers;
    request.header_count = 2u;
    request.params = params;
    request.param_count = 1u;

    check_true(cmeta_data_desc_valid(chttp_web_request_context_data()));
    check_equal(chttp_web_request_context_init(
                    &context, &request, &options, &error),
                CHTTP_WEB_OK);
    check_equal(chttp_web_renderer_init(
                    &renderer, templates, 1u, &config, &error),
                CHTTP_WEB_OK);
    check_equal(chttp_web_render(
                    &renderer, "page.html",
                    chttp_web_request_context_data(), &context,
                    &html, &html_size, &error),
                CHTTP_WEB_OK);
    check_not_null(html);
    check_equal(html_size, sizeof(expected) - 1u);
    check_equal(html, expected, sizeof(expected) - 1u);

    chttp_web_output_free(html);
    chttp_web_renderer_destroy(&renderer);
  }
}
