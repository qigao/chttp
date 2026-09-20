#include <openapi/ui_model.h>
#include "ui_model_internal.h"

#include <json_parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "line %d: %s (%s)\n", __LINE__, #x, error.message); return 1; } } while (0)
#define API(tags, declaration) "/**\n" tags "\n */\n" declaration "\n"

static const char DIRECT_JSON[] =
    "{\"openapi\":\"3.1.0\","
    "\"info\":{\"title\":\"Direct\",\"version\":\"1\"},"
    "\"servers\":[{\"url\":\"https://api.example.test/v1\"}],"
    "\"paths\":{\"/ping\":{"
      "\"parameters\":["
        "{\"name\":\"tenant\",\"in\":\"header\",\"required\":true,"
          "\"schema\":{\"type\":\"string\"},\"description\":\"Inherited\"},"
        "{\"name\":\"q\",\"in\":\"query\",\"required\":false,"
          "\"schema\":{\"type\":\"string\"},\"description\":\"Path default\"}"
      "],"
      "\"get\":{"
        "\"operationId\":\"ping\","
        "\"tags\":[\"health\"],"
        "\"parameters\":["
          "{\"name\":\"q\",\"in\":\"query\",\"required\":true,"
            "\"schema\":{\"type\":\"string\"},\"description\":\"Operation override\"},"
          "{\"name\":\"limit\",\"in\":\"query\",\"required\":false,"
            "\"schema\":{\"type\":\"integer\"}}"
        "],"
        "\"responses\":{\"204\":{\"description\":\"OK\"}}"
      "}"
    "}}}";

typedef struct failing_allocator_state {
    size_t calls;
    size_t fail_at;
    size_t live;
} failing_allocator_state;

static void *failing_calloc(void *userdata, size_t count, size_t size) {
    failing_allocator_state *state = (failing_allocator_state *)userdata;
    ++state->calls;
    if (state->calls == state->fail_at) return NULL;
    void *memory = calloc(count, size);
    if (memory) ++state->live;
    return memory;
}

static void failing_free(void *userdata, void *memory) {
    failing_allocator_state *state = (failing_allocator_state *)userdata;
    if (!memory) return;
    if (state->live != 0u) --state->live;
    free(memory);
}

static int view_is(vstr value, const char *text) {
    size_t length = strlen(text);
    return value.len == length &&
           (length == 0u || (value.data && memcmp(value.data, text, length) == 0));
}

static const cmeta_data_field_desc *data_field(
    const cmeta_data_desc *data, const char *name) {
    if (!data || data->kind != CMETA_DATA_STRUCT || !data->shape) return NULL;
    const cmeta_data_struct_shape *shape = (const cmeta_data_struct_shape *)data->shape;
    for (size_t i = 0; i < shape->field_count; ++i)
        if (!strcmp(shape->fields[i].name, name)) return &shape->fields[i];
    return NULL;
}

static int check_parsed_json_entry(oa_error *error) {
    json_value_t *root = json_parse(DIRECT_JSON, strlen(DIRECT_JSON));
    if (!root) {
        snprintf(error->message, sizeof(error->message), "direct JSON parse failed: %s",
                 json_get_error() ? json_get_error() : "unknown");
        return 0;
    }
    oa_ui_model *model = oa_ui_model_create_json(root, error);
    json_free(root);
    if (!model) return 0;

    const oa_ui_document *view = oa_ui_model_view(model);
    if (!view || !view_is(view->title, "Direct") ||
        !view_is(view->server_url, "https://api.example.test/v1") ||
        view->operations.count != 1u) {
        snprintf(error->message, sizeof(error->message), "direct JSON root projection mismatch");
        oa_ui_model_free(model);
        return 0;
    }

    const oa_ui_operation *op = (const oa_ui_operation *)view->operations.data;
    if (!op || !view_is(op[0].path, "/ping") || !view_is(op[0].method, "get")) {
        snprintf(error->message, sizeof(error->message), "direct JSON operation mismatch");
        oa_ui_model_free(model);
        return 0;
    }
    if (op[0].tags.count != 1u ||
        !view_is(((const vstr *)op[0].tags.data)[0], "health")) {
        snprintf(error->message, sizeof(error->message), "direct JSON tag mismatch");
        oa_ui_model_free(model);
        return 0;
    }

    /* Match the current browser behavior: path parameters are inherited,
     * operation parameters replace the same in:name key without changing its
     * insertion slot, and new operation parameters append. */
    if (op[0].parameters.count != 3u) {
        snprintf(error->message, sizeof(error->message),
                 "direct JSON effective parameter count=%zu", op[0].parameters.count);
        oa_ui_model_free(model);
        return 0;
    }
    const oa_ui_parameter *parameters =
        (const oa_ui_parameter *)op[0].parameters.data;
    if (!view_is(parameters[0].name, "tenant") ||
        !view_is(parameters[0].location, "header") ||
        !parameters[0].required ||
        !view_is(parameters[1].name, "q") ||
        !view_is(parameters[1].description, "Operation override") ||
        !parameters[1].required ||
        !view_is(parameters[2].name, "limit")) {
        snprintf(error->message, sizeof(error->message),
                 "direct JSON effective parameter merge mismatch");
        oa_ui_model_free(model);
        return 0;
    }

    oa_ui_model_free(model);
    return 1;
}

