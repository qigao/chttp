#include <openapi/generator.h>
#include <tree_sitter/api.h>
#include <json_parser.h>
#include <cyaml/cyaml_json_adapter.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const TSLanguage *tree_sitter_c(void);
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "line %d: %s (%s)\n", __LINE__, #x, error.message); return 1; } } while (0)
#define API(tags, declaration) "/**\n" tags "\n */\n" declaration "\n"
#define ROUTE(path, method, name) API(" * @route " method " " path "\n * @response 200 OK", "int " name "(void) { return 0; }")

/* Object order is not part of JSON/YAML semantic equality. */
static int same_value(const json_value_t *a, const json_value_t *b) {
    size_t i;
    if (!a || !b || json_type(a) != json_type(b)) return 0;
    switch (json_type(a)) {
    case JSON_NULL: return 1;
    case JSON_BOOL: return json_bool(a) == json_bool(b);
    case JSON_NUMBER: return json_number(a) == json_number(b);
    case JSON_STRING: return json_string_len(a) == json_string_len(b) && memcmp(json_string(a), json_string(b), json_string_len(a)) == 0;
    case JSON_ARRAY:
        if (json_array_size(a) != json_array_size(b)) return 0;
        for (i = 0; i < json_array_size(a); ++i)
            if (!same_value(json_array_get(a, i), json_array_get(b, i))) return 0;
        return 1;
    case JSON_OBJECT:
        if (json_object_size(a) != json_object_size(b)) return 0;
        for (i = 0; i < json_object_size(a); ++i)
            if (!same_value(json_object_value(a, i), json_object_get_v(b, json_object_key_v(a, i)))) return 0;
        return 1;
    }
    return 0;
}

static int rejected_unchanged(oa_document *doc, oa_plugin *plugin, const char *source,
                              const char *before, oa_error *error) {
    size_t length;
    char *after;
    int equal;
    error->message[0] = '\0';
    if (oa_document_add(doc, plugin, source, strlen(source), error) || !error->message[0]) return 0;
    after = oa_document_render(doc, "json", &length, error);
    equal = after && strcmp(before, after) == 0;
    oa_text_free(after);
    return equal;
}

static int string_is(const json_value_t *object, const char *key, const char *expected) {
    const char *actual = json_get_string(object, key);
    return actual && strcmp(actual, expected) == 0;
}

static int parameter_names(oa_plugin *plugin) {
    oa_error error = {{0}};
    const char *names[] = {"bad:name", "bad=name", "bad/name", "bad;name", "bad[name]", "\xe4\xb8\xad", "bad\x01name", "bad\x7fname"};
    const char *locations[] = {"header", "cookie"};
    oa_document *doc = oa_document_create("Names", "1", &error);
    REQUIRE(doc);
    size_t length;
    char *before = oa_document_render(doc, "json", &length, &error);
    REQUIRE(before);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        for (size_t j = 0; j < sizeof(locations) / sizeof(locations[0]); ++j) {
            char source[512];
            snprintf(source, sizeof(source), "/**\n@route GET /names\n@param %s %s string optional Name\n@response 200 OK\n*/\nint names(void) { return 1; }", names[i], locations[j]);
            REQUIRE(rejected_unchanged(doc, plugin, source, before, &error));
            REQUIRE(strstr(error.message, "parameter name"));
        }
    }
    const char *source = API("@route GET /names/{a:b}\n@param a:b path string required Path\n@param a:b query string optional Query\n@param X-Trace_1 header string optional Header\n@param session.id cookie string optional Cookie\n@minLength header.X-Trace_1 1\n@minLength cookie.session.id 1\n@response 200 OK", "int names(void) { return 1; }");
    REQUIRE(oa_document_add(doc, plugin, source, strlen(source), &error));
    oa_text_free(before); oa_document_free(doc);
    return 0;
}

