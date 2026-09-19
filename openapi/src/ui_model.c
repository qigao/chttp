#include <openapi/ui_model.h>
#include "internal.h"
#include "ui_model_internal.h"

#include <salts_cmeta_data.h>

#include <stddef.h>

#define OA_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

struct oa_ui_model {
    oa_ui_allocator allocator;
    json_value_t *snapshot;
    oa_ui_document document;
    oa_ui_operation *operations;
};

static const char *const OA_UI_METHODS[] = {
    "get", "post", "put", "patch", "delete", "head", "options", "trace"
};

static void *oa_ui_system_calloc(void *userdata, size_t count, size_t size) {
    (void)userdata;
    return calloc(count, size);
}

static void oa_ui_system_free(void *userdata, void *memory) {
    (void)userdata;
    free(memory);
}

static const oa_ui_allocator OA_UI_SYSTEM_ALLOCATOR = {
    oa_ui_system_calloc, oa_ui_system_free, NULL
};

static void *oa_ui_calloc(oa_ui_model *model, size_t count, size_t size) {
    if (!model || !model->allocator.calloc_fn || !count || !size ||
        count > SIZE_MAX / size)
        return NULL;
    return model->allocator.calloc_fn(model->allocator.userdata, count, size);
}

static void oa_ui_free(oa_ui_model *model, void *memory) {
    if (model && memory && model->allocator.free_fn)
        model->allocator.free_fn(model->allocator.userdata, memory);
}

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

static const cmeta_type_identity OA_UI_SEQUENCE_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.sequence_view");
static const cmeta_type_desc OA_UI_SEQUENCE_TYPE = {
    "oa_ui_sequence_view", sizeof(oa_ui_sequence_view), _Alignof(oa_ui_sequence_view),
    CMETA_T_OBJECT, NULL, NULL, &OA_UI_SEQUENCE_IDENTITY
};

#define OA_LAYOUT_FIELD(owner, member, field_type_, type_name_, type_) \
    {#member, type_name_, offsetof(owner, member), sizeof(((owner *)0)->member), \
     _Alignof(field_type_), type_, NULL}

