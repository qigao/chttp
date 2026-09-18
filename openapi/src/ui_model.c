#include <openapi/ui_model.h>
#include "internal.h"

#include <cmeta/struct.h>
#include <salts_cmeta_data.h>

#include <stddef.h>

#define OA_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

struct oa_ui_model {
    oa_ui_document document;
    oa_ui_operation *operations;
};

static const char *const OA_UI_METHODS[] = {
    "get", "post", "put", "patch", "delete", "head", "options", "trace"
};

static const cmeta_data_buffer_shape OA_UI_VSTR_SHAPE = {
    .ownership = CMETA_DATA_BUFFER_BORROWED
};

static const cmeta_data_desc OA_UI_VSTR_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "openapi.ui.vstr.data",
    .display_name = "OpenAPI UI text",
    .kind = CMETA_DATA_STRING,
    .storage_type = &salts_vstr_cmeta_type,
    .shape = &OA_UI_VSTR_SHAPE,
    .buffer_ops = &salts_vstr_cmeta_buffer_ops
};

static const cmeta_field_desc OA_UI_PARAMETER_LAYOUT_FIELDS[] = {
    {"name", "vstr", offsetof(oa_ui_parameter, name), sizeof(((oa_ui_parameter *)0)->name),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"location", "vstr", offsetof(oa_ui_parameter, location), sizeof(((oa_ui_parameter *)0)->location),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"description", "vstr", offsetof(oa_ui_parameter, description), sizeof(((oa_ui_parameter *)0)->description),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"schema_json", "vstr", offsetof(oa_ui_parameter, schema_json), sizeof(((oa_ui_parameter *)0)->schema_json),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"required", "bool", offsetof(oa_ui_parameter, required), sizeof(((oa_ui_parameter *)0)->required),
     _Alignof(bool), NULL, NULL}
};
static const cmeta_struct_desc OA_UI_PARAMETER_LAYOUT = {
    "oa_ui_parameter", sizeof(oa_ui_parameter), _Alignof(oa_ui_parameter),
    OA_UI_PARAMETER_LAYOUT_FIELDS, OA_ARRAY_COUNT(OA_UI_PARAMETER_LAYOUT_FIELDS)
};
static const cmeta_data_field_desc OA_UI_PARAMETER_FIELDS[] = {
    {"openapi.ui.parameter.name", "name", offsetof(oa_ui_parameter, name), &OA_UI_VSTR_DATA},
    {"openapi.ui.parameter.location", "location", offsetof(oa_ui_parameter, location), &OA_UI_VSTR_DATA},
    {"openapi.ui.parameter.description", "description", offsetof(oa_ui_parameter, description), &OA_UI_VSTR_DATA},
    {"openapi.ui.parameter.schema_json", "schema_json", offsetof(oa_ui_parameter, schema_json), &OA_UI_VSTR_DATA},
    {"openapi.ui.parameter.required", "required", offsetof(oa_ui_parameter, required), &cmeta_data_bool}
};
static const cmeta_data_struct_shape OA_UI_PARAMETER_SHAPE = {
    &OA_UI_PARAMETER_LAYOUT, OA_UI_PARAMETER_FIELDS, OA_ARRAY_COUNT(OA_UI_PARAMETER_FIELDS)
};
static const cmeta_type_identity OA_UI_PARAMETER_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.parameter");
static const cmeta_type_desc OA_UI_PARAMETER_TYPE = {
    "oa_ui_parameter", sizeof(oa_ui_parameter), _Alignof(oa_ui_parameter), CMETA_T_OBJECT,
    NULL, NULL, &OA_UI_PARAMETER_IDENTITY
};
static const cmeta_data_desc OA_UI_PARAMETER_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "openapi.ui.parameter.data",
    .display_name = "OpenAPI UI parameter",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &OA_UI_PARAMETER_TYPE,
    .shape = &OA_UI_PARAMETER_SHAPE
};