static int media_types(oa_plugin *plugin) {
    oa_error error = {{0}};
    const char *valid[] = {"application/json", "application/vnd.api+json", "text/*",
        "Text/Plain;charset=utf-8", "text/plain;charset=\"utf-8\"", "text/plain;x=\"\"",
        "text/plain;x=\"a;b\";y=token", "text/plain;x=\"a\\\"b\"", "text/plain;;charset=utf-8;"};
    const char *invalid[] = {"application/json/extra", "/json", "text/", "text/plain,application/json",
        "text/plain;x", "text/plain;x=", "text/plain;=value", "text/plain;x=a=b",
        "text/plain;x=\"unterminated", "text/plain;x=\"a\"junk", "text/(plain)",
        "text/pl\xc3\xa4in", "text/plain;x=\"a\x01\"", "text/plain;x=\"a\\"};
    const char *tags[] = {"@body %s string required Body", "@produces 200 %s string"};
    for (size_t kind = 0; kind < sizeof(tags) / sizeof(tags[0]); ++kind) {
        for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
            oa_document *doc = oa_document_create("Media", "1", &error);
            char tag[256], source[512];
            snprintf(tag, sizeof(tag), tags[kind], valid[i]);
            snprintf(source, sizeof(source), "/**\n@route POST /media\n@response 200 OK\n%s\n*/\nint media(void) { return 1; }", tag);
            REQUIRE(doc && oa_document_add(doc, plugin, source, strlen(source), &error));
            size_t length;
            char *text = oa_document_render(doc, "json", &length, &error);
            REQUIRE(text);
            json_value_t *root = json_parse(text, length);
            json_value_t *op = json_object_get(json_object_get(json_object_get(root, "paths"), "/media"), "post");
            json_value_t *owner = kind ? json_object_get(json_object_get(op, "responses"), "200") : json_object_get(op, "requestBody");
            REQUIRE(json_object_get(json_object_get(owner, "content"), valid[i]));
            json_free(root); oa_text_free(text); oa_document_free(doc);
        }
        oa_document *doc = oa_document_create("Media", "1", &error);
        size_t length;
        REQUIRE(doc);
        char *before = oa_document_render(doc, "json", &length, &error);
        REQUIRE(before);
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            char tag[256], source[512];
            snprintf(tag, sizeof(tag), tags[kind], invalid[i]);
            snprintf(source, sizeof(source), "/**\n@route POST /media\n@response 200 OK\n%s\n*/\nint media(void) { return 1; }", tag);
            REQUIRE(rejected_unchanged(doc, plugin, source, before, &error));
            REQUIRE(strstr(error.message, "media type"));
        }
        oa_text_free(before); oa_document_free(doc);
    }
    return 0;
}

static int utf8_inputs(oa_plugin *plugin) {
    oa_error error = {{0}};
    const char *invalid[] = {"\x80", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82"};
    const char *unicode = "\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x90\xbe";
    oa_document *doc = oa_document_create(unicode, unicode, &error);
    REQUIRE(doc);
    size_t length;
    char *before = oa_document_render(doc, "json", &length, &error);
    REQUIRE(before);
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        REQUIRE(!oa_document_create(invalid[i], "1", &error));
        REQUIRE(strstr(error.message, "UTF-8"));
        REQUIRE(!oa_document_create("Title", invalid[i], &error));
        REQUIRE(strstr(error.message, "UTF-8"));
        char source[256];
        snprintf(source, sizeof(source), "/** @route GET /encoding\n@summary %s\n@response 200 OK\n*/\nint encoding(void) { return 1; }", invalid[i]);
        REQUIRE(rejected_unchanged(doc, plugin, source, before, &error));
        REQUIRE(strstr(error.message, "UTF-8"));
    }
    REQUIRE(rejected_unchanged(doc, plugin, "// \xe2\x82", before, &error));
    REQUIRE(strstr(error.message, "byte 3"));
    const char *source = API("@route GET /unicode\n@summary \xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x90\xbe\n@response 200 OK", "int unicode(void) { return 1; }");
    REQUIRE(oa_document_add(doc, plugin, source, strlen(source), &error));
    const char *formats[] = {"json", "yaml"};
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        char *text = oa_document_render(doc, formats[i], &length, &error);
        REQUIRE(text);
        cyaml_doc_t *yaml = i ? cyaml_parse(text, length, NULL, NULL) : NULL;
        json_value_t *parsed = i ? json_value_from_cyaml(yaml) : json_parse(text, length);
        REQUIRE(parsed && string_is(json_object_get(parsed, "info"), "title", unicode));
        REQUIRE(string_is(json_object_get(parsed, "info"), "version", unicode));
        REQUIRE(string_is(json_object_get(json_object_get(json_object_get(parsed, "paths"), "/unicode"), "get"), "summary", unicode));
        json_free(parsed);
        if (yaml) cyaml_free(yaml);
        oa_text_free(text);
    }
    oa_text_free(before);
    oa_document_free(doc);
    return 0;
}

