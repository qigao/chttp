#include <openapi/ui_model.h>
#include "internal.h"

#include <salts_cmeta_data.h>

#include <stddef.h>

struct oa_ui_model {
    oa_ui_document document;
    oa_ui_operation *operations;
};

static const cmeta_data_buffer_shape OA_UI_BORROWED_STRING_SHAPE = {
    CMETA_DATA_BUFFER_BORROWED
};

static const cmeta_data_desc OA_UI_VSTR_DATA = {
    sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION,
    "openapi.ui.vstr.data", "vstr", CMETA_DATA_STRING,
    &salts_vstr_cmeta_type, &OA_UI_BORROWED_STRING_SHAPE,
    &salts_vstr_cmeta_buffer_ops, NULL, NULL, NULL, NULL
};

static const cmeta_type_identity OA_UI_PARAMETER_ID =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.parameter");
static const cmeta_type_identity OA_UI_OPERATION_ID =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.operation");
static const cmeta_type_identity OA_UI_DOCUMENT_ID =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.document");

static const cmeta_type_desc OA_UI_PARAMETER_TYPE = {
    "oa_ui_parameter", sizeof(oa_ui_parameter), _Alignof(oa_ui_parameter),
    CMETA_T_OBJECT, NULL, NULL, &OA_UI_PARAMETER_ID
};
static const cmeta_type_desc OA_UI_OPERATION_TYPE = {
    "oa_ui_operation", sizeof(oa_ui_operation), _Alignof(oa_ui_operation),
    CMETA_T_OBJECT, NULL, NULL, &OA_UI_OPERATION_ID
};
static const cmeta_type_desc OA_UI_DOCUMENT_TYPE = {
    "oa_ui_document", sizeof(oa_ui_document), _Alignof(oa_ui_document),
    CMETA_T_OBJECT, NULL, NULL, &OA_UI_DOCUMENT_ID
};

#define OA_UI_LAYOUT_FIELD(owner_, field_type_, member_) \
    {#member_, #field_type_, offsetof(owner_, member_), sizeof(((owner_ *)0)->member_), \
     _Alignof(field_type_), NULL, NULL}

static const cmeta_field_desc OA_UI_PARAMETER_LAYOUT_FIELDS[] = {
    OA_UI_LAYOUT_FIELD(oa_ui_parameter, vstr, name),
    OA_UI_LAYOUT_FIELD(oa_ui_parameter, vstr, location),
    OA_UI_LAYOUT_FIELD(oa_ui_parameter, vstr, description),
    OA_UI_LAYOUT_FIELD(oa_ui_parameter, vstr, schema_json),
    OA_UI_LAYOUT_FIELD(oa_ui_parameter, bool, required)
};
static const cmeta_struct_desc OA_UI_PARAMETER_LAYOUT = {
    "oa_ui_parameter", sizeof(oa_ui_parameter), _Alignof(oa_ui_parameter),
    OA_UI_PARAMETER_LAYOUT_FIELDS,
    sizeof(OA_UI_PARAMETER_LAYOUT_FIELDS) / sizeof(OA_UI_PARAMETER_LAYOUT_FIELDS[0])
};
static const cmeta_data_field_desc OA_UI_PARAMETER_FIELDS[] = {
    {"openapi.ui.parameter.name", "name", offsetof(oa_ui_parameter, name), &OA_UI_VSTR_DATA},
    {"openapi.ui.parameter.location", "location", offsetof(oa_ui_parameter, location), &OA_UI_VSTR_DATA},
    {"openapi.ui.parameter.description", "description", offsetof(oa_ui_parameter, description), &OA_UI_VSTR_DATA},
    {"openapi.ui.parameter.schema_json", "schema_json", offsetof(oa_ui_parameter, schema_json), &OA_UI_VSTR_DATA},
    {"openapi.ui.parameter.required", "required", offsetof(oa_ui_parameter, required), &cmeta_data_bool}
};
static const cmeta_data_struct_shape OA_UI_PARAMETER_SHAPE = {
    &OA_UI_PARAMETER_LAYOUT, OA_UI_PARAMETER_FIELDS,
    sizeof(OA_UI_PARAMETER_FIELDS) / sizeof(OA_UI_PARAMETER_FIELDS[0])
};
static const cmeta_data_desc OA_UI_PARAMETER_DATA = {
    sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION,
    "openapi.ui.parameter.data", "oa_ui_parameter", CMETA_DATA_STRUCT,
    &OA_UI_PARAMETER_TYPE, &OA_UI_PARAMETER_SHAPE, NULL, NULL, NULL, NULL, NULL
};

