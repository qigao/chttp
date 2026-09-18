#include <openapi/ui_model.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "line %d: %s (%s)\n", __LINE__, #x, error.message); return 1; } } while (0)

static int text_is(vstr value, const char *expected) {
    const size_t length = strlen(expected);
    return value.len == length && (length == 0u || (value.data && memcmp(value.data, expected, length) == 0));
}

int main(void) {
    oa_error error = {{0}};
    oa_plugin *plugin = oa_plugin_open(C_PLUGIN, &error);
    REQUIRE(plugin);

    oa_document *document = oa_document_create("Pets \xe4\xb8\xad\xe6\x96\x87", "1.2", &error);
    REQUIRE(document);

    const char *source =
        "/**\n"
        " * @route POST /pets\n"
        " * @body application/json object required Pet to create\n"
        " * @field name string required Pet name\n"
        " * @response 201 Created\n"
        " */\n"
        "int create_pet(void) { return 0; }\n"
        "/**\n"
        " * @route GET /pets\n"
        " * @summary List \xe5\xae\xa0\xe7\x89\xa9\n"
        " * @description Stable description\n"
        " * @deprecated\n"
        " * @param q query string optional Search term\n"
        " * @minLength query.q 1\n"
        " * @response 200 OK\n"
        " */\n"
        "int list_pets(void) { return 0; }\n";
    REQUIRE(oa_document_add(document, plugin, source, strlen(source), &error));

    oa_ui_model *model = oa_ui_model_create(document, &error);
    REQUIRE(model);
    const oa_ui_document *ui = oa_ui_model_document(model);
    REQUIRE(ui);

    /* The model owns its projected bytes and must survive generator teardown. */
    oa_document_free(document);
    document = NULL;

    REQUIRE(text_is(ui->title, "Pets \xe4\xb8\xad\xe6\x96\x87"));
    REQUIRE(text_is(ui->version, "1.2"));
    REQUIRE(text_is(ui->openapi_version, "3.1.0"));
    REQUIRE(ui->operation_count == 2u);

    /* Preserve the existing UI's fixed method ordering, not JSON insertion order. */
    const oa_ui_operation *get = &ui->operations[0];
    const oa_ui_operation *post = &ui->operations[1];
    REQUIRE(text_is(get->method, "get"));
    REQUIRE(text_is(post->method, "post"));
    REQUIRE(text_is(get->path, "/pets"));
    REQUIRE(text_is(get->operation_id, "list_pets"));
    REQUIRE(text_is(get->summary, "List \xe5\xae\xa0\xe7\x89\xa9"));
    REQUIRE(text_is(get->description, "Stable description"));
    REQUIRE(get->deprecated);
    REQUIRE(get->parameter_count == 1u);
    REQUIRE(text_is(get->parameters[0].name, "q"));
    REQUIRE(text_is(get->parameters[0].location, "query"));
    REQUIRE(text_is(get->parameters[0].description, "Search term"));
    REQUIRE(!get->parameters[0].required);
    REQUIRE(strstr(get->parameters[0].schema_json.data, "\"minLength\": 1") != NULL);

    REQUIRE(text_is(post->operation_id, "create_pet"));
    REQUIRE(post->summary.len == 0u);
    REQUIRE(post->parameter_count == 0u);
    REQUIRE(post->request_body_json.len != 0u);
    REQUIRE(strstr(post->request_body_json.data, "application/json") != NULL);
    REQUIRE(post->responses_json.len != 0u);
    REQUIRE(strstr(post->responses_json.data, "\"201\"") != NULL);

    const cmeta_data_desc *parameter_data = oa_ui_parameter_cmeta_data();
    const cmeta_data_desc *operation_data = oa_ui_operation_cmeta_data();
    const cmeta_data_desc *document_data = oa_ui_document_cmeta_data();
    REQUIRE(parameter_data && parameter_data->kind == CMETA_DATA_STRUCT);
    REQUIRE(operation_data && operation_data->kind == CMETA_DATA_STRUCT);
    REQUIRE(document_data && document_data->kind == CMETA_DATA_STRUCT);
    REQUIRE(parameter_data->stable_id && strcmp(parameter_data->stable_id, "openapi.ui.parameter.data") == 0);
    REQUIRE(operation_data->stable_id && strcmp(operation_data->stable_id, "openapi.ui.operation.data") == 0);
    REQUIRE(document_data->stable_id && strcmp(document_data->stable_id, "openapi.ui.document.data") == 0);

    oa_ui_model_free(model);
    oa_plugin_close(plugin);

    error.message[0] = '\0';
    REQUIRE(!oa_ui_model_create(NULL, &error));
    REQUIRE(strstr(error.message, "document"));
    oa_ui_model_free(NULL);
    return 0;
}