static int numeric_text_preserved(oa_plugin *plugin) {
    oa_error error = {{0}};
    oa_document *doc = oa_document_create("Exact numbers", "1", &error);
    const char *source = API("@route POST /exact\n@body application/json number required Value\n@minimum body 9007199254740993\n@maximum body 9007199254740995\n@multipleOf body 0.000000000000000000123456789\n@response 200 OK", "int exact(void) { return 1; }");
    REQUIRE(doc && oa_document_add(doc, plugin, source, strlen(source), &error));
    const char *formats[] = {"json", "yaml"};
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        size_t length;
        char *text = oa_document_render(doc, formats[i], &length, &error);
        REQUIRE(text && strstr(text, "9007199254740993") && strstr(text, "9007199254740995"));
        REQUIRE(strstr(text, "0.000000000000000000123456789"));
        oa_text_free(text);
    }
    oa_document_free(doc);
    return 0;
}

/* Literal schemas catch wrong keyword types, destinations and JSON/YAML drift. */
static int standard_constraints(oa_plugin *plugin) {
    const struct { const char *tags; const char *schema; } cases[] = {
        {"@body application/json number required Value\n@minimum body -2.5\n@maximum body 10\n@exclusiveMinimum body -3\n@exclusiveMaximum body 11\n@multipleOf body 0.5",
         "{\"type\":\"number\",\"minimum\":-2.5,\"maximum\":10,\"exclusiveMinimum\":-3,\"exclusiveMaximum\":11,\"multipleOf\":0.5}"},
        {"@body application/json string required Value\n@minLength body 0\n@maxLength body 80\n@pattern body ^[a-z ]+$\n@format body email\n@enum body first value\n@enum body second",
         "{\"type\":\"string\",\"minLength\":0,\"maxLength\":80,\"pattern\":\"^[a-z ]+$\",\"format\":\"email\",\"enum\":[\"first value\",\"second\"]}"},
        {"@body application/json string[] required Value\n@minItems body 1\n@maxItems body 4\n@uniqueItems body true\n@minLength body[] 2",
         "{\"type\":\"array\",\"minItems\":1,\"maxItems\":4,\"uniqueItems\":true,\"items\":{\"type\":\"string\",\"minLength\":2}}"},
        {"@body application/json object required Value\n@field name string required Name\n@minLength body.name 1\n@maxLength body.name 80\n@minProperties body 1\n@maxProperties body 5\n@additionalProperties body false",
         "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Name\",\"minLength\":1,\"maxLength\":80}},\"required\":[\"name\"],\"minProperties\":1,\"maxProperties\":5,\"additionalProperties\":false}"},
        {"@body application/json int required Value\n@enum body 1\n@enum body 2",
         "{\"type\":\"integer\",\"enum\":[1,2]}"},
        {"@body application/json bool required Value\n@enum body true\n@enum body false",
         "{\"type\":\"boolean\",\"enum\":[true,false]}"},
        {"@body application/json number required Value\n@enum body 0.5\n@enum body -1.25",
         "{\"type\":\"number\",\"enum\":[0.5,-1.25]}"},
        {"@body application/json string required Value\n@maxLength body 9007199254740991\n@pattern body ^\\w+$",
         "{\"type\":\"string\",\"maxLength\":9007199254740991,\"pattern\":\"^\\\\w+$\"}"},
        {"@body application/json int[][] required Value\n@minimum body[][] 0\n@uniqueItems body false",
         "{\"type\":\"array\",\"uniqueItems\":false,\"items\":{\"type\":\"array\",\"items\":{\"type\":\"integer\",\"minimum\":0}}}"}
    };
    const char *invalid[] = {
        "@minLength body.name -1", "@minLength body.name 1.5",
        "@minLength body.name 9007199254740992", "@minLength body.name 2 extra",
        "@minimum body.name 1", "@maximum query.n nan", "@maximum query.n inf",
        "@maximum query.n 1e999", "@exclusiveMinimum query.n true",
        "@multipleOf query.n 0", "@multipleOf query.n -1",
        "@minimum missing 0", "@minimum n 0", "@minimum body.missing 0",
        "@minLength body.name[] 1", "@uniqueItems body.name true",
        "@additionalProperties body maybe", "@minLength body.name 1\n@minLength body.name 2",
        "@enum query.n 1.5", "@enum query.n 1\n@enum query.n 1",
        "@enum query.n 9007199254740991.1",
        "@enum body.name duplicate\n@enum body.name duplicate",
        "@enum body name", "@pattern body.name", "@format body.name",
        "@before authorize", "@constraint body.name minLength 1"
    };
    oa_error error = {{0}};
    enum { SOURCE_CAPACITY = 16384, ENUM_CAPACITY = 256 };
    char source[SOURCE_CAPACITY];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        oa_document *doc = oa_document_create("Constraints", "1", &error);
        size_t length;
        int size = snprintf(source, sizeof(source), "/**\n@route POST /value\n%s\n@response 200 OK\n*/\nint value(void) { return 1; }", cases[i].tags);
        REQUIRE(size > 0 && (size_t)size < sizeof(source));
        REQUIRE(doc && oa_document_add(doc, plugin, source, strlen(source), &error));
        char *text = oa_document_render(doc, "json", &length, &error);
        REQUIRE(text);
        json_value_t *root = json_parse(text, length);
        json_value_t *operation = json_object_get(json_object_get(json_object_get(root, "paths"), "/value"), "post");
        json_value_t *actual = json_object_get(json_object_get(json_object_get(json_object_get(operation, "requestBody"), "content"), "application/json"), "schema");
        json_value_t *expected = json_parse(cases[i].schema, strlen(cases[i].schema));
        REQUIRE(same_value(actual, expected));
        char *yaml = oa_document_render(doc, "yaml", &length, &error);
        REQUIRE(yaml);
        cyaml_doc_t *yaml_doc = cyaml_parse(yaml, length, NULL, NULL);
        REQUIRE(yaml_doc);
        json_value_t *yaml_root = json_value_from_cyaml(yaml_doc);
        REQUIRE(same_value(root, yaml_root));
        json_free(yaml_root); cyaml_free(yaml_doc); oa_text_free(yaml);
        json_free(expected); json_free(root); oa_text_free(text); oa_document_free(doc);
    }
    oa_document *doc = oa_document_create("Parameters", "1", &error);
    const char *parameters = API("@route GET /value/{n}\n@param n query int optional Query\n@param n path int required Path\n@param key header string optional Header\n@param key cookie string optional Cookie\n@minimum query.n 1\n@maximum path.n 100\n@minLength header.key 2\n@maxLength cookie.key 8\n@response 200 OK", "int value(void) { return 1; }");
    REQUIRE(doc && oa_document_add(doc, plugin, parameters, strlen(parameters), &error));
    size_t length;
    char *text = oa_document_render(doc, "json", &length, &error);
    REQUIRE(text);
    json_value_t *root = json_parse(text, length);
    json_value_t *params = json_object_get(json_object_get(json_object_get(json_object_get(root, "paths"), "/value/{n}"), "get"), "parameters");
    const char *expected_params = "[{\"name\":\"n\",\"in\":\"query\",\"required\":false,\"description\":\"Query\",\"schema\":{\"type\":\"integer\",\"minimum\":1}},{\"name\":\"n\",\"in\":\"path\",\"required\":true,\"description\":\"Path\",\"schema\":{\"type\":\"integer\",\"maximum\":100}},{\"name\":\"key\",\"in\":\"header\",\"required\":false,\"description\":\"Header\",\"schema\":{\"type\":\"string\",\"minLength\":2}},{\"name\":\"key\",\"in\":\"cookie\",\"required\":false,\"description\":\"Cookie\",\"schema\":{\"type\":\"string\",\"maxLength\":8}}]";
    json_value_t *expected = json_parse(expected_params, strlen(expected_params));
    REQUIRE(same_value(params, expected));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        int size = snprintf(source, sizeof(source), "/**\n@route POST /invalid\n@param n query int optional Query\n@param n header int optional Header\n@body application/json object required Body\n@field name string required Name\n%s\n@response 200 OK\n*/\nint invalid(void) { return 1; }", invalid[i]);
        REQUIRE(size > 0 && (size_t)size < sizeof(source));
        REQUIRE(rejected_unchanged(doc, plugin, source, text, &error));
    }
    json_free(expected); json_free(root); oa_text_free(text); oa_document_free(doc);
    doc = oa_document_create("Enum capacity", "1", &error);
    REQUIRE(doc);
    size_t used = (size_t)snprintf(source, sizeof(source), "/**\n@route POST /bounded\n@body application/json int required Value\n");
    for (size_t i = 0; i < ENUM_CAPACITY; ++i) {
        int size = snprintf(source + used, sizeof(source) - used, "@enum body %zu\n", i);
        REQUIRE(size > 0 && (size_t)size < sizeof(source) - used);
        used += (size_t)size;
    }
    const char *ending = "@response 200 OK\n*/\nint bounded(void) { return 1; }";
    REQUIRE(used + strlen(ending) < sizeof(source));
    strcpy(source + used, ending);
    REQUIRE(oa_document_add(doc, plugin, source, strlen(source), &error));
    text = oa_document_render(doc, "json", &length, &error);
    REQUIRE(text);
    /* A fresh document isolates the enum limit from duplicate route rejection. */
    oa_document *overflow = oa_document_create("Overflow", "1", &error);
    REQUIRE(overflow);
    int size = snprintf(source + used, sizeof(source) - used, "@enum body %u\n%s", ENUM_CAPACITY, ending);
    REQUIRE(size > 0 && (size_t)size < sizeof(source) - used);
    char *empty = oa_document_render(overflow, "json", &length, &error);
    REQUIRE(empty && rejected_unchanged(overflow, plugin, source, empty, &error));
    oa_text_free(empty); oa_document_free(overflow); oa_text_free(text); oa_document_free(doc);
    return 0;
}