static const cmeta_field_desc OA_UI_PARAMETER_LAYOUT_FIELDS[] = {
    OA_LAYOUT_FIELD(oa_ui_parameter, name, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_parameter, location, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_parameter, description, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_parameter, schema_json, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_parameter, required, bool, "bool", NULL)
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
    "oa_ui_parameter", sizeof(oa_ui_parameter), _Alignof(oa_ui_parameter),
    CMETA_T_OBJECT, NULL, NULL, &OA_UI_PARAMETER_IDENTITY
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
    OA_LAYOUT_FIELD(oa_ui_operation, method, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_operation, path, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_operation, operation_id, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_operation, summary, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_operation, description, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_operation, tags, oa_ui_sequence_view, "oa_ui_sequence_view", &OA_UI_SEQUENCE_TYPE),
    OA_LAYOUT_FIELD(oa_ui_operation, parameters, oa_ui_sequence_view, "oa_ui_sequence_view", &OA_UI_SEQUENCE_TYPE),
    OA_LAYOUT_FIELD(oa_ui_operation, request_body_json, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_operation, responses_json, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_operation, deprecated, bool, "bool", NULL)
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
    {"openapi.ui.operation.tags", "tags", offsetof(oa_ui_operation, tags), &cmeta_data_sequence},
    {"openapi.ui.operation.parameters", "parameters", offsetof(oa_ui_operation, parameters), &cmeta_data_sequence},
    {"openapi.ui.operation.request_body_json", "request_body_json", offsetof(oa_ui_operation, request_body_json), &OA_UI_VSTR_DATA},
    {"openapi.ui.operation.responses_json", "responses_json", offsetof(oa_ui_operation, responses_json), &OA_UI_VSTR_DATA},
    {"openapi.ui.operation.deprecated", "deprecated", offsetof(oa_ui_operation, deprecated), &cmeta_data_bool}
};
static const cmeta_data_struct_shape OA_UI_OPERATION_SHAPE = {
    &OA_UI_OPERATION_LAYOUT, OA_UI_OPERATION_FIELDS, OA_ARRAY_COUNT(OA_UI_OPERATION_FIELDS)
};
static const cmeta_type_identity OA_UI_OPERATION_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.operation");
static const cmeta_type_desc OA_UI_OPERATION_TYPE = {
    "oa_ui_operation", sizeof(oa_ui_operation), _Alignof(oa_ui_operation),
    CMETA_T_OBJECT, NULL, NULL, &OA_UI_OPERATION_IDENTITY
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
    OA_LAYOUT_FIELD(oa_ui_document, title, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_document, version, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_document, openapi_version, vstr, "vstr", &salts_vstr_cmeta_type),
    OA_LAYOUT_FIELD(oa_ui_document, operations, oa_ui_sequence_view, "oa_ui_sequence_view", &OA_UI_SEQUENCE_TYPE)
};
static const cmeta_struct_desc OA_UI_DOCUMENT_LAYOUT = {
    "oa_ui_document", sizeof(oa_ui_document), _Alignof(oa_ui_document),
    OA_UI_DOCUMENT_LAYOUT_FIELDS, OA_ARRAY_COUNT(OA_UI_DOCUMENT_LAYOUT_FIELDS)
};
static const cmeta_data_field_desc OA_UI_DOCUMENT_FIELDS[] = {
    {"openapi.ui.document.title", "title", offsetof(oa_ui_document, title), &OA_UI_VSTR_DATA},
    {"openapi.ui.document.version", "version", offsetof(oa_ui_document, version), &OA_UI_VSTR_DATA},
    {"openapi.ui.document.openapi_version", "openapi_version", offsetof(oa_ui_document, openapi_version), &OA_UI_VSTR_DATA},
    {"openapi.ui.document.operations", "operations", offsetof(oa_ui_document, operations), &cmeta_data_sequence}
};
static const cmeta_data_struct_shape OA_UI_DOCUMENT_SHAPE = {
    &OA_UI_DOCUMENT_LAYOUT, OA_UI_DOCUMENT_FIELDS, OA_ARRAY_COUNT(OA_UI_DOCUMENT_FIELDS)
};
static const cmeta_type_identity OA_UI_DOCUMENT_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.document");
static const cmeta_type_desc OA_UI_DOCUMENT_TYPE = {
    "oa_ui_document", sizeof(oa_ui_document), _Alignof(oa_ui_document),
    CMETA_T_OBJECT, NULL, NULL, &OA_UI_DOCUMENT_IDENTITY
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

#undef OA_LAYOUT_FIELD

static vstr oa_ui_empty(void) {
    return (vstr){NULL, 0u};
}

static int oa_ui_borrow_string(vstr *out, const json_value_t *value,
                               const char *field, int required, oa_error *error) {
    if (!out) return oa_fail(error, "invalid OpenAPI UI string destination");
    if (!value) {
        if (required) return oa_fail(error, "OpenAPI UI %s is required", field);
        *out = oa_ui_empty();
        return 1;
    }
    if (json_type(value) != JSON_STRING)
        return oa_fail(error, "OpenAPI UI %s must be a string", field);
    *out = vstr_from_buf(json_string(value), json_string_len(value));
    return 1;
}

static int oa_ui_serialize(vstr *out, const json_value_t *value,
                           const char *field, oa_error *error) {
    size_t length = 0u;
    char *text;
    if (!out) return oa_fail(error, "invalid OpenAPI UI JSON destination");
    if (!value) {
        *out = oa_ui_empty();
        return 1;
    }
    text = json_serialize_pretty(value, &length);
    if (!text) return oa_fail(error, "failed to serialize OpenAPI UI %s", field);
    *out = vstr_from_buf(text, length);
    return 1;
}

static void oa_ui_serialized_free(vstr *value) {
    if (!value || !value->data) return;
    json_serialize_free((char *)value->data);
    *value = oa_ui_empty();
}

static oa_ui_sequence_view oa_ui_sequence_empty(size_t stride,
                                                const cmeta_data_desc *element) {
    oa_ui_sequence_view view = {NULL, 0u, stride, element};
    return view;
}

static void oa_ui_parameter_free(oa_ui_parameter *parameter) {
    if (!parameter) return;
    oa_ui_serialized_free(&parameter->schema_json);
}

static void oa_ui_operation_free(oa_ui_operation *operation) {
    if (!operation) return;
    if (operation->parameters.data) {
        oa_ui_parameter *parameters = (oa_ui_parameter *)operation->parameters.data;
        for (size_t i = 0u; i < operation->parameters.count; ++i)
            oa_ui_parameter_free(&parameters[i]);
        free(parameters);
    }
    free((void *)operation->tags.data);
    oa_ui_serialized_free(&operation->request_body_json);
    oa_ui_serialized_free(&operation->responses_json);
    operation->tags = oa_ui_sequence_empty(sizeof(vstr), &OA_UI_VSTR_DATA);
    operation->parameters =
        oa_ui_sequence_empty(sizeof(oa_ui_parameter), &OA_UI_PARAMETER_DATA);
}

void oa_ui_model_free(oa_ui_model *model) {
    if (!model) return;
    if (model->operations) {
        for (size_t i = 0u; i < model->document.operations.count; ++i)
            oa_ui_operation_free(&model->operations[i]);
        free(model->operations);
    }
    json_free(model->snapshot);
    free(model);
}

static size_t oa_ui_count_operations(const json_value_t *paths) {
    size_t count = 0u;
    for (size_t i = 0u; i < json_object_size(paths); ++i) {
        const json_value_t *path_item = json_object_value(paths, i);
        if (!path_item || json_type(path_item) != JSON_OBJECT) continue;
        for (size_t m = 0u; m < OA_ARRAY_COUNT(OA_UI_METHODS); ++m)
            if (json_object_get(path_item, OA_UI_METHODS[m])) ++count;
    }
    return count;
}

static int oa_ui_project_tags(oa_ui_operation *out, const json_value_t *operation,
                              oa_error *error) {
    const json_value_t *tags = json_object_get(operation, "tags");
    out->tags = oa_ui_sequence_empty(sizeof(vstr), &OA_UI_VSTR_DATA);
    if (!tags) return 1;
    if (json_type(tags) != JSON_ARRAY)
        return oa_fail(error, "OpenAPI UI tags must be an array");
    size_t count = json_array_size(tags);
    if (!count) return 1;
    vstr *items = calloc(count, sizeof(*items));
    if (!items) return oa_fail(error, "out of memory constructing OpenAPI UI tags");
    out->tags.data = items;
    out->tags.count = count;
    for (size_t i = 0u; i < count; ++i) {
        if (!oa_ui_borrow_string(&items[i], json_array_get(tags, i), "tag", 1, error))
            return 0;
    }
    return 1;
}

static int oa_ui_project_parameter(oa_ui_parameter *out,
                                   const json_value_t *parameter,
                                   oa_error *error) {
    const json_value_t *required;
    if (!parameter || json_type(parameter) != JSON_OBJECT)
        return oa_fail(error, "OpenAPI UI parameter must be an object");
    if (!oa_ui_borrow_string(&out->name, json_object_get(parameter, "name"),
                             "parameter name", 1, error) ||
        !oa_ui_borrow_string(&out->location, json_object_get(parameter, "in"),
                             "parameter location", 1, error) ||
        !oa_ui_borrow_string(&out->description,
                             json_object_get(parameter, "description"),
                             "parameter description", 0, error) ||
        !oa_ui_serialize(&out->schema_json, json_object_get(parameter, "schema"),
                         "parameter schema", error))
        return 0;
    required = json_object_get(parameter, "required");
    if (required && json_type(required) != JSON_BOOL)
        return oa_fail(error, "OpenAPI UI parameter required must be boolean");
    out->required = required ? json_bool(required) : false;
    return 1;
}

static int oa_ui_project_parameters(oa_ui_operation *out,
                                    const json_value_t *operation,
                                    oa_error *error) {
    const json_value_t *parameters = json_object_get(operation, "parameters");
    out->parameters =
        oa_ui_sequence_empty(sizeof(oa_ui_parameter), &OA_UI_PARAMETER_DATA);
    if (!parameters) return 1;
    if (json_type(parameters) != JSON_ARRAY)
        return oa_fail(error, "OpenAPI UI parameters must be an array");
    size_t count = json_array_size(parameters);
    if (!count) return 1;
    oa_ui_parameter *items = calloc(count, sizeof(*items));
    if (!items)
        return oa_fail(error, "out of memory constructing OpenAPI UI parameters");
    out->parameters.data = items;
    out->parameters.count = count;
    for (size_t i = 0u; i < count; ++i)
        if (!oa_ui_project_parameter(&items[i], json_array_get(parameters, i), error))
            return 0;
    return 1;
}

static int oa_ui_project_operation(oa_ui_operation *out,
                                   const char *path, size_t path_length,
                                   const char *method,
                                   const json_value_t *operation,
                                   oa_error *error) {
    const json_value_t *deprecated;
    if (!operation || json_type(operation) != JSON_OBJECT)
        return oa_fail(error, "OpenAPI UI operation must be an object");
    out->method = vstr_from_cstr(method);
    out->path = vstr_from_buf(path, path_length);
    if (!oa_ui_borrow_string(&out->operation_id,
                             json_object_get(operation, "operationId"),
                             "operationId", 0, error) ||
        !oa_ui_borrow_string(&out->summary, json_object_get(operation, "summary"),
                             "summary", 0, error) ||
        !oa_ui_borrow_string(&out->description,
                             json_object_get(operation, "description"),
                             "description", 0, error) ||
        !oa_ui_project_tags(out, operation, error) ||
        !oa_ui_project_parameters(out, operation, error) ||
        !oa_ui_serialize(&out->request_body_json,
                         json_object_get(operation, "requestBody"),
                         "request body", error) ||
        !oa_ui_serialize(&out->responses_json,
                         json_object_get(operation, "responses"),
                         "responses", error))
        return 0;
    deprecated = json_object_get(operation, "deprecated");
    if (deprecated && json_type(deprecated) != JSON_BOOL)
        return oa_fail(error, "OpenAPI UI deprecated must be boolean");
    out->deprecated = deprecated ? json_bool(deprecated) : false;
    return 1;
}

static int oa_ui_project_snapshot(oa_ui_model *model, oa_error *error) {
    const json_value_t *root = model->snapshot;
    const json_value_t *info = json_object_get(root, "info");
    const json_value_t *paths = json_object_get(root, "paths");
    size_t operation_index = 0u;

    if (!info || json_type(info) != JSON_OBJECT ||
        !paths || json_type(paths) != JSON_OBJECT)
        return oa_fail(error, "OpenAPI UI document requires info and paths");
    if (!oa_ui_borrow_string(&model->document.title,
                             json_object_get(info, "title"),
                             "title", 1, error) ||
        !oa_ui_borrow_string(&model->document.version,
                             json_object_get(info, "version"),
                             "version", 1, error) ||
        !oa_ui_borrow_string(&model->document.openapi_version,
                             json_object_get(root, "openapi"),
                             "OpenAPI version", 1, error))
        return 0;

    size_t operation_count = oa_ui_count_operations(paths);
    model->document.operations =
        oa_ui_sequence_empty(sizeof(oa_ui_operation), &OA_UI_OPERATION_DATA);
    if (operation_count) {
        model->operations = calloc(operation_count, sizeof(*model->operations));
        if (!model->operations)
            return oa_fail(error, "out of memory constructing OpenAPI UI operations");
        model->document.operations.data = model->operations;
        model->document.operations.count = operation_count;
    }

    for (size_t i = 0u; i < json_object_size(paths); ++i) {
        const char *path = json_object_key(paths, i);
        size_t path_length = json_object_key_len(paths, i);
        const json_value_t *path_item = json_object_value(paths, i);
        if (!path || !path_item || json_type(path_item) != JSON_OBJECT)
            return oa_fail(error, "OpenAPI UI path item must be an object");
        for (size_t m = 0u; m < OA_ARRAY_COUNT(OA_UI_METHODS); ++m) {
            const json_value_t *operation =
                json_object_get(path_item, OA_UI_METHODS[m]);
            if (!operation) continue;
            if (operation_index >= operation_count ||
                !oa_ui_project_operation(&model->operations[operation_index],
                                         path, path_length, OA_UI_METHODS[m],
                                         operation, error))
                return 0;
            ++operation_index;
        }
    }
    if (operation_index != operation_count)
        return oa_fail(error, "OpenAPI UI operation projection count changed");
    return 1;
}

oa_ui_model *oa_ui_model_create_json(const json_value_t *root, oa_error *error) {
    oa_ui_model *model;
    if (!root)
        return oa_fail(error, "JSON document is required"), (oa_ui_model *)NULL;
    if (json_type(root) != JSON_OBJECT)
        return oa_fail(error, "JSON document root must be an object"),
               (oa_ui_model *)NULL;
    model = calloc(1u, sizeof(*model));
    if (!model) {
        oa_fail(error, "out of memory constructing OpenAPI UI model");
        return NULL;
    }
    model->snapshot = json_clone(root);
    if (!model->snapshot) {
        oa_fail(error, "out of memory cloning OpenAPI UI document");
        oa_ui_model_free(model);
        return NULL;
    }
    if (!oa_ui_project_snapshot(model, error)) {
        oa_ui_model_free(model);
        return NULL;
    }
    return model;
}

oa_ui_model *oa_ui_model_create(const oa_document *document, oa_error *error) {
    if (!document)
        return oa_fail(error, "document is required"), (oa_ui_model *)NULL;
    return oa_ui_model_create_json(document->root, error);
}

const oa_ui_document *oa_ui_model_view(const oa_ui_model *model) {
    return model ? &model->document : NULL;
}

const cmeta_data_desc *oa_ui_parameter_cmeta_data(void) {
    return &OA_UI_PARAMETER_DATA;
}
const cmeta_data_desc *oa_ui_operation_cmeta_data(void) {
    return &OA_UI_OPERATION_DATA;
}
const cmeta_data_desc *oa_ui_document_cmeta_data(void) {
    return &OA_UI_DOCUMENT_DATA;
}
