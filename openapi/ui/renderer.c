#include "renderer.h"

#include <jinja_cmeta.h>
#include <jinja_cmeta_runtime.h>

#include <cmeta/struct.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OA_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct oa_ui_jinja_operation {
    vstr method;
    vstr path;
    vstr operation_id;
    vstr summary;
    vstr description;
    JINJA_CMETA_SEQUENCE_VIEW tags;
    JINJA_CMETA_SEQUENCE_VIEW parameters;
    vstr request_body_json;
    vstr responses_json;
    bool deprecated;
} oa_ui_jinja_operation;

typedef struct oa_ui_jinja_document {
    vstr title;
    vstr version;
    vstr openapi_version;
    JINJA_CMETA_SEQUENCE_VIEW operations;
} oa_ui_jinja_document;

typedef struct oa_ui_renderer_impl {
    const oa_ui_document *source;
    oa_ui_jinja_document root;
    oa_ui_jinja_operation *operations;

    JINJA_CMETA_ENV *env;
    JINJA_CMETA_TEMPLATE *templ;
    JINJA_CMETA_RUNTIME_CONFIG *runtime;
    JINJA_CMETA_RENDER_OPTIONS render_options;

    cmeta_data_field_desc parameter_fields[5];
    cmeta_data_struct_shape parameter_shape;
    cmeta_data_desc parameter_data;

    cmeta_data_field_desc operation_fields[10];
    cmeta_data_struct_shape operation_shape;
    cmeta_data_desc operation_data;

    cmeta_data_field_desc document_fields[4];
    cmeta_data_struct_shape document_shape;
    cmeta_data_desc document_data;
} oa_ui_renderer_impl;

static const cmeta_type_identity OA_PARAMETER_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.jinja.parameter");
static const cmeta_type_identity OA_OPERATION_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.jinja.operation");
static const cmeta_type_identity OA_DOCUMENT_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.jinja.document");

static const cmeta_type_desc OA_PARAMETER_TYPE = {
    "oa_ui_parameter", sizeof(oa_ui_parameter), _Alignof(oa_ui_parameter),
    CMETA_T_OBJECT, NULL, NULL, &OA_PARAMETER_IDENTITY
};
static const cmeta_type_desc OA_OPERATION_TYPE = {
    "oa_ui_jinja_operation", sizeof(oa_ui_jinja_operation), _Alignof(oa_ui_jinja_operation),
    CMETA_T_OBJECT, NULL, NULL, &OA_OPERATION_IDENTITY
};
static const cmeta_type_desc OA_DOCUMENT_TYPE = {
    "oa_ui_jinja_document", sizeof(oa_ui_jinja_document), _Alignof(oa_ui_jinja_document),
    CMETA_T_OBJECT, NULL, NULL, &OA_DOCUMENT_IDENTITY
};

#define OA_LAYOUT_FIELD(owner, member, field_type_, type_name_) \
    {#member, type_name_, offsetof(owner, member), sizeof(((owner *)0)->member), \
     _Alignof(field_type_), NULL, NULL}

static const cmeta_field_desc OA_PARAMETER_LAYOUT_FIELDS[] = {
    OA_LAYOUT_FIELD(oa_ui_parameter, name, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_parameter, location, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_parameter, description, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_parameter, schema_json, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_parameter, required, bool, "bool")
};
static const cmeta_struct_desc OA_PARAMETER_LAYOUT = {
    "oa_ui_parameter", sizeof(oa_ui_parameter), _Alignof(oa_ui_parameter),
    OA_PARAMETER_LAYOUT_FIELDS, OA_ARRAY_COUNT(OA_PARAMETER_LAYOUT_FIELDS)
};