int main(void) {
    oa_error error = {{0}};
    oa_plugin *plugin = oa_plugin_open(C_PLUGIN, &error);
    oa_document *doc;
    char *json, *yaml, *after, *oversized;
    size_t len = 0, yaml_len = 0, i;
    json_value_t *root, *yaml_root, *paths, *get, *post, *expected;
    cyaml_doc_t *yaml_doc;
    const char *first = ROUTE("/pets", "GET", "list_pets")
        "int ordinary(void) { return 0; }\n"
        API(" * @brief Ordinary documentation", "int documented_ordinary(void) { return 0; }")
        "/* @route broken */\nint unrelated;\nint no_documentation(void) { return 0; }\n"
        API(" * @route get /health\n * @response 200 OK", "const char *health(void) { return \"ok\"; }");
    const char *second = API(
        " * @route POST /pets\n"
        " * @operationId createPet\n"
        " * @brief Create a pet\n"
        " * @details Line one\n"
        " * Line two: # pet \"quoted\" \\u0000\n"
        " * @tag pets\n"
        " * @deprecated false\n"
        " * @param limit query int optional Maximum results\n"
        " * @minimum limit 1.5\n"
        " * @body application/json object required New pet\n"
        " * @field name char* required Pet name\n"
        " * @field active bool optional Active flag\n"
        " * @response 201 Created\n"
        " * @produces 201 application/json string[]",
        "int create_pet(void) { return 0; }");
    /* This is an independent output assertion, never annotation input. */
    const char *expected_operation =
        "{\"operationId\":\"createPet\",\"summary\":\"Create a pet\","
        "\"description\":\"Line one\\nLine two: # pet \\\"quoted\\\" \\\\u0000\","
        "\"tags\":[\"pets\"],\"deprecated\":false,"
        "\"parameters\":[{\"name\":\"limit\",\"in\":\"query\",\"required\":false,\"description\":\"Maximum results\",\"schema\":{\"type\":\"integer\",\"minimum\":1.5}}],"
        "\"requestBody\":{\"required\":true,\"description\":\"New pet\",\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Pet name\"},\"active\":{\"type\":\"boolean\",\"description\":\"Active flag\"}},\"required\":[\"name\"]}}}},"
        "\"responses\":{\"201\":{\"description\":\"Created\",\"content\":{\"application/json\":{\"schema\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}}}}";
    const char *bad_sources[] = {
        API("@route GET /body-target\n@param body query int optional Query\n@minimum body 1\n@response 200 OK", "int body_target_is_not_a_parameter(void) { return 1; }"),
        API("@route POST /numeric\n@body application/json number required Value\n@minimum body 0x10\n@response 200 OK", "int hex_is_not_json_number(void) { return 1; }"),
        API("@route POST /numeric\n@body application/json number required Value\n@maximum body +2\n@response 200 OK", "int plus_is_not_json_number(void) { return 1; }"),
        API("@route POST /numeric\n@body application/json number required Value\n@enum body 01\n@response 200 OK", "int leading_zero_is_not_json_number(void) { return 1; }"),
        ROUTE("/missing/{id}", "get", "missing_path_parameter"),
        API("@route GET /extra\n@param id path int required Id\n@response 200 OK", "int extra_path_parameter(void) { return 0; }"),
        ROUTE("/bad/{", "get", "unclosed_template"),
        ROUTE("/bad/}", "get", "unopened_template"),
        ROUTE("/bad/{}", "get", "empty_template"),
        API("@route GET /bad/{a{b}\n@param a{b path int required Id\n@response 200 OK", "int nested_template(void) { return 0; }"),
        API("@route GET /same/{id}\n@param id path int required Id\n@response 200 OK", "int first_template(void) { return 0; }")
        API("@route POST /same/{name}\n@param name path int required Name\n@response 200 OK", "int equivalent_template(void) { return 0; }"),
        API(" * @route GET /invalid\n * @response 200 OK\n * @response x-typo Mistake", "int bad_status(void) { return 0; }"),
        ROUTE("/pets", "get", "duplicate_route"),
        API(" * @route get /other\n * @operationId createPet\n * @response 200 OK", "int other(void) { return 0; }"),
        API(" * @route", "int malformed(void) { return 0; }"),
        ROUTE("/invalid", "fetch", "unsupported"),
        ROUTE("/invalid", "OA_GET", "enum_symbol_is_not_http_method"),
        API(" * @route get /invalid", "int missing_responses(void) { return 0; }"),
        API(" * @response 200 OK", "int missing_route(void) { return 0; }"),
        API(" * @route get /invalid\n * @response 200", "int missing_description(void) { return 0; }"),
        ROUTE("invalid", "get", "bad_path"),
        "int broken( {",
        ROUTE("/staged", "get", "staged") ROUTE("/pets", "get", "conflict"),
        ROUTE("/staged", "get", "staged") API(" * @route", "int broken(void) { return 0; }"),
        API(" * @route get /invalid\n * @response 200 OK\n * @unknown surprise", "int unknown_tag(void) { return 0; }"),
        API(" * @route get /invalid\n * @route post /invalid\n * @response 200 OK", "int repeated_route(void) { return 0; }"),
        API(" * @route get /invalid\n * @brief First\n * @summary Second\n * @response 200 OK", "int repeated_summary(void) { return 0; }"),
        API(" * @route get /invalid\n * @response 200 OK\n * @response 200 Again", "int repeated_response(void) { return 0; }"),
        API(" * @route get /invalid\n * @param n query int optional Number\n * @param n query int optional Number\n * @response 200 OK", "int repeated_parameter(void) { return 0; }"),
        API(" * @route get /invalid\n * @body application/json object required Body\n * @field n int required Number\n * @field n int optional Number\n * @response 200 OK", "int repeated_field(void) { return 0; }"),
        API(" * @route get /invalid\n * @param n query mystery optional Number\n * @response 200 OK", "int bad_type(void) { return 0; }"),
        API(" * @route get /invalid\n * @param n query int maybe Number\n * @response 200 OK", "int bad_required(void) { return 0; }"),
        API(" * @route get /invalid\n * @param n somewhere int optional Number\n * @response 200 OK", "int bad_location(void) { return 0; }"),
        API(" * @route get /invalid/{n}\n * @param n path int optional Number\n * @response 200 OK", "int optional_path(void) { return 0; }"),
        API(" * @route get /invalid\n * @produces 200 application/json string\n * @response 200 OK", "int response_order(void) { return 0; }"),
        API(" * @route get /invalid\n * @field name string required Name\n * @response 200 OK", "int field_without_body(void) { return 0; }"),
        API(" * @route get /invalid\n * @body application/json string required Body\n * @field name string required Name\n * @response 200 OK", "int nonobject_field(void) { return 0; }"),
        API(" * @route get /invalid\n * @body application/json object required Body\n * @body application/json object required Again\n * @response 200 OK", "int repeated_body(void) { return 0; }"),
        API(" * @route get /invalid\n * @deprecated maybe\n * @response 200 OK", "int bad_deprecated(void) { return 0; }"),
        API(" * @route get /invalid\n * @param n query int[][][][][][][][][] optional Number\n * @response 200 OK", "int excessive_arrays(void) { return 0; }"),
        /* Legacy JSON annotations must fail; JSON/YAML are output formats only. */
        API(" * @openapi {\"path\":\"/legacy\",\"method\":\"get\",\"operation\":{\"responses\":{\"200\":{\"description\":\"OK\"}}}}", "int legacy(void) { return 0; }")
    };
    oa_language_plugin invalid = {99, sizeof(invalid), "bad", tree_sitter_c, "(translation_unit) @function @name @doc"};
    REQUIRE(plugin);
    REQUIRE(numeric_text_preserved(plugin) == 0);
    REQUIRE(utf8_inputs(plugin) == 0);
    REQUIRE(media_types(plugin) == 0);
    REQUIRE(parameter_names(plugin) == 0);
    REQUIRE(standard_constraints(plugin) == 0);
    {
        oa_document *wrapped = oa_document_create("Wrapped declarators", "1.0", &error);
        const char *wrapped_source =
            API(" * @route GET /names\n * @response 200 OK", "char **list_names(void) { return 0; }")
            API(" * @route get /wrapped-health\n * @response 200 OK", "int (health)(void) { return 1; }")
            API(" * @route GET /factory\n * @response 200 OK", "int (*factory(void))(int) { return 0; }");
        REQUIRE(wrapped);
        REQUIRE(oa_document_add(wrapped, plugin, wrapped_source, strlen(wrapped_source), &error));
        json = oa_document_render(wrapped, "json", &len, &error);
        REQUIRE(json);
        root = json_parse(json, len);
        REQUIRE(root);
        paths = json_object_get(root, "paths");
        REQUIRE(json_object_size(paths) == 3);
        REQUIRE(string_is(json_object_get(json_object_get(paths, "/names"), "get"), "operationId", "list_names"));
        REQUIRE(string_is(json_object_get(json_object_get(paths, "/wrapped-health"), "get"), "operationId", "health"));
        REQUIRE(string_is(json_object_get(json_object_get(paths, "/factory"), "get"), "operationId", "factory"));
        json_free(root);
        oa_text_free(json);
        oa_document_free(wrapped);
    }
    doc = oa_document_create("Pets", "1.0", &error);
    REQUIRE(doc);
    REQUIRE(oa_document_add(doc, plugin, first, strlen(first), &error));
    REQUIRE(oa_document_add(doc, plugin, second, strlen(second), &error));
    json = oa_document_render(doc, "json", &len, &error);
    REQUIRE(json && len == strlen(json));
    root = json_parse(json, len);
    REQUIRE(root && string_is(root, "openapi", "3.1.0"));
    REQUIRE(string_is(json_object_get(root, "info"), "title", "Pets"));
    REQUIRE(string_is(json_object_get(root, "info"), "version", "1.0"));
    paths = json_object_get(root, "paths");
    REQUIRE(json_object_size(paths) == 2);
    get = json_object_get(json_object_get(paths, "/pets"), "get");
    post = json_object_get(json_object_get(paths, "/pets"), "post");
    REQUIRE(get && post && string_is(get, "operationId", "list_pets"));
    REQUIRE(string_is(json_object_get(json_object_get(paths, "/health"), "get"), "operationId", "health"));
    expected = json_parse(expected_operation, strlen(expected_operation));
    REQUIRE(expected);
    if (!same_value(post, expected)) {
        char *actual = json_serialize_pretty(post, &len);
        fprintf(stderr, "Actual operation:\n%s\nExpected:\n%s\n", actual ? actual : "(null)", expected_operation);
        json_serialize_free(actual);
        REQUIRE(0);
    }
    json_free(expected);
    for (i = 0; i < sizeof(bad_sources) / sizeof(bad_sources[0]); ++i) {
        if (!rejected_unchanged(doc, plugin, bad_sources[i], json, &error)) {
            fprintf(stderr, "transactional failure case %zu failed: %s\n", i, error.message);
            return 1;
        }
    }
    REQUIRE(oa_document_add(doc, plugin, "int ignored(void) { return 1; }", strlen("int ignored(void) { return 1; }"), &error));
    after = oa_document_render(doc, "json", &len, &error);
    REQUIRE(after && strcmp(json, after) == 0);
    oa_text_free(after);
    oversized = malloc(16u * 1024u * 1024u + 1u);
    REQUIRE(oversized);
    memset(oversized, ' ', 16u * 1024u * 1024u + 1u);
    REQUIRE(!oa_document_add(doc, plugin, oversized, 16u * 1024u * 1024u + 1u, &error));
    free(oversized);
    REQUIRE(!oa_document_render(doc, "xml", &len, &error));
    yaml = oa_document_render(doc, "yaml", &yaml_len, &error);
    REQUIRE(yaml && yaml_len == strlen(yaml));
    yaml_doc = cyaml_parse(yaml, yaml_len, NULL, NULL);
    REQUIRE(yaml_doc);
    yaml_root = json_value_from_cyaml(yaml_doc);
    REQUIRE(yaml_root && same_value(root, yaml_root));
    json_free(yaml_root);
    cyaml_free(yaml_doc);
    json_free(root);
    oa_text_free(yaml);
    oa_text_free(json);
    REQUIRE(!oa_plugin_open("openapi-deliberately-missing-plugin-7d61.dll", &error));
    REQUIRE(!oa_plugin_register(&invalid, &error));
    invalid.abi_version = OA_PLUGIN_ABI_V1;
    invalid.struct_size = 1;
    REQUIRE(!oa_plugin_register(&invalid, &error));
    invalid.struct_size = sizeof(invalid);
    invalid.query = "(this_node_does_not_exist) @function @name @doc";
    REQUIRE(!oa_plugin_register(&invalid, &error));
    invalid.query = "(translation_unit) @function";
    REQUIRE(!oa_plugin_register(&invalid, &error));
    invalid.language = NULL;
    REQUIRE(!oa_plugin_register(&invalid, &error));
    oa_document_free(doc);
    oa_plugin_close(plugin);
    oa_document_free(NULL);
    oa_plugin_close(NULL);
    oa_text_free(NULL);
    puts("generator integration passed");
    return 0;
}