static const cmeta_field_desc OA_UI_OPERATION_LAYOUT_FIELDS[] = {
    {"method", "vstr", offsetof(oa_ui_operation, method), sizeof(((oa_ui_operation *)0)->method),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"path", "vstr", offsetof(oa_ui_operation, path), sizeof(((oa_ui_operation *)0)->path),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"operation_id", "vstr", offsetof(oa_ui_operation, operation_id), sizeof(((oa_ui_operation *)0)->operation_id),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"summary", "vstr", offsetof(oa_ui_operation, summary), sizeof(((oa_ui_operation *)0)->summary),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"description", "vstr", offsetof(oa_ui_operation, description), sizeof(((oa_ui_operation *)0)->description),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"deprecated", "bool", offsetof(oa_ui_operation, deprecated), sizeof(((oa_ui_operation *)0)->deprecated),
     _Alignof(bool), NULL, NULL},
    {"parameters", "const oa_ui_parameter *", offsetof(oa_ui_operation, parameters), sizeof(((oa_ui_operation *)0)->parameters),
     _Alignof(const oa_ui_parameter *), NULL, NULL},
    {"parameter_count", "size_t", offsetof(oa_ui_operation, parameter_count), sizeof(((oa_ui_operation *)0)->parameter_count),
     _Alignof(size_t), NULL, NULL},
    {"request_body_json", "vstr", offsetof(oa_ui_operation, request_body_json), sizeof(((oa_ui_operation *)0)->request_body_json),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"responses_json", "vstr", offsetof(oa_ui_operation, responses_json), sizeof(((oa_ui_operation *)0)->responses_json),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL}
};
static const cmeta_struct_desc OA_UI_OPERATION_LAYOUT = {
    "oa_ui_operation", sizeof(oa_ui_operation), _Alignof(oa_ui_operation),
    OA_UI_OPERATION_LAYOUT_FIELDS, OA_ARRAY_COUNT(OA_UI_OPERATION_LAYOUT_FIELDS)
};
static const cmeta_data_field_desc OA_UI_OPERATION_FIELDS[] = {
    {"openapi.ui.operation.method", "method", offsetof(oa_ui_operation, method), &OA_UI_VSTR_DATA},
    {"openapi.ui.operation.path", "path", offsetof(oa_ui_operation, path), &OA_UI_VSTR_DATA},
    {"openapi.ui.operation.operation_id", "operation_id", offsetof(oa_ui_operation, operation_id), &OA_UI_VSTR_DATA},
    {"openapi.ui.operation.summary", "summary", offsetof(oa_ui_operation, summary), &OA_UI_VSTR_DATA},
    {"openapi.ui.operation.description", "description", offsetof(oa_ui_operation, description), &OA_UI_VSTR_DATA},
    {"openapi.ui.operation.deprecated", "deprecated", offsetof(oa_ui_operation, deprecated), &cmeta_data_bool},
    {"openapi.ui.operation.parameter_count", "parameter_count", offsetof(oa_ui_operation, parameter_count), &cmeta_data_size},
    {"openapi.ui.operation.request_body_json", "request_body_json", offsetof(oa_ui_operation, request_body_json), &OA_UI_VSTR_DATA},
    {"openapi.ui.operation.responses_json", "responses_json", offsetof(oa_ui_operation, responses_json), &OA_UI_VSTR_DATA}
};
static const cmeta_data_struct_shape OA_UI_OPERATION_SHAPE = {
    &OA_UI_OPERATION_LAYOUT, OA_UI_OPERATION_FIELDS, OA_ARRAY_COUNT(OA_UI_OPERATION_FIELDS)
};
static const cmeta_type_identity OA_UI_OPERATION_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.operation");
static const cmeta_type_desc OA_UI_OPERATION_TYPE = {
    "oa_ui_operation", sizeof(oa_ui_operation), _Alignof(oa_ui_operation), CMETA_T_OBJECT,
    NULL, NULL, &OA_UI_OPERATION_IDENTITY
};
static const cmeta_data_desc OA_UI_OPERATION_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "openapi.ui.operation.data",
    .display_name = "OpenAPI UI operation",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &OA_UI_OPERATION_TYPE,
    .shape = &OA_UI_OPERATION_SHAPE
};