static const cmeta_field_desc OA_OPERATION_LAYOUT_FIELDS[] = {
    OA_LAYOUT_FIELD(oa_ui_jinja_operation, method, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_operation, path, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_operation, operation_id, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_operation, summary, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_operation, description, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_operation, tags, JINJA_CMETA_SEQUENCE_VIEW, "JINJA_CMETA_SEQUENCE_VIEW"),
    OA_LAYOUT_FIELD(oa_ui_jinja_operation, parameters, JINJA_CMETA_SEQUENCE_VIEW, "JINJA_CMETA_SEQUENCE_VIEW"),
    OA_LAYOUT_FIELD(oa_ui_jinja_operation, request_body_json, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_operation, responses_json, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_operation, deprecated, bool, "bool")
};
static const cmeta_struct_desc OA_OPERATION_LAYOUT = {
    "oa_ui_jinja_operation", sizeof(oa_ui_jinja_operation), _Alignof(oa_ui_jinja_operation),
    OA_OPERATION_LAYOUT_FIELDS, OA_ARRAY_COUNT(OA_OPERATION_LAYOUT_FIELDS)
};

static const cmeta_field_desc OA_DOCUMENT_LAYOUT_FIELDS[] = {
    OA_LAYOUT_FIELD(oa_ui_jinja_document, title, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_document, version, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_document, openapi_version, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_document, operations, JINJA_CMETA_SEQUENCE_VIEW, "JINJA_CMETA_SEQUENCE_VIEW")
};
static const cmeta_struct_desc OA_DOCUMENT_LAYOUT = {
    "oa_ui_jinja_document", sizeof(oa_ui_jinja_document), _Alignof(oa_ui_jinja_document),
    OA_DOCUMENT_LAYOUT_FIELDS, OA_ARRAY_COUNT(OA_DOCUMENT_LAYOUT_FIELDS)
};

#undef OA_LAYOUT_FIELD

static oa_ui_renderer_status oa_ui_renderer_fail(
    oa_ui_renderer_error *error, oa_ui_renderer_status status, const char *message) {
    if (error) {
        error->status = status;
        if (message) {
            (void)snprintf(error->message, sizeof(error->message), "%s", message);
        } else {
            error->message[0] = '\0';
        }
    }
    return status;
}

static oa_ui_renderer_status oa_ui_renderer_from_jinja(
    oa_ui_renderer_error *error, JINJA_CMETA_STATUS status, int compiling) {
    switch (status) {
    case JINJA_CMETA_OK:
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_OK, NULL);
    case JINJA_CMETA_ERR_CAPACITY:
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_CAPACITY,
                                   compiling ? "template exceeds renderer limits"
                                             : "render exceeds renderer limits");
    case JINJA_CMETA_ERR_OUT_OF_MEMORY:
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_OUT_OF_MEMORY,
                                   "renderer is out of memory");
    default:
        return oa_ui_renderer_fail(error,
                                   compiling ? OA_UI_RENDERER_TEMPLATE
                                             : OA_UI_RENDERER_RENDER,
                                   compiling ? "template compilation failed"
                                             : "template rendering failed");
    }
}

static int oa_ui_html_autoescape(void *userdata, vstr name) {
    (void)userdata;
    (void)name;
    return 1;
}

static int oa_ui_renderer_config_valid(const oa_ui_renderer_config *config) {
    return config != NULL &&
           config->max_template_bytes != 0u &&
           config->max_output_bytes != 0u &&
           config->max_nodes != 0u &&
           config->max_value_visits != 0u &&
           config->max_render_depth != 0u;
}