static int check_allocation_failures(oa_error *error) {
    json_value_t *root = json_parse(DIRECT_JSON, strlen(DIRECT_JSON));
    if (!root) {
        snprintf(error->message, sizeof(error->message), "OOM fixture parse failed");
        return 0;
    }

    size_t failures = 0u;
    int reached_success = 0;
    for (size_t fail_at = 1u; fail_at <= 16u; ++fail_at) {
        failing_allocator_state state = {0u, fail_at, 0u};
        oa_ui_allocator allocator = {failing_calloc, failing_free, &state};
        error->message[0] = '\0';

        oa_ui_model *model =
            oa_ui_model_create_json_with_allocator(root, &allocator, error);
        if (!model) {
            ++failures;
            if (!strstr(error->message, "out of memory") || state.live != 0u) {
                json_free(root);
                return 0;
            }
            continue;
        }

        oa_ui_model_free(model);
        if (state.live != 0u) {
            snprintf(error->message, sizeof(error->message),
                     "allocator live blocks after success=%zu", state.live);
            json_free(root);
            return 0;
        }
        reached_success = 1;
        break;
    }
    json_free(root);

    if (!reached_success || failures < 4u) {
        snprintf(error->message, sizeof(error->message),
                 "allocator failure coverage too small: failures=%zu", failures);
        return 0;
    }
    return 1;
}

int main(void) {
    oa_error error = {{0}};
    oa_plugin *plugin = oa_plugin_open(C_PLUGIN, &error);
    REQUIRE(plugin);

    oa_document *document =
        oa_document_create("Pets \xE4\xB8\xAD\xE6\x96\x87", "2.1", &error);
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
    REQUIRE(view->selected_operations.count == 0u);
    REQUIRE(view->selected_operations.stride == sizeof(oa_ui_operation));
    REQUIRE(view->selected_operations.element == oa_ui_operation_cmeta_data());
    REQUIRE(view->operations.element == oa_ui_operation_cmeta_data());

    const oa_ui_operation *operations =
        (const oa_ui_operation *)view->operations.data;
    REQUIRE(view_is(operations[0].method, "get"));
    REQUIRE(view_is(operations[0].path, "/pets/{id}"));
    REQUIRE(view_is(operations[0].operation_id, "getPet"));
    REQUIRE(view_is(operations[0].route_key, "getPet"));
    REQUIRE(view_is(operations[0].summary, "Read pet"));
    REQUIRE(view_is(operations[0].description,
                    "\xE8\xBF\x94\xE5\x9B\x9E\xE5\xAE\xA0\xE7\x89\xA9"));
    REQUIRE(!operations[0].deprecated);
    REQUIRE(operations[0].tags.count == 2u);
    REQUIRE(operations[0].tags.stride == sizeof(vstr));
    const vstr *tags = (const vstr *)operations[0].tags.data;
    REQUIRE(view_is(tags[0], "read"));
    REQUIRE(view_is(tags[1], "pets"));

    REQUIRE(operations[0].parameters.count == 2u);
    REQUIRE(operations[0].parameters.stride == sizeof(oa_ui_parameter));
    REQUIRE(operations[0].parameters.element == oa_ui_parameter_cmeta_data());
    const oa_ui_parameter *parameters =
        (const oa_ui_parameter *)operations[0].parameters.data;
    REQUIRE(view_is(parameters[0].name, "id"));
    REQUIRE(view_is(parameters[0].location, "path"));
    REQUIRE(parameters[0].required);
    REQUIRE(view_is(parameters[1].name, "verbose"));
    REQUIRE(view_is(parameters[1].location, "query"));
    REQUIRE(!parameters[1].required);
    REQUIRE(parameters[1].schema_json.len != 0u);
    json_value_t *schema =
        json_parse(parameters[1].schema_json.data, parameters[1].schema_json.len);
    REQUIRE(schema && json_type(schema) == JSON_OBJECT);
    REQUIRE(!strcmp(json_string(json_object_get(schema, "type")), "boolean"));
    json_free(schema);

    REQUIRE(view_is(operations[1].method, "post"));
    REQUIRE(view_is(operations[1].path, "/pets/{id}"));
    REQUIRE(view_is(operations[1].operation_id, "update_pet"));
    REQUIRE(view_is(operations[1].route_key, "update_pet"));
    REQUIRE(operations[1].request_body_json.len != 0u);
    REQUIRE(operations[1].responses_json.len != 0u);

    REQUIRE(view_is(operations[2].method, "get"));
    REQUIRE(view_is(operations[2].path, "/health"));
    REQUIRE(view_is(operations[2].operation_id, "health"));
    REQUIRE(view_is(operations[2].route_key, "health"));
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
    REQUIRE(data_field(operation_data, "route_key"));
    REQUIRE(data_field(document_data, "operations"));
    REQUIRE(data_field(document_data, "operations")->value->kind == CMETA_DATA_SEQUENCE);
    REQUIRE(data_field(document_data, "selected_operations"));
    REQUIRE(data_field(document_data, "selected_operations")->value->kind == CMETA_DATA_SEQUENCE);

    oa_document_free(document);
    document = NULL;
    REQUIRE(view_is(view->title, "Pets \xE4\xB8\xAD\xE6\x96\x87"));
    REQUIRE(view_is(operations[0].description,
                    "\xE8\xBF\x94\xE5\x9B\x9E\xE5\xAE\xA0\xE7\x89\xA9"));
    REQUIRE(view_is(parameters[0].name, "id"));

    REQUIRE(check_parsed_json_entry(&error));
    REQUIRE(check_allocation_failures(&error));

    error.message[0] = '\0';
    REQUIRE(!oa_ui_model_create(NULL, &error));
    REQUIRE(strstr(error.message, "document"));
    REQUIRE(!oa_ui_model_create_json(NULL, &error));
    REQUIRE(!oa_ui_model_view(NULL));
    oa_ui_model_free(NULL);
    oa_ui_model_free(model);
    oa_plugin_close(plugin);
    puts("openapi ui model passed");
    return 0;
}