static const cmeta_field_desc OA_UI_DOCUMENT_LAYOUT_FIELDS[] = {
    {"title", "vstr", offsetof(oa_ui_document, title), sizeof(((oa_ui_document *)0)->title),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"version", "vstr", offsetof(oa_ui_document, version), sizeof(((oa_ui_document *)0)->version),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"openapi_version", "vstr", offsetof(oa_ui_document, openapi_version), sizeof(((oa_ui_document *)0)->openapi_version),
     _Alignof(vstr), &salts_vstr_cmeta_type, NULL},
    {"operations", "const oa_ui_operation *", offsetof(oa_ui_document, operations), sizeof(((oa_ui_document *)0)->operations),
     _Alignof(const oa_ui_operation *), NULL, NULL},
    {"operation_count", "size_t", offsetof(oa_ui_document, operation_count), sizeof(((oa_ui_document *)0)->operation_count),
     _Alignof(size_t), NULL, NULL}
};
static const cmeta_struct_desc OA_UI_DOCUMENT_LAYOUT = {
    "oa_ui_document", sizeof(oa_ui_document), _Alignof(oa_ui_document),
    OA_UI_DOCUMENT_LAYOUT_FIELDS, OA_ARRAY_COUNT(OA_UI_DOCUMENT_LAYOUT_FIELDS)
};
static const cmeta_data_field_desc OA_UI_DOCUMENT_FIELDS[] = {
    {"openapi.ui.document.title", "title", offsetof(oa_ui_document, title), &OA_UI_VSTR_DATA},
    {"openapi.ui.document.version", "version", offsetof(oa_ui_document, version), &OA_UI_VSTR_DATA},
    {"openapi.ui.document.openapi_version", "openapi_version", offsetof(oa_ui_document, openapi_version), &OA_UI_VSTR_DATA},
    {"openapi.ui.document.operation_count", "operation_count", offsetof(oa_ui_document, operation_count), &cmeta_data_size}
};
static const cmeta_data_struct_shape OA_UI_DOCUMENT_SHAPE = {
    &OA_UI_DOCUMENT_LAYOUT, OA_UI_DOCUMENT_FIELDS, OA_ARRAY_COUNT(OA_UI_DOCUMENT_FIELDS)
};
static const cmeta_type_identity OA_UI_DOCUMENT_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.document");
static const cmeta_type_desc OA_UI_DOCUMENT_TYPE = {
    "oa_ui_document", sizeof(oa_ui_document), _Alignof(oa_ui_document), CMETA_T_OBJECT,
    NULL, NULL, &OA_UI_DOCUMENT_IDENTITY
};
static const cmeta_data_desc OA_UI_DOCUMENT_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "openapi.ui.document.data",
    .display_name = "OpenAPI UI document",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &OA_UI_DOCUMENT_TYPE,
    .shape = &OA_UI_DOCUMENT_SHAPE
};

static void oa_ui_text_free(vstr *value) {
    if (!value) return;
    free((void *)value->data);
    value->data = NULL;
    value->len = 0u;
}

static int oa_ui_copy_bytes(vstr *out, const char *data, size_t length, oa_error *error) {
    char *copy;
    if (!out || (length != 0u && !data))
        return oa_fail(error, "invalid OpenAPI UI text");
    copy = oa_copy(data ? data : "", length);
    if (!copy)
        return oa_fail(error, "out of memory constructing OpenAPI UI model");
    out->data = copy;
    out->len = length;
    return 1;
}

static int oa_ui_copy_json_string(vstr *out, const json_value_t *value,
                                  const char *field, oa_error *error) {
    if (!value) {
        *out = (vstr){0};
        return 1;
    }
    if (json_type(value) != JSON_STRING)
        return oa_fail(error, "OpenAPI UI %s must be a string", field);
    return oa_ui_copy_bytes(out, json_string(value), json_string_len(value), error);
}

static int oa_ui_copy_json(vstr *out, const json_value_t *value,
                           const char *field, oa_error *error) {
    size_t length = 0u;
    char *serialized;
    int ok;
    if (!value) {
        *out = (vstr){0};
        return 1;
    }
    serialized = json_serialize_pretty(value, &length);
    if (!serialized)
        return oa_fail(error, "failed to serialize OpenAPI UI %s", field);
    ok = oa_ui_copy_bytes(out, serialized, length, error);
    json_serialize_free(serialized);
    return ok;
}

static void oa_ui_parameter_free(oa_ui_parameter *parameter) {
    if (!parameter) return;
    oa_ui_text_free(&parameter->name);
    oa_ui_text_free(&parameter->location);
    oa_ui_text_free(&parameter->description);
    oa_ui_text_free(&parameter->schema_json);
}