static void oa_ui_renderer_init_descriptors(oa_ui_renderer_impl *impl) {
    const cmeta_data_desc *text = jinja_cmeta_vstr_data();
    const cmeta_data_desc *sequence = jinja_cmeta_sequence_data();

    impl->parameter_fields[0] = (cmeta_data_field_desc){
        "openapi.ui.jinja.parameter.name", "name", offsetof(oa_ui_parameter, name), text};
    impl->parameter_fields[1] = (cmeta_data_field_desc){
        "openapi.ui.jinja.parameter.location", "location", offsetof(oa_ui_parameter, location), text};
    impl->parameter_fields[2] = (cmeta_data_field_desc){
        "openapi.ui.jinja.parameter.description", "description", offsetof(oa_ui_parameter, description), text};
    impl->parameter_fields[3] = (cmeta_data_field_desc){
        "openapi.ui.jinja.parameter.schema_json", "schema_json", offsetof(oa_ui_parameter, schema_json), text};
    impl->parameter_fields[4] = (cmeta_data_field_desc){
        "openapi.ui.jinja.parameter.required", "required", offsetof(oa_ui_parameter, required), &cmeta_data_bool};
    impl->parameter_shape = (cmeta_data_struct_shape){
        &OA_PARAMETER_LAYOUT, impl->parameter_fields, OA_ARRAY_COUNT(impl->parameter_fields)};
    impl->parameter_data = (cmeta_data_desc){
        sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION,
        "openapi.ui.jinja.parameter.data", "OpenAPI UI Jinja parameter",
        CMETA_DATA_STRUCT, &OA_PARAMETER_TYPE, &impl->parameter_shape,
        NULL, NULL, NULL};

    impl->operation_fields[0] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.method", "method", offsetof(oa_ui_jinja_operation, method), text};
    impl->operation_fields[1] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.path", "path", offsetof(oa_ui_jinja_operation, path), text};
    impl->operation_fields[2] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.operation_id", "operation_id", offsetof(oa_ui_jinja_operation, operation_id), text};
    impl->operation_fields[3] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.summary", "summary", offsetof(oa_ui_jinja_operation, summary), text};
    impl->operation_fields[4] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.description", "description", offsetof(oa_ui_jinja_operation, description), text};
    impl->operation_fields[5] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.tags", "tags", offsetof(oa_ui_jinja_operation, tags), sequence};
    impl->operation_fields[6] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.parameters", "parameters", offsetof(oa_ui_jinja_operation, parameters), sequence};
    impl->operation_fields[7] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.request_body_json", "request_body_json", offsetof(oa_ui_jinja_operation, request_body_json), text};
    impl->operation_fields[8] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.responses_json", "responses_json", offsetof(oa_ui_jinja_operation, responses_json), text};
    impl->operation_fields[9] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.deprecated", "deprecated", offsetof(oa_ui_jinja_operation, deprecated), &cmeta_data_bool};
    impl->operation_shape = (cmeta_data_struct_shape){
        &OA_OPERATION_LAYOUT, impl->operation_fields, OA_ARRAY_COUNT(impl->operation_fields)};
    impl->operation_data = (cmeta_data_desc){
        sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION,
        "openapi.ui.jinja.operation.data", "OpenAPI UI Jinja operation",
        CMETA_DATA_STRUCT, &OA_OPERATION_TYPE, &impl->operation_shape,
        NULL, NULL, NULL};

    impl->document_fields[0] = (cmeta_data_field_desc){
        "openapi.ui.jinja.document.title", "title", offsetof(oa_ui_jinja_document, title), text};
    impl->document_fields[1] = (cmeta_data_field_desc){
        "openapi.ui.jinja.document.version", "version", offsetof(oa_ui_jinja_document, version), text};
    impl->document_fields[2] = (cmeta_data_field_desc){
        "openapi.ui.jinja.document.openapi_version", "openapi_version", offsetof(oa_ui_jinja_document, openapi_version), text};
    impl->document_fields[3] = (cmeta_data_field_desc){
        "openapi.ui.jinja.document.operations", "operations", offsetof(oa_ui_jinja_document, operations), sequence};
    impl->document_shape = (cmeta_data_struct_shape){
        &OA_DOCUMENT_LAYOUT, impl->document_fields, OA_ARRAY_COUNT(impl->document_fields)};
    impl->document_data = (cmeta_data_desc){
        sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION,
        "openapi.ui.jinja.document.data", "OpenAPI UI Jinja document",
        CMETA_DATA_STRUCT, &OA_DOCUMENT_TYPE, &impl->document_shape,
        NULL, NULL, NULL};
}