static const cmeta_field_desc OA_UI_OPERATION_LAYOUT_FIELDS[] = {
    OA_UI_LAYOUT_FIELD(oa_ui_operation, vstr, method),
    OA_UI_LAYOUT_FIELD(oa_ui_operation, vstr, path),
    OA_UI_LAYOUT_FIELD(oa_ui_operation, vstr, operation_id),
    OA_UI_LAYOUT_FIELD(oa_ui_operation, vstr, summary),
    OA_UI_LAYOUT_FIELD(oa_ui_operation, vstr, description),
    OA_UI_LAYOUT_FIELD(oa_ui_operation, bool, deprecated),
    OA_UI_LAYOUT_FIELD(oa_ui_operation, const oa_ui_parameter *, parameters),
    OA_UI_LAYOUT_FIELD(oa_ui_operation, size_t, parameter_count),
    OA_UI_LAYOUT_FIELD(oa_ui_operation, vstr, request_body_json),
    OA_UI_LAYOUT_FIELD(oa_ui_operation, vstr, responses_json)
};
static const cmeta_struct_desc OA_UI_OPERATION_LAYOUT = {
    "oa_ui_operation", sizeof(oa_ui_operation), _Alignof(oa_ui_operation),
    OA_UI_OPERATION_LAYOUT_FIELDS,
    sizeof(OA_UI_OPERATION_LAYOUT_FIELDS) / sizeof(OA_UI_OPERATION_LAYOUT_FIELDS[0])
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
    &OA_UI_OPERATION_LAYOUT, OA_UI_OPERATION_FIELDS,
    sizeof(OA_UI_OPERATION_FIELDS) / sizeof(OA_UI_OPERATION_FIELDS[0])
};
static const cmeta_data_desc OA_UI_OPERATION_DATA = {
    sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION,
    "openapi.ui.operation.data", "oa_ui_operation", CMETA_DATA_STRUCT,
    &OA_UI_OPERATION_TYPE, &OA_UI_OPERATION_SHAPE, NULL, NULL, NULL, NULL, NULL
};

static const cmeta_field_desc OA_UI_DOCUMENT_LAYOUT_FIELDS[] = {
    OA_UI_LAYOUT_FIELD(oa_ui_document, vstr, title),
    OA_UI_LAYOUT_FIELD(oa_ui_document, vstr, version),
    OA_UI_LAYOUT_FIELD(oa_ui_document, vstr, openapi_version),
    OA_UI_LAYOUT_FIELD(oa_ui_document, const oa_ui_operation *, operations),
    OA_UI_LAYOUT_FIELD(oa_ui_document, size_t, operation_count)
};
static const cmeta_struct_desc OA_UI_DOCUMENT_LAYOUT = {
    "oa_ui_document", sizeof(oa_ui_document), _Alignof(oa_ui_document),
    OA_UI_DOCUMENT_LAYOUT_FIELDS,
    sizeof(OA_UI_DOCUMENT_LAYOUT_FIELDS) / sizeof(OA_UI_DOCUMENT_LAYOUT_FIELDS[0])
};
static const cmeta_data_field_desc OA_UI_DOCUMENT_FIELDS[] = {
    {"openapi.ui.document.title", "title", offsetof(oa_ui_document, title), &OA_UI_VSTR_DATA},
    {"openapi.ui.document.version", "version", offsetof(oa_ui_document, version), &OA_UI_VSTR_DATA},
    {"openapi.ui.document.openapi_version", "openapi_version", offsetof(oa_ui_document, openapi_version), &OA_UI_VSTR_DATA},
    {"openapi.ui.document.operation_count", "operation_count", offsetof(oa_ui_document, operation_count), &cmeta_data_size}
};
static const cmeta_data_struct_shape OA_UI_DOCUMENT_SHAPE = {
    &OA_UI_DOCUMENT_LAYOUT, OA_UI_DOCUMENT_FIELDS,
    sizeof(OA_UI_DOCUMENT_FIELDS) / sizeof(OA_UI_DOCUMENT_FIELDS[0])
};
static const cmeta_data_desc OA_UI_DOCUMENT_DATA = {
    sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION,
    "openapi.ui.document.data", "oa_ui_document", CMETA_DATA_STRUCT,
    &OA_UI_DOCUMENT_TYPE, &OA_UI_DOCUMENT_SHAPE, NULL, NULL, NULL, NULL, NULL
};

#undef OA_UI_LAYOUT_FIELD