static void oa_ui_operation_free(oa_ui_operation *operation) {
    if (!operation) return;
    oa_ui_text_free(&operation->method);
    oa_ui_text_free(&operation->path);
    oa_ui_text_free(&operation->operation_id);
    oa_ui_text_free(&operation->summary);
    oa_ui_text_free(&operation->description);
    oa_ui_text_free(&operation->request_body_json);
    oa_ui_text_free(&operation->responses_json);
    if (operation->parameters) {
        oa_ui_parameter *parameters = (oa_ui_parameter *)operation->parameters;
        for (size_t i = 0u; i < operation->parameter_count; ++i)
            oa_ui_parameter_free(&parameters[i]);
        free(parameters);
    }
    operation->parameters = NULL;
    operation->parameter_count = 0u;
}

void oa_ui_model_free(oa_ui_model *model) {
    if (!model) return;
    oa_ui_text_free(&model->document.title);
    oa_ui_text_free(&model->document.version);
    oa_ui_text_free(&model->document.openapi_version);
    if (model->operations) {
        for (size_t i = 0u; i < model->document.operation_count; ++i)
            oa_ui_operation_free(&model->operations[i]);
        free(model->operations);
    }
    free(model);
}

static size_t oa_ui_operation_count(const json_value_t *paths) {
    size_t count = 0u;
    if (!paths || json_type(paths) != JSON_OBJECT) return 0u;
    for (size_t i = 0u; i < json_object_size(paths); ++i) {
        const json_value_t *path_item = json_object_value(paths, i);
        if (!path_item || json_type(path_item) != JSON_OBJECT) continue;
        for (size_t m = 0u; m < OA_ARRAY_COUNT(OA_UI_METHODS); ++m)
            if (json_object_get(path_item, OA_UI_METHODS[m]))
                ++count;
    }
    return count;
}

static int oa_ui_project_parameter(oa_ui_parameter *out, const json_value_t *parameter,
                                   oa_error *error) {
    const json_value_t *required;
    if (!out || !parameter || json_type(parameter) != JSON_OBJECT)
        return oa_fail(error, "OpenAPI UI parameter must be an object");
    if (!oa_ui_copy_json_string(&out->name, json_object_get(parameter, "name"), "parameter name", error) ||
        !oa_ui_copy_json_string(&out->location, json_object_get(parameter, "in"), "parameter location", error) ||
        !oa_ui_copy_json_string(&out->description, json_object_get(parameter, "description"), "parameter description", error) ||
        !oa_ui_copy_json(&out->schema_json, json_object_get(parameter, "schema"), "parameter schema", error))
        return 0;
    required = json_object_get(parameter, "required");
    if (required && json_type(required) != JSON_BOOL)
        return oa_fail(error, "OpenAPI UI parameter required must be boolean");
    out->required = required ? json_bool(required) : false;
    return 1;
}

static int oa_ui_project_parameters(oa_ui_operation *out, const json_value_t *operation,
                                    oa_error *error) {
    const json_value_t *parameters = json_object_get(operation, "parameters");
    oa_ui_parameter *items;
    size_t count;
    if (!parameters) return 1;
    if (json_type(parameters) != JSON_ARRAY)
        return oa_fail(error, "OpenAPI UI parameters must be an array");
    count = json_array_size(parameters);
    if (!count) return 1;
    items = calloc(count, sizeof(*items));
    if (!items)
        return oa_fail(error, "out of memory constructing OpenAPI UI parameters");
    out->parameters = items;
    out->parameter_count = count;
    for (size_t i = 0u; i < count; ++i)
        if (!oa_ui_project_parameter(&items[i], json_array_get(parameters, i), error))
            return 0;
    return 1;
}

