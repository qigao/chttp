#include "ui_model.h"

#include <json_parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "line %d: %s (%s)\n", __LINE__, #x, error.message); return 1; } } while (0)
#define API(tags, declaration) "/**\n" tags "\n */\n" declaration "\n"

static int view_is(vstr value, const char *text) {
    size_t length = strlen(text);
    return value.len == length && (length == 0u || (value.data && memcmp(value.data, text, length) == 0));
}

static const cmeta_data_field_desc *data_field(
    const cmeta_data_desc *data, const char *name) {
    if (!data || data->kind != CMETA_DATA_STRUCT || !data->shape) return NULL;
    const cmeta_data_struct_shape *shape = (const cmeta_data_struct_shape *)data->shape;
    for (size_t i = 0; i < shape->field_count; ++i)
        if (!strcmp(shape->fields[i].name, name)) return &shape->fields[i];
    return NULL;
}

int main(void) {
    oa_error error = {{0}};
    oa_plugin *plugin = oa_plugin_open(C_PLUGIN, &error);
    REQUIRE(plugin);

    oa_document *document = oa_document_create("Pets \xE4\xB8\xAD\xE6\x96\x87", "2.1", &error);
    REQUIRE(document);

    const char *source =
        API("@route POST /pets/{id}\n"
            "@summary Update pet\n"
            "@tag write\n"
            "@param id path int required Pet id\n"
            "@body application/json object required Update\n"
            "@field name string optional Name\n"
            "@response 200 Updated",
            "int update_pet(void) { return 0; }")
        API("@route GET /pets/{id}\n"
            "@operationId getPet\n"
            "@summary Read pet\n"
            "@details \xE8\xBF\x94\xE5\x9B\x9E\xE5\xAE\xA0\xE7\x89\xA9\n"
            "@tag read\n"
            "@tag pets\n"
            "@param id path int required Pet id\n"
            "@param verbose query bool optional Verbose\n"
            "@response 200 Found",
            "int get_pet(void) { return 0; }")
        API("@route GET /health\n"
            "@response 204 Healthy",
            "int health(void) { return 0; }");

    REQUIRE(oa_document_add(document, plugin, source, strlen(source), &error));

    oa_ui_model *model = oa_ui_model_create(document, &error);
    REQUIRE(model);
    const oa_ui_document *view = oa_ui_model_view(model);
    REQUIRE(view);
    REQUIRE(view_is(view->title, "Pets \xE4\xB8\xAD\xE6\x96\x87"));
    REQUIRE(view_is(view->version, "2.1"));
    REQUIRE(view_is(view->openapi_version, "3.1.0"));
    REQUIRE(view->operations.count == 3u);
    REQUIRE(view->operations.stride == sizeof(oa_ui_operation));
    REQUIRE(view->operations.element == oa_ui_operation_cmeta_data());

    const oa_ui_operation *operations = (const oa_ui_operation *)view->operations.data;
    /* Preserve path insertion order, but normalize methods to the existing UI method order. */
    REQUIRE(view_is(operations[0].method, "get"));
    REQUIRE(view_is(operations[0].path, "/pets/{id}"));
    REQUIRE(view_is(operations[0].operation_id, "getPet"));
    REQUIRE(view_is(operations[0].summary, "Read pet"));
    REQUIRE(view_is(operations[0].description, "\xE8\xBF\x94\xE5\x9B\x9E\xE5\xAE\xA0\xE7\x89\xA9"));
    REQUIRE(!operations[0].deprecated);
    REQUIRE(operations[0].tags.count == 2u);
    REQUIRE(operations[0].tags.stride == sizeof(vstr));
    const vstr *tags = (const vstr *)operations[0].tags.data;
    REQUIRE(view_is(tags[0], "read"));
    REQUIRE(view_is(tags[1], "pets"));

    REQUIRE(operations[0].parameters.count == 2u);
    REQUIRE(operations[0].parameters.stride == sizeof(oa_ui_parameter));
    REQUIRE(operations[0].parameters.element == oa_ui_parameter_cmeta_data());
    const oa_ui_parameter *parameters = (const oa_ui_parameter *)operations[0].parameters.data;
    REQUIRE(view_is(parameters[0].name, "id"));
    REQUIRE(view_is(parameters[0].location, "path"));
    REQUIRE(parameters[0].required);
    REQUIRE(view_is(parameters[1].name, "verbose"));
    REQUIRE(view_is(parameters[1].location, "query"));
    REQUIRE(!parameters[1].required);
    REQUIRE(parameters[1].schema_json.len != 0u);
    json_value_t *schema = json_parse(parameters[1].schema_json.data, parameters[1].schema_json.len);
    REQUIRE(schema && json_type(schema) == JSON_OBJECT);
    REQUIRE(!strcmp(json_string(json_object_get(schema, "type")), "boolean"));
    json_free(schema);

    /* GET must precede POST even though POST was declared first in the source. */
    REQUIRE(view_is(operations[1].method, "post"));
    REQUIRE(view_is(operations[1].path, "/pets/{id}"));
    REQUIRE(view_is(operations[1].operation_id, "update_pet"));
    REQUIRE(operations[1].request_body_json.len != 0u);
    REQUIRE(operations[1].responses_json.len != 0u);

    /* Missing optional text is represented as an empty borrowed view. */
    REQUIRE(view_is(operations[2].method, "get"));
    REQUIRE(view_is(operations[2].path, "/health"));
    REQUIRE(view_is(operations[2].summary, ""));
    REQUIRE(view_is(operations[2].description, ""));
    REQUIRE(operations[2].parameters.count == 0u);
    REQUIRE(operations[2].parameters.data == NULL);
    REQUIRE(operations[2].request_body_json.len == 0u);

    const cmeta_data_desc *parameter_data = oa_ui_parameter_cmeta_data();
    const cmeta_data_desc *operation_data = oa_ui_operation_cmeta_data();
    const cmeta_data_desc *document_data = oa_ui_document_cmeta_data();
    REQUIRE(cmeta_data_desc_valid(parameter_data));
    REQUIRE(cmeta_data_desc_valid(operation_data));
    REQUIRE(cmeta_data_desc_valid(document_data));
    REQUIRE(data_field(operation_data, "parameters"));
    REQUIRE(data_field(operation_data, "parameters")->value->kind == CMETA_DATA_SEQUENCE);
    REQUIRE(data_field(operation_data, "tags"));
    REQUIRE(data_field(operation_data, "tags")->value->kind == CMETA_DATA_SEQUENCE);
    REQUIRE(data_field(document_data, "operations"));
    REQUIRE(data_field(document_data, "operations")->value->kind == CMETA_DATA_SEQUENCE);

    /* A successful model is a snapshot and no longer borrows the source document. */
    oa_document_free(document);
    document = NULL;
    REQUIRE(view_is(view->title, "Pets \xE4\xB8\xAD\xE6\x96\x87"));
    REQUIRE(view_is(operations[0].description, "\xE8\xBF\x94\xE5\x9B\x9E\xE5\xAE\xA0\xE7\x89\xA9"));
    REQUIRE(view_is(parameters[0].name, "id"));

    REQUIRE(!oa_ui_model_create(NULL, &error));
    REQUIRE(!oa_ui_model_view(NULL));
    oa_ui_model_free(NULL);
    oa_ui_model_free(model);
    oa_plugin_close(plugin);
    puts("openapi ui model passed");
    return 0;
}