static int ui_own_bytes(vstr source, vstr *out) {
    char *copy;
    if (!out) return 0;
    *out = (vstr){0};
    if (source.len == 0u) return 1;
    if (!source.data) return 0;
    copy = malloc(source.len + 1u);
    if (!copy) return 0;
    memcpy(copy, source.data, source.len);
    copy[source.len] = '\0';
    *out = vstr_from_buf(copy, source.len);
    return 1;
}

static int ui_own_cstr(const char *text, vstr *out) {
    return ui_own_bytes(text ? vstr_from_cstr(text) : (vstr){0}, out);
}

static int ui_own_json(const json_value_t *value, vstr *out) {
    size_t length = 0u;
    char *serialized;
    char *copy;
    if (!out) return 0;
    *out = (vstr){0};
    if (!value) return 1;
    serialized = json_serialize_pretty(value, &length);
    if (!serialized) return 0;
    copy = malloc(length + 1u);
    if (!copy) {
        json_serialize_free(serialized);
        return 0;
    }
    memcpy(copy, serialized, length);
    copy[length] = '\0';
    json_serialize_free(serialized);
    *out = vstr_from_buf(copy, length);
    return 1;
}

static void ui_release_vstr(vstr *value) {
    if (!value) return;
    free((void *)value->data);
    *value = (vstr){0};
}

static const json_value_t *ui_object(const json_value_t *value, const char *key) {
    const json_value_t *child = value ? json_object_get(value, key) : NULL;
    return child && json_type(child) == JSON_OBJECT ? child : NULL;
}

static vstr ui_json_string_view(const json_value_t *object, const char *key) {
    const json_value_t *value = object ? json_object_get(object, key) : NULL;
    if (!value || json_type(value) != JSON_STRING)
        return (vstr){0};
    return vstr_from_buf(json_string(value), json_string_len(value));
}

static int ui_json_bool(const json_value_t *object, const char *key) {
    const json_value_t *value = object ? json_object_get(object, key) : NULL;
    return value && json_type(value) == JSON_BOOL && json_bool(value);
}

static size_t ui_operation_count(const json_value_t *paths) {
    static const char *const methods[] = {
        "get", "post", "put", "patch", "delete", "head", "options", "trace"
    };
    size_t count = 0u;
    if (!paths) return 0u;
    for (size_t i = 0u; i < json_object_size(paths); ++i) {
        const json_value_t *item = json_object_value(paths, i);
        if (!item || json_type(item) != JSON_OBJECT) continue;
        for (size_t method = 0u; method < sizeof(methods) / sizeof(methods[0]); ++method)
            if (json_object_get(item, methods[method])) ++count;
    }
    return count;
}

static void ui_parameter_release(oa_ui_parameter *parameter) {
    if (!parameter) return;
    ui_release_vstr(&parameter->name);
    ui_release_vstr(&parameter->location);
    ui_release_vstr(&parameter->description);
    ui_release_vstr(&parameter->schema_json);
}

static void ui_operation_release(oa_ui_operation *operation) {
    if (!operation) return;
    ui_release_vstr(&operation->method);
    ui_release_vstr(&operation->path);
    ui_release_vstr(&operation->operation_id);
    ui_release_vstr(&operation->summary);
    ui_release_vstr(&operation->description);
    ui_release_vstr(&operation->request_body_json);
    ui_release_vstr(&operation->responses_json);
    if (operation->parameters) {
        oa_ui_parameter *parameters = (oa_ui_parameter *)operation->parameters;
        for (size_t i = 0u; i < operation->parameter_count; ++i)
            ui_parameter_release(&parameters[i]);
        free(parameters);
    }
    operation->parameters = NULL;
    operation->parameter_count = 0u;
}

static int ui_parameter_project(oa_ui_parameter *out, const json_value_t *parameter) {
    if (!out || !parameter || json_type(parameter) != JSON_OBJECT) return 0;
    memset(out, 0, sizeof(*out));
    if (!ui_own_bytes(ui_json_string_view(parameter, "name"), &out->name) ||
        !ui_own_bytes(ui_json_string_view(parameter, "in"), &out->location) ||
        !ui_own_bytes(ui_json_string_view(parameter, "description"), &out->description) ||
        !ui_own_json(json_object_get(parameter, "schema"), &out->schema_json)) {
        ui_parameter_release(out);
        return 0;
    }
    out->required = ui_json_bool(parameter, "required") != 0;
    return 1;
}