static int oa_ui_project_operation(oa_ui_operation *out, const char *path, size_t path_length,
                                   const char *method, const json_value_t *operation,
                                   oa_error *error) {
    const json_value_t *deprecated;
    if (!out || !path || !method || !operation || json_type(operation) != JSON_OBJECT)
        return oa_fail(error, "invalid OpenAPI UI operation");
    if (!oa_ui_copy_bytes(&out->method, method, strlen(method), error) ||
        !oa_ui_copy_bytes(&out->path, path, path_length, error) ||
        !oa_ui_copy_json_string(&out->operation_id, json_object_get(operation, "operationId"), "operationId", error) ||
        !oa_ui_copy_json_string(&out->summary, json_object_get(operation, "summary"), "summary", error) ||
        !oa_ui_copy_json_string(&out->description, json_object_get(operation, "description"), "description", error) ||
        !oa_ui_project_parameters(out, operation, error) ||
        !oa_ui_copy_json(&out->request_body_json, json_object_get(operation, "requestBody"), "request body", error) ||
        !oa_ui_copy_json(&out->responses_json, json_object_get(operation, "responses"), "responses", error))
        return 0;
    deprecated = json_object_get(operation, "deprecated");
    if (deprecated && json_type(deprecated) != JSON_BOOL)
        return oa_fail(error, "OpenAPI UI deprecated must be boolean");
    out->deprecated = deprecated ? json_bool(deprecated) : false;
    return 1;
}

oa_ui_model *oa_ui_model_create(const oa_document *document, oa_error *error) {
    const json_value_t *root;
    const json_value_t *info;
    const json_value_t *paths;
    oa_ui_model *model;
    size_t operation_index = 0u;
    size_t operation_count;

    if (!document)
        return oa_fail(error, "document is required"), (oa_ui_model *)NULL;
    root = document->root;
    if (!root || json_type(root) != JSON_OBJECT)
        return oa_fail(error, "document root is invalid"), (oa_ui_model *)NULL;
    info = json_object_get(root, "info");
    paths = json_object_get(root, "paths");
    if (!info || json_type(info) != JSON_OBJECT || !paths || json_type(paths) != JSON_OBJECT)
        return oa_fail(error, "document is missing info or paths"), (oa_ui_model *)NULL;

    model = calloc(1u, sizeof(*model));
    if (!model) {
        oa_fail(error, "out of memory constructing OpenAPI UI model");
        return NULL;
    }
    if (!oa_ui_copy_json_string(&model->document.title, json_object_get(info, "title"), "title", error) ||
        !oa_ui_copy_json_string(&model->document.version, json_object_get(info, "version"), "version", error) ||
        !oa_ui_copy_json_string(&model->document.openapi_version, json_object_get(root, "openapi"), "OpenAPI version", error))
        goto fail;

    operation_count = oa_ui_operation_count(paths);
    model->document.operation_count = operation_count;
    if (operation_count) {
        model->operations = calloc(operation_count, sizeof(*model->operations));
        if (!model->operations) {
            oa_fail(error, "out of memory constructing OpenAPI UI operations");
            goto fail;
        }
        model->document.operations = model->operations;
    }

    for (size_t i = 0u; i < json_object_size(paths); ++i) {
        const char *path = json_object_key(paths, i);
        const size_t path_length = json_object_key_len(paths, i);
        const json_value_t *path_item = json_object_value(paths, i);
        if (!path || !path_item || json_type(path_item) != JSON_OBJECT) {
            oa_fail(error, "OpenAPI UI path item is invalid");
            goto fail;
        }
        for (size_t m = 0u; m < OA_ARRAY_COUNT(OA_UI_METHODS); ++m) {
            const json_value_t *operation = json_object_get(path_item, OA_UI_METHODS[m]);
            if (!operation) continue;
            if (operation_index >= operation_count ||
                !oa_ui_project_operation(&model->operations[operation_index], path, path_length,
                                         OA_UI_METHODS[m], operation, error))
                goto fail;
            ++operation_index;
        }
    }
    if (operation_index != operation_count) {
        oa_fail(error, "OpenAPI UI operation projection count changed");
        goto fail;
    }
    return model;

fail:
    oa_ui_model_free(model);
    return NULL;
}

const oa_ui_document *oa_ui_model_document(const oa_ui_model *model) {
    return model ? &model->document : NULL;
}

const cmeta_data_desc *oa_ui_parameter_cmeta_data(void) { return &OA_UI_PARAMETER_DATA; }
const cmeta_data_desc *oa_ui_operation_cmeta_data(void) { return &OA_UI_OPERATION_DATA; }
const cmeta_data_desc *oa_ui_document_cmeta_data(void) { return &OA_UI_DOCUMENT_DATA; }