static int oa_ui_renderer_adapt_document(oa_ui_renderer_impl *impl) {
    const oa_ui_document *source = impl->source;
    const oa_ui_operation *operations =
        (const oa_ui_operation *)source->operations.data;

    impl->root.title = source->title;
    impl->root.version = source->version;
    impl->root.openapi_version = source->openapi_version;
    impl->root.operations = (JINJA_CMETA_SEQUENCE_VIEW){
        NULL, source->operations.count, sizeof(oa_ui_jinja_operation),
        &impl->operation_data};

    if (source->operations.count == 0u)
        return 1;
    if (source->operations.data == NULL ||
        source->operations.stride != sizeof(oa_ui_operation))
        return 0;

    impl->operations = calloc(source->operations.count, sizeof(*impl->operations));
    if (!impl->operations) return -1;

    for (size_t i = 0u; i < source->operations.count; ++i) {
        const oa_ui_operation *in = &operations[i];
        oa_ui_jinja_operation *out = &impl->operations[i];

        out->method = in->method;
        out->path = in->path;
        out->operation_id = in->operation_id;
        out->summary = in->summary;
        out->description = in->description;
        out->request_body_json = in->request_body_json;
        out->responses_json = in->responses_json;
        out->deprecated = in->deprecated;

        if ((in->tags.count != 0u &&
             (in->tags.data == NULL || in->tags.stride != sizeof(vstr))) ||
            (in->parameters.count != 0u &&
             (in->parameters.data == NULL ||
              in->parameters.stride != sizeof(oa_ui_parameter)))) {
            return 0;
        }

        out->tags = (JINJA_CMETA_SEQUENCE_VIEW){
            in->tags.data, in->tags.count, sizeof(vstr), jinja_cmeta_vstr_data()};
        out->parameters = (JINJA_CMETA_SEQUENCE_VIEW){
            in->parameters.data, in->parameters.count, sizeof(oa_ui_parameter),
            &impl->parameter_data};
    }

    impl->root.operations.data = impl->operations;
    return 1;
}

static void oa_ui_renderer_impl_destroy(oa_ui_renderer_impl *impl) {
    if (!impl) return;
    jinja_cmeta_release(impl->templ);
    jinja_cmeta_runtime_config_destroy(impl->runtime);
    jinja_cmeta_env_destroy(impl->env);
    free(impl->operations);
    free(impl);
}