static int ui_operation_project(oa_ui_operation *out, const char *path,
                                const char *method, const json_value_t *operation) {
    const json_value_t *parameters;
    size_t parameter_count;
    if (!out || !path || !method || !operation || json_type(operation) != JSON_OBJECT) return 0;
    memset(out, 0, sizeof(*out));

    if (!ui_own_cstr(method, &out->method) ||
        !ui_own_cstr(path, &out->path) ||
        !ui_own_bytes(ui_json_string_view(operation, "operationId"), &out->operation_id) ||
        !ui_own_bytes(ui_json_string_view(operation, "summary"), &out->summary) ||
        !ui_own_bytes(ui_json_string_view(operation, "description"), &out->description) ||
        !ui_own_json(json_object_get(operation, "requestBody"), &out->request_body_json) ||
        !ui_own_json(json_object_get(operation, "responses"), &out->responses_json)) {
        ui_operation_release(out);
        return 0;
    }
    out->deprecated = ui_json_bool(operation, "deprecated") != 0;

    parameters = json_object_get(operation, "parameters");
    parameter_count = parameters && json_type(parameters) == JSON_ARRAY
        ? json_array_size(parameters) : 0u;
    if (parameter_count != 0u) {
        oa_ui_parameter *items = calloc(parameter_count, sizeof(*items));
        if (!items) {
            ui_operation_release(out);
            return 0;
        }
        out->parameters = items;
        out->parameter_count = parameter_count;
        for (size_t i = 0u; i < parameter_count; ++i) {
            if (!ui_parameter_project(&items[i], json_array_get(parameters, i))) {
                ui_operation_release(out);
                return 0;
            }
        }
    }
    return 1;
}

oa_ui_model *oa_ui_model_create(const oa_document *document, oa_error *error) {
    static const char *const methods[] = {
        "get", "post", "put", "patch", "delete", "head", "options", "trace"
    };
    const json_value_t *info;
    const json_value_t *paths;
    oa_ui_model *model;
    size_t operation_count;
    size_t operation_index = 0u;

    if (!document || !document->root) {
        oa_fail(error, "document is required");
        return NULL;
    }
    info = ui_object(document->root, "info");
    paths = ui_object(document->root, "paths");
    if (!info || !paths) {
        oa_fail(error, "document is missing info or paths");
        return NULL;
    }

    operation_count = ui_operation_count(paths);
    model = calloc(1, sizeof(*model));
    if (!model) {
        oa_fail(error, "out of memory projecting UI document");
        return NULL;
    }
    if (operation_count != 0u) {
        model->operations = calloc(operation_count, sizeof(*model->operations));
        if (!model->operations) {
            oa_ui_model_free(model);
            oa_fail(error, "out of memory projecting UI operations");
            return NULL;
        }
    }
    model->document.operations = model->operations;
    model->document.operation_count = operation_count;

    if (!ui_own_bytes(ui_json_string_view(info, "title"), &model->document.title) ||
        !ui_own_bytes(ui_json_string_view(info, "version"), &model->document.version) ||
        !ui_own_bytes(ui_json_string_view(document->root, "openapi"),
                      &model->document.openapi_version)) {
        oa_ui_model_free(model);
        oa_fail(error, "out of memory projecting UI document strings");
        return NULL;
    }

    for (size_t path_index = 0u; path_index < json_object_size(paths); ++path_index) {
        const char *path = json_object_key(paths, path_index);
        const json_value_t *item = json_object_value(paths, path_index);
        if (!path || !item || json_type(item) != JSON_OBJECT) continue;
        for (size_t method_index = 0u;
             method_index < sizeof(methods) / sizeof(methods[0]); ++method_index) {
            const json_value_t *operation = json_object_get(item, methods[method_index]);
            if (!operation) continue;
            if (operation_index >= operation_count ||
                !ui_operation_project(&model->operations[operation_index], path,
                                      methods[method_index], operation)) {
                oa_ui_model_free(model);
                oa_fail(error, "out of memory projecting UI operation");
                return NULL;
            }
            ++operation_index;
        }
    }
    if (operation_index != operation_count) {
        oa_ui_model_free(model);
        oa_fail(error, "document changed during UI projection");
        return NULL;
    }
    return model;
}

void oa_ui_model_free(oa_ui_model *model) {
    if (!model) return;
    ui_release_vstr(&model->document.title);
    ui_release_vstr(&model->document.version);
    ui_release_vstr(&model->document.openapi_version);
    if (model->operations) {
        for (size_t i = 0u; i < model->document.operation_count; ++i)
            ui_operation_release(&model->operations[i]);
        free(model->operations);
    }
    free(model);
}

const oa_ui_document *oa_ui_model_document(const oa_ui_model *model) {
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