oa_ui_renderer_status oa_ui_renderer_init(
    oa_ui_renderer *renderer,
    const oa_ui_document *document,
    vstr template_name,
    vstr source,
    const oa_ui_renderer_config *config,
    oa_ui_renderer_error *error) {
    JINJA_CMETA_ERROR jerror = JINJA_CMETA_ERROR_INIT;
    JINJA_CMETA_ENV_OPTIONS env_options = JINJA_CMETA_ENV_OPTIONS_INIT;
    oa_ui_renderer_impl *impl;
    int adapted;

    if (!renderer || !document || !oa_ui_renderer_config_valid(config) ||
        (template_name.len != 0u && template_name.data == NULL) ||
        (source.len != 0u && source.data == NULL))
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                   "renderer initialization arguments are invalid");
    renderer->impl = NULL;
    if (source.len > config->max_template_bytes)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_CAPACITY,
                                   "template exceeds renderer limit");

    impl = calloc(1u, sizeof(*impl));
    if (!impl)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_OUT_OF_MEMORY,
                                   "renderer is out of memory");
    impl->source = document;
    oa_ui_renderer_init_descriptors(impl);

    if (!cmeta_data_desc_valid(&impl->parameter_data) ||
        !cmeta_data_desc_valid(&impl->operation_data) ||
        !cmeta_data_desc_valid(&impl->document_data)) {
        oa_ui_renderer_impl_destroy(impl);
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_RENDER,
                                   "renderer metadata is invalid");
    }

    adapted = oa_ui_renderer_adapt_document(impl);
    if (adapted <= 0) {
        oa_ui_renderer_impl_destroy(impl);
        return oa_ui_renderer_fail(error,
                                   adapted < 0 ? OA_UI_RENDERER_OUT_OF_MEMORY
                                               : OA_UI_RENDERER_INVALID_ARGUMENT,
                                   adapted < 0 ? "renderer is out of memory"
                                               : "OpenAPI UI model is invalid");
    }

    env_options.autoescape_selector = oa_ui_html_autoescape;
    impl->env = jinja_cmeta_env_create(&env_options, &jerror);
    if (!impl->env) {
        oa_ui_renderer_status result =
            oa_ui_renderer_from_jinja(error, jerror.status, 1);
        oa_ui_renderer_impl_destroy(impl);
        return result;
    }

    impl->templ = jinja_cmeta_env_compile(
        impl->env, template_name, source, &jerror);
    if (!impl->templ) {
        oa_ui_renderer_status result =
            oa_ui_renderer_from_jinja(error, jerror.status, 1);
        oa_ui_renderer_impl_destroy(impl);
        return result;
    }

    impl->runtime = jinja_cmeta_runtime_config_create(&jerror);
    if (!impl->runtime) {
        oa_ui_renderer_status result =
            oa_ui_renderer_from_jinja(error, jerror.status, 0);
        oa_ui_renderer_impl_destroy(impl);
        return result;
    }
    if (jinja_cmeta_runtime_config_set_limit(
            impl->runtime, JINJA_CMETA_RESOURCE_CELLS,
            config->max_nodes, &jerror) != JINJA_CMETA_OK ||
        jinja_cmeta_runtime_config_set_limit(
            impl->runtime, JINJA_CMETA_RESOURCE_ACTIVATIONS,
            config->max_nodes, &jerror) != JINJA_CMETA_OK ||
        jinja_cmeta_runtime_config_set_limit(
            impl->runtime, JINJA_CMETA_RESOURCE_VALUES,
            config->max_value_visits, &jerror) != JINJA_CMETA_OK) {
        oa_ui_renderer_status result =
            oa_ui_renderer_from_jinja(error, jerror.status, 0);
        oa_ui_renderer_impl_destroy(impl);
        return result;
    }

    impl->render_options = (JINJA_CMETA_RENDER_OPTIONS)JINJA_CMETA_RENDER_OPTIONS_INIT;
    impl->render_options.max_nodes = config->max_nodes;
    impl->render_options.max_string_bytes = config->max_output_bytes;
    impl->render_options.max_render_depth = config->max_render_depth;
    impl->render_options.max_value_visits = config->max_value_visits;

    renderer->impl = impl;
    return oa_ui_renderer_fail(error, OA_UI_RENDERER_OK, NULL);
}

oa_ui_renderer_status oa_ui_renderer_render(
    oa_ui_renderer *renderer,
    char **out_html,
    size_t *out_size,
    oa_ui_renderer_error *error) {
    oa_ui_renderer_impl *impl;
    JINJA_CMETA_ERROR jerror = JINJA_CMETA_ERROR_INIT;
    JINJA_CMETA_STATUS status;
    char *html = NULL;

    if (out_html) *out_html = NULL;
    if (out_size) *out_size = 0u;
    if (!renderer || !renderer->impl || !out_html || !out_size)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                   "render arguments are invalid");

    impl = (oa_ui_renderer_impl *)renderer->impl;
    status = jinja_cmeta_render_string_ex(
        impl->templ, &impl->document_data, &impl->root,
        &impl->render_options, impl->runtime, &html, &jerror);
    if (status != JINJA_CMETA_OK) {
        free(html);
        return oa_ui_renderer_from_jinja(error, status, 0);
    }

    *out_size = strlen(html);
    *out_html = html;
    return oa_ui_renderer_fail(error, OA_UI_RENDERER_OK, NULL);
}

void oa_ui_renderer_output_free(char *html) {
    free(html);
}

void oa_ui_renderer_destroy(oa_ui_renderer *renderer) {
    if (!renderer) return;
    oa_ui_renderer_impl_destroy((oa_ui_renderer_impl *)renderer->impl);
    renderer->impl = NULL;
}
