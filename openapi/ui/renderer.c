#include "renderer.h"

#include <jinja_cmeta.h>
#include <jinja_cmeta_runtime.h>

#include <cmeta/struct.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OA_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))


_Static_assert(sizeof(oa_ui_sequence_view) == sizeof(JINJA_CMETA_SEQUENCE_VIEW),
               "OpenAPI/Jinja sequence views must remain layout-compatible");
_Static_assert(_Alignof(oa_ui_sequence_view) == _Alignof(JINJA_CMETA_SEQUENCE_VIEW),
               "OpenAPI/Jinja sequence view alignment must match");
_Static_assert(offsetof(oa_ui_sequence_view, data) ==
                   offsetof(JINJA_CMETA_SEQUENCE_VIEW, data),
               "sequence data offsets must match");
_Static_assert(offsetof(oa_ui_sequence_view, count) ==
                   offsetof(JINJA_CMETA_SEQUENCE_VIEW, count),
               "sequence count offsets must match");
_Static_assert(offsetof(oa_ui_sequence_view, stride) ==
                   offsetof(JINJA_CMETA_SEQUENCE_VIEW, stride),
               "sequence stride offsets must match");
_Static_assert(offsetof(oa_ui_sequence_view, element) ==
                   offsetof(JINJA_CMETA_SEQUENCE_VIEW, element),
               "sequence element offsets must match");

typedef struct oa_ui_renderer_template_entry {
    vstr name;
    vstr source;
    JINJA_CMETA_TEMPLATE *templ;
} oa_ui_renderer_template_entry;

typedef struct oa_ui_jinja_document {
    vstr title;
    vstr version;
    vstr openapi_version;
    vstr server_url;
    JINJA_CMETA_SEQUENCE_VIEW operations;
    JINJA_CMETA_SEQUENCE_VIEW operation_keys;
    JINJA_CMETA_SEQUENCE_VIEW selected_operations;
} oa_ui_jinja_document;

typedef struct oa_ui_renderer_impl {
    const oa_ui_document *source;
    oa_ui_jinja_document root;

    JINJA_CMETA_ENV *env;
    JINJA_CMETA_TEMPLATE *templ;
    JINJA_CMETA_RUNTIME_CONFIG *runtime;
    JINJA_CMETA_RENDER_OPTIONS render_options;

    oa_ui_renderer_template_entry *templates;
    size_t template_count;
    vstr *operation_keys;
    char *operation_key_bytes;

    cmeta_data_field_desc operation_fields[10];
    cmeta_data_struct_shape operation_shape;
    cmeta_data_desc operation_data;

    cmeta_data_field_desc document_fields[7];
    cmeta_data_struct_shape document_shape;
    cmeta_data_desc document_data;
} oa_ui_renderer_impl;

static const cmeta_type_identity OA_OPERATION_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.jinja.operation");
static const cmeta_type_identity OA_DOCUMENT_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("openapi.ui.jinja.document");

static const cmeta_type_desc OA_OPERATION_TYPE = {
    "oa_ui_operation", sizeof(oa_ui_operation), _Alignof(oa_ui_operation),
    CMETA_T_OBJECT, NULL, NULL, &OA_OPERATION_IDENTITY
};
static const cmeta_type_desc OA_DOCUMENT_TYPE = {
    "oa_ui_jinja_document", sizeof(oa_ui_jinja_document), _Alignof(oa_ui_jinja_document),
    CMETA_T_OBJECT, NULL, NULL, &OA_DOCUMENT_IDENTITY
};

#define OA_LAYOUT_FIELD(owner, member, field_type_, type_name_) \
    {#member, type_name_, offsetof(owner, member), sizeof(((owner *)0)->member), \
     _Alignof(field_type_), NULL, NULL}

static const cmeta_field_desc OA_OPERATION_LAYOUT_FIELDS[] = {
    OA_LAYOUT_FIELD(oa_ui_operation, method, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_operation, path, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_operation, operation_id, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_operation, summary, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_operation, description, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_operation, tags, oa_ui_sequence_view, "oa_ui_sequence_view"),
    OA_LAYOUT_FIELD(oa_ui_operation, parameters, oa_ui_sequence_view, "oa_ui_sequence_view"),
    OA_LAYOUT_FIELD(oa_ui_operation, request_body_json, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_operation, responses_json, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_operation, deprecated, bool, "bool")
};
static const cmeta_struct_desc OA_OPERATION_LAYOUT = {
    "oa_ui_operation", sizeof(oa_ui_operation), _Alignof(oa_ui_operation),
    OA_OPERATION_LAYOUT_FIELDS, OA_ARRAY_COUNT(OA_OPERATION_LAYOUT_FIELDS)
};

static const cmeta_field_desc OA_DOCUMENT_LAYOUT_FIELDS[] = {
    OA_LAYOUT_FIELD(oa_ui_jinja_document, title, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_document, version, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_document, openapi_version, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_document, server_url, vstr, "vstr"),
    OA_LAYOUT_FIELD(oa_ui_jinja_document, operations, JINJA_CMETA_SEQUENCE_VIEW,
                    "JINJA_CMETA_SEQUENCE_VIEW"),
    OA_LAYOUT_FIELD(oa_ui_jinja_document, operation_keys, JINJA_CMETA_SEQUENCE_VIEW,
                    "JINJA_CMETA_SEQUENCE_VIEW"),
    OA_LAYOUT_FIELD(oa_ui_jinja_document, selected_operations, JINJA_CMETA_SEQUENCE_VIEW,
                    "JINJA_CMETA_SEQUENCE_VIEW")
};
static const cmeta_struct_desc OA_DOCUMENT_LAYOUT = {
    "oa_ui_jinja_document", sizeof(oa_ui_jinja_document), _Alignof(oa_ui_jinja_document),
    OA_DOCUMENT_LAYOUT_FIELDS, OA_ARRAY_COUNT(OA_DOCUMENT_LAYOUT_FIELDS)
};

#undef OA_LAYOUT_FIELD

typedef struct oa_ui_render_buffer {
    char *data;
    size_t size;
    size_t capacity;
    size_t limit;
    oa_ui_renderer_status failure;
} oa_ui_render_buffer;

static int oa_ui_render_buffer_write(const char *text, size_t size, void *userdata) {
    oa_ui_render_buffer *buffer = (oa_ui_render_buffer *)userdata;
    size_t required;
    size_t storage;
    size_t max_capacity;
    size_t next_capacity;
    char *grown;

    if (!buffer || (size != 0u && !text)) return -1;
    if (buffer->failure != OA_UI_RENDERER_OK) return -1;
    if (buffer->size > buffer->limit || size > buffer->limit - buffer->size) {
        buffer->failure = OA_UI_RENDERER_CAPACITY;
        return -1;
    }

    required = buffer->size + size;
    if (required == SIZE_MAX) {
        buffer->failure = OA_UI_RENDERER_CAPACITY;
        return -1;
    }
    storage = required + 1u;
    if (storage > buffer->capacity) {
        max_capacity =
            buffer->limit == SIZE_MAX ? SIZE_MAX : buffer->limit + 1u;
        next_capacity = buffer->capacity ? buffer->capacity : 256u;
        if (next_capacity > max_capacity) next_capacity = max_capacity;
        while (next_capacity < storage) {
            if (next_capacity > max_capacity / 2u) {
                next_capacity = max_capacity;
                break;
            }
            next_capacity *= 2u;
        }
        if (next_capacity < storage) next_capacity = storage;
        grown = (char *)realloc(buffer->data, next_capacity);
        if (!grown) {
            buffer->failure = OA_UI_RENDERER_OUT_OF_MEMORY;
            return -1;
        }
        buffer->data = grown;
        buffer->capacity = next_capacity;
    }

    if (size != 0u)
        memcpy(buffer->data + buffer->size, text, size);
    buffer->size = required;
    buffer->data[buffer->size] = '\0';
    return 0;
}

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

    impl->operation_fields[0] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.method", "method",
        offsetof(oa_ui_operation, method), text};
    impl->operation_fields[1] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.path", "path",
        offsetof(oa_ui_operation, path), text};
    impl->operation_fields[2] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.operation_id", "operation_id",
        offsetof(oa_ui_operation, operation_id), text};
    impl->operation_fields[3] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.summary", "summary",
        offsetof(oa_ui_operation, summary), text};
    impl->operation_fields[4] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.description", "description",
        offsetof(oa_ui_operation, description), text};
    impl->operation_fields[5] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.tags", "tags",
        offsetof(oa_ui_operation, tags), sequence};
    impl->operation_fields[6] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.parameters", "parameters",
        offsetof(oa_ui_operation, parameters), sequence};
    impl->operation_fields[7] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.request_body_json", "request_body_json",
        offsetof(oa_ui_operation, request_body_json), text};
    impl->operation_fields[8] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.responses_json", "responses_json",
        offsetof(oa_ui_operation, responses_json), text};
    impl->operation_fields[9] = (cmeta_data_field_desc){
        "openapi.ui.jinja.operation.deprecated", "deprecated",
        offsetof(oa_ui_operation, deprecated), &cmeta_data_bool};
    impl->operation_shape = (cmeta_data_struct_shape){
        &OA_OPERATION_LAYOUT, impl->operation_fields,
        OA_ARRAY_COUNT(impl->operation_fields)};
    impl->operation_data = (cmeta_data_desc){
        .struct_size = sizeof(cmeta_data_desc),
        .abi_version = CMETA_DATA_DESC_ABI_VERSION,
        .stable_id = "openapi.ui.jinja.operation.data",
        .display_name = "OpenAPI UI Jinja operation",
        .kind = CMETA_DATA_STRUCT,
        .storage_type = &OA_OPERATION_TYPE,
        .shape = &impl->operation_shape
    };

    impl->document_fields[0] = (cmeta_data_field_desc){
        "openapi.ui.jinja.document.title", "title",
        offsetof(oa_ui_jinja_document, title), text};
    impl->document_fields[1] = (cmeta_data_field_desc){
        "openapi.ui.jinja.document.version", "version",
        offsetof(oa_ui_jinja_document, version), text};
    impl->document_fields[2] = (cmeta_data_field_desc){
        "openapi.ui.jinja.document.openapi_version", "openapi_version",
        offsetof(oa_ui_jinja_document, openapi_version), text};
    impl->document_fields[3] = (cmeta_data_field_desc){
        "openapi.ui.jinja.document.server_url", "server_url",
        offsetof(oa_ui_jinja_document, server_url), text};
    impl->document_fields[4] = (cmeta_data_field_desc){
        "openapi.ui.jinja.document.operations", "operations",
        offsetof(oa_ui_jinja_document, operations), sequence};
    impl->document_fields[5] = (cmeta_data_field_desc){
        "openapi.ui.jinja.document.operation_keys", "operation_keys",
        offsetof(oa_ui_jinja_document, operation_keys), sequence};
    impl->document_fields[6] = (cmeta_data_field_desc){
        "openapi.ui.jinja.document.selected_operations", "selected_operations",
        offsetof(oa_ui_jinja_document, selected_operations), sequence};
    impl->document_shape = (cmeta_data_struct_shape){
        &OA_DOCUMENT_LAYOUT, impl->document_fields,
        OA_ARRAY_COUNT(impl->document_fields)};
    impl->document_data = (cmeta_data_desc){
        .struct_size = sizeof(cmeta_data_desc),
        .abi_version = CMETA_DATA_DESC_ABI_VERSION,
        .stable_id = "openapi.ui.jinja.document.data",
        .display_name = "OpenAPI UI Jinja document",
        .kind = CMETA_DATA_STRUCT,
        .storage_type = &OA_DOCUMENT_TYPE,
        .shape = &impl->document_shape
    };
}

static int oa_ui_renderer_adapt_document(oa_ui_renderer_impl *impl) {
    const oa_ui_document *source = impl->source;

    if ((source->operations.count != 0u &&
         (source->operations.data == NULL ||
          source->operations.stride != sizeof(oa_ui_operation))))
        return 0;

    impl->root.title = source->title;
    impl->root.version = source->version;
    impl->root.openapi_version = source->openapi_version;
    impl->root.server_url = source->server_url;
    impl->root.operations = (JINJA_CMETA_SEQUENCE_VIEW){
        source->operations.data, source->operations.count,
        sizeof(oa_ui_operation), &impl->operation_data};
    impl->root.operation_keys = (JINJA_CMETA_SEQUENCE_VIEW){
        NULL, 0u, sizeof(vstr), jinja_cmeta_vstr_data()};
    impl->root.selected_operations = (JINJA_CMETA_SEQUENCE_VIEW){
        NULL, 0u, sizeof(oa_ui_operation), &impl->operation_data};
    return 1;
}

static void oa_ui_renderer_impl_destroy(oa_ui_renderer_impl *impl) {
    if (!impl) return;
    jinja_cmeta_release(impl->templ);
    if (impl->templates) {
        for (size_t i = 0u; i < impl->template_count; ++i)
            jinja_cmeta_release(impl->templates[i].templ);
    }
    jinja_cmeta_runtime_config_destroy(impl->runtime);
    jinja_cmeta_env_destroy(impl->env);
    if (impl->templates) {
        for (size_t i = 0u; i < impl->template_count; ++i) {
            free((void *)impl->templates[i].name.data);
            free((void *)impl->templates[i].source.data);
        }
    }
    free(impl->templates);
    free(impl->operation_keys);
    free(impl->operation_key_bytes);
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

    if (!cmeta_data_desc_valid(&impl->operation_data) ||
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
    const JINJA_CMETA_RENDERER output_renderer = {oa_ui_render_buffer_write};
    oa_ui_render_buffer buffer = {0};
    JINJA_CMETA_STATUS status;

    if (out_html) *out_html = NULL;
    if (out_size) *out_size = 0u;
    if (!renderer || !renderer->impl || !out_html || !out_size)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                   "render arguments are invalid");

    impl = (oa_ui_renderer_impl *)renderer->impl;
    buffer.limit = impl->render_options.max_string_bytes;
    status = jinja_cmeta_render_ex(
        impl->templ, &impl->document_data, &impl->root,
        &impl->render_options, impl->runtime,
        &output_renderer, &buffer, &jerror);
    if (status != JINJA_CMETA_OK || buffer.failure != OA_UI_RENDERER_OK) {
        oa_ui_renderer_status result;
        if (buffer.failure == OA_UI_RENDERER_CAPACITY) {
            result = oa_ui_renderer_fail(
                error, OA_UI_RENDERER_CAPACITY, "render exceeds renderer limits");
        } else if (buffer.failure == OA_UI_RENDERER_OUT_OF_MEMORY) {
            result = oa_ui_renderer_fail(
                error, OA_UI_RENDERER_OUT_OF_MEMORY, "renderer is out of memory");
        } else {
            result = oa_ui_renderer_from_jinja(error, status, 0);
        }
        free(buffer.data);
        return result;
    }

    if (!buffer.data) {
        buffer.data = (char *)malloc(1u);
        if (!buffer.data)
            return oa_ui_renderer_fail(error, OA_UI_RENDERER_OUT_OF_MEMORY,
                                       "renderer is out of memory");
        buffer.data[0] = '\0';
    }

    *out_html = buffer.data;
    *out_size = buffer.size;
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


enum {
    OA_UI_RENDERER_MAX_OPERATIONS = 4096,
    OA_UI_RENDERER_MAX_OPERATION_KEY_BYTES = 128
};

static int oa_ui_renderer_copy_view(vstr source, vstr *out) {
    char *copy;
    if (!out || !vstr_is_valid(source) || source.len == SIZE_MAX) return 0;
    copy = (char *)malloc(source.len + 1u);
    if (!copy) return 0;
    if (source.len != 0u) memcpy(copy, source.data, source.len);
    copy[source.len] = '\0';
    *out = vstr_from_buf(copy, source.len);
    return 1;
}

static int oa_ui_renderer_key_safe(vstr key) {
    if (!vstr_is_valid(key) || key.len == 0u ||
        key.len > OA_UI_RENDERER_MAX_OPERATION_KEY_BYTES)
        return 0;
    for (size_t i = 0u; i < key.len; ++i) {
        const unsigned char ch = (unsigned char)key.data[i];
        if ((ch >= (unsigned char)'A' && ch <= (unsigned char)'Z') ||
            (ch >= (unsigned char)'a' && ch <= (unsigned char)'z') ||
            (ch >= (unsigned char)'0' && ch <= (unsigned char)'9') ||
            ch == (unsigned char)'-' || ch == (unsigned char)'.' ||
            ch == (unsigned char)'_' || ch == (unsigned char)'~')
            continue;
        return 0;
    }
    return 1;
}

static oa_ui_renderer_status oa_ui_renderer_build_operation_keys(
    oa_ui_renderer_impl *impl, oa_ui_renderer_error *error) {
    const oa_ui_operation *operations;
    const size_t count = impl->source->operations.count;
    size_t total = 0u;
    char fallback[32];

    if (count > OA_UI_RENDERER_MAX_OPERATIONS)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_CAPACITY,
                                   "operation count exceeds renderer limit");
    if (count == 0u) {
        impl->root.operation_keys = (JINJA_CMETA_SEQUENCE_VIEW){
            NULL, 0u, sizeof(vstr), jinja_cmeta_vstr_data()};
        return OA_UI_RENDERER_OK;
    }

    operations = (const oa_ui_operation *)impl->source->operations.data;
    for (size_t i = 0u; i < count; ++i) {
        size_t length;
        if (oa_ui_renderer_key_safe(operations[i].operation_id)) {
            length = operations[i].operation_id.len;
        } else {
            const int written = snprintf(fallback, sizeof(fallback), "op-%zu", i);
            if (written < 0 || (size_t)written >= sizeof(fallback))
                return oa_ui_renderer_fail(error, OA_UI_RENDERER_CAPACITY,
                                           "operation route key is too large");
            length = (size_t)written;
        }
        if (total > SIZE_MAX - length - 1u)
            return oa_ui_renderer_fail(error, OA_UI_RENDERER_CAPACITY,
                                       "operation route keys exceed capacity");
        total += length + 1u;
    }

    impl->operation_keys = (vstr *)calloc(count, sizeof(*impl->operation_keys));
    impl->operation_key_bytes = (char *)malloc(total);
    if (!impl->operation_keys || !impl->operation_key_bytes)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_OUT_OF_MEMORY,
                                   "unable to allocate operation route keys");

    char *cursor = impl->operation_key_bytes;
    for (size_t i = 0u; i < count; ++i) {
        const vstr id = operations[i].operation_id;
        size_t length;
        if (oa_ui_renderer_key_safe(id)) {
            length = id.len;
            memcpy(cursor, id.data, length);
        } else {
            const int written = snprintf(fallback, sizeof(fallback), "op-%zu", i);
            if (written < 0 || (size_t)written >= sizeof(fallback))
                return oa_ui_renderer_fail(error, OA_UI_RENDERER_CAPACITY,
                                           "operation route key is too large");
            length = (size_t)written;
            memcpy(cursor, fallback, length);
        }
        cursor[length] = '\0';
        impl->operation_keys[i] = vstr_from_buf(cursor, length);
        for (size_t j = 0u; j < i; ++j) {
            if (vstr_eq(impl->operation_keys[j], impl->operation_keys[i]))
                return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                           "operation route keys must be unique");
        }
        cursor += length + 1u;
    }

    impl->root.operation_keys = (JINJA_CMETA_SEQUENCE_VIEW){
        impl->operation_keys, count, sizeof(vstr), jinja_cmeta_vstr_data()};
    return OA_UI_RENDERER_OK;
}

static JINJA_CMETA_STATUS oa_ui_renderer_bundle_load(
    void *userdata, vstr name, JINJA_CMETA_SOURCE *source,
    JINJA_CMETA_ERROR *error) {
    oa_ui_renderer_impl *impl = (oa_ui_renderer_impl *)userdata;
    (void)error;
    if (!impl || !source || !vstr_is_valid(name))
        return JINJA_CMETA_ERR_INVALID_ARGUMENT;
    for (size_t i = 0u; i < impl->template_count; ++i) {
        if (!vstr_eq(impl->templates[i].name, name)) continue;
        *source = (JINJA_CMETA_SOURCE){
            .text = impl->templates[i].source,
            .lease = &impl->templates[i]
        };
        return JINJA_CMETA_OK;
    }
    return JINJA_CMETA_ERR_NOT_FOUND;
}

static void oa_ui_renderer_bundle_release(
    void *userdata, JINJA_CMETA_SOURCE *source) {
    (void)userdata;
    if (source) *source = (JINJA_CMETA_SOURCE){0};
}

static oa_ui_renderer_status oa_ui_renderer_copy_bundle(
    oa_ui_renderer_impl *impl,
    const oa_ui_renderer_template *templates,
    size_t template_count,
    const oa_ui_renderer_config *config,
    size_t *out_source_bytes,
    oa_ui_renderer_error *error) {
    size_t source_bytes = 0u;
    if (!impl || !templates || !out_source_bytes ||
        template_count == 0u ||
        template_count > JINJA_CMETA_MAX_CACHED_TEMPLATES)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                   "template bundle is invalid");

    impl->templates = (oa_ui_renderer_template_entry *)calloc(
        template_count, sizeof(*impl->templates));
    if (!impl->templates)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_OUT_OF_MEMORY,
                                   "unable to allocate template bundle");
    impl->template_count = template_count;

    for (size_t i = 0u; i < template_count; ++i) {
        if (!vstr_is_valid(templates[i].name) ||
            !vstr_is_valid(templates[i].source) ||
            templates[i].name.len == 0u ||
            templates[i].name.len > JINJA_CMETA_MAX_TEMPLATE_BYTES)
            return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                       "template bundle entry is invalid");
        for (size_t j = 0u; j < i; ++j) {
            if (vstr_eq(templates[j].name, templates[i].name))
                return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                           "template names must be unique");
        }
        if (templates[i].source.len > config->max_template_bytes ||
            source_bytes > config->max_template_bytes - templates[i].source.len)
            return oa_ui_renderer_fail(error, OA_UI_RENDERER_CAPACITY,
                                       "template bundle exceeds renderer limit");
        source_bytes += templates[i].source.len;

        if (!oa_ui_renderer_copy_view(
                templates[i].name, &impl->templates[i].name) ||
            !oa_ui_renderer_copy_view(
                templates[i].source, &impl->templates[i].source))
            return oa_ui_renderer_fail(error, OA_UI_RENDERER_OUT_OF_MEMORY,
                                       "unable to copy template bundle");
    }
    *out_source_bytes = source_bytes;
    return OA_UI_RENDERER_OK;
}

static oa_ui_renderer_status oa_ui_renderer_init_runtime(
    oa_ui_renderer_impl *impl,
    const oa_ui_renderer_config *config,
    oa_ui_renderer_error *error) {
    JINJA_CMETA_ERROR jerror = JINJA_CMETA_ERROR_INIT;
    impl->runtime = jinja_cmeta_runtime_config_create(&jerror);
    if (!impl->runtime)
        return oa_ui_renderer_from_jinja(error, jerror.status, 0);

    if (jinja_cmeta_runtime_config_set_limit(
            impl->runtime, JINJA_CMETA_RESOURCE_CELLS,
            config->max_nodes, &jerror) != JINJA_CMETA_OK ||
        jinja_cmeta_runtime_config_set_limit(
            impl->runtime, JINJA_CMETA_RESOURCE_ACTIVATIONS,
            config->max_nodes, &jerror) != JINJA_CMETA_OK ||
        jinja_cmeta_runtime_config_set_limit(
            impl->runtime, JINJA_CMETA_RESOURCE_VALUES,
            config->max_value_visits, &jerror) != JINJA_CMETA_OK)
        return oa_ui_renderer_from_jinja(error, jerror.status, 0);

    impl->render_options =
        (JINJA_CMETA_RENDER_OPTIONS)JINJA_CMETA_RENDER_OPTIONS_INIT;
    impl->render_options.max_nodes = config->max_nodes;
    impl->render_options.max_string_bytes = config->max_output_bytes;
    impl->render_options.max_render_depth = config->max_render_depth;
    impl->render_options.max_value_visits = config->max_value_visits;
    return OA_UI_RENDERER_OK;
}

static oa_ui_renderer_status oa_ui_renderer_render_compiled(
    oa_ui_renderer_impl *impl,
    const JINJA_CMETA_TEMPLATE *templ,
    size_t selected_operation,
    char **out_html,
    size_t *out_size,
    oa_ui_renderer_error *error) {
    JINJA_CMETA_ERROR jerror = JINJA_CMETA_ERROR_INIT;
    const JINJA_CMETA_RENDERER output_renderer = {oa_ui_render_buffer_write};
    oa_ui_render_buffer buffer = {0};
    oa_ui_jinja_document root;
    JINJA_CMETA_STATUS status;

    if (out_html) *out_html = NULL;
    if (out_size) *out_size = 0u;
    if (!impl || !templ || !out_html || !out_size)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                   "render arguments are invalid");
    if (selected_operation != OA_UI_RENDERER_NO_SELECTION &&
        selected_operation >= impl->source->operations.count)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                   "selected operation is out of range");

    root = impl->root;
    if (selected_operation != OA_UI_RENDERER_NO_SELECTION) {
        const unsigned char *operations =
            (const unsigned char *)impl->source->operations.data;
        root.selected_operations = (JINJA_CMETA_SEQUENCE_VIEW){
            operations + selected_operation * sizeof(oa_ui_operation),
            1u, sizeof(oa_ui_operation), &impl->operation_data
        };
    }

    buffer.limit = impl->render_options.max_string_bytes;
    status = jinja_cmeta_render_ex(
        templ, &impl->document_data, &root,
        &impl->render_options, impl->runtime,
        &output_renderer, &buffer, &jerror);
    if (status != JINJA_CMETA_OK || buffer.failure != OA_UI_RENDERER_OK) {
        oa_ui_renderer_status result;
        if (buffer.failure == OA_UI_RENDERER_CAPACITY) {
            result = oa_ui_renderer_fail(
                error, OA_UI_RENDERER_CAPACITY,
                "render exceeds renderer limits");
        } else if (buffer.failure == OA_UI_RENDERER_OUT_OF_MEMORY) {
            result = oa_ui_renderer_fail(
                error, OA_UI_RENDERER_OUT_OF_MEMORY,
                "renderer is out of memory");
        } else {
            result = oa_ui_renderer_from_jinja(error, status, 0);
        }
        free(buffer.data);
        return result;
    }

    if (!buffer.data) {
        buffer.data = (char *)malloc(1u);
        if (!buffer.data)
            return oa_ui_renderer_fail(error, OA_UI_RENDERER_OUT_OF_MEMORY,
                                       "renderer is out of memory");
        buffer.data[0] = '\0';
    }
    *out_html = buffer.data;
    *out_size = buffer.size;
    return oa_ui_renderer_fail(error, OA_UI_RENDERER_OK, NULL);
}

static oa_ui_renderer_template_entry *oa_ui_renderer_template_find(
    oa_ui_renderer_impl *impl, vstr name) {
    if (!impl || !vstr_is_valid(name)) return NULL;
    for (size_t i = 0u; i < impl->template_count; ++i)
        if (vstr_eq(impl->templates[i].name, name))
            return &impl->templates[i];
    return NULL;
}

oa_ui_renderer_status oa_ui_renderer_init_bundle(
    oa_ui_renderer *renderer,
    const oa_ui_document *document,
    const oa_ui_renderer_template *templates,
    size_t template_count,
    const oa_ui_renderer_config *config,
    oa_ui_renderer_error *error) {
    JINJA_CMETA_ERROR jerror = JINJA_CMETA_ERROR_INIT;
    JINJA_CMETA_ENV_OPTIONS env_options = JINJA_CMETA_ENV_OPTIONS_INIT;
    oa_ui_renderer_impl *impl;
    oa_ui_renderer_status result;
    size_t source_bytes = 0u;

    if (!renderer || !document || !oa_ui_renderer_config_valid(config) ||
        !templates || template_count == 0u)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                   "renderer bundle arguments are invalid");
    renderer->impl = NULL;

    impl = (oa_ui_renderer_impl *)calloc(1u, sizeof(*impl));
    if (!impl)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_OUT_OF_MEMORY,
                                   "renderer is out of memory");
    impl->source = document;
    oa_ui_renderer_init_descriptors(impl);
    if (!cmeta_data_desc_valid(&impl->operation_data) ||
        !cmeta_data_desc_valid(&impl->document_data)) {
        result = oa_ui_renderer_fail(error, OA_UI_RENDERER_RENDER,
                                     "renderer metadata is invalid");
        goto fail;
    }
    if (!oa_ui_renderer_adapt_document(impl)) {
        result = oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                     "OpenAPI UI model is invalid");
        goto fail;
    }

    result = oa_ui_renderer_build_operation_keys(impl, error);
    if (result != OA_UI_RENDERER_OK) goto fail;
    result = oa_ui_renderer_copy_bundle(
        impl, templates, template_count, config, &source_bytes, error);
    if (result != OA_UI_RENDERER_OK) goto fail;

    env_options.loader = (JINJA_CMETA_LOADER){
        oa_ui_renderer_bundle_load, oa_ui_renderer_bundle_release, impl};
    env_options.max_loaded_templates = template_count;
    env_options.max_loaded_source_bytes = source_bytes ? source_bytes : 1u;
    env_options.autoescape_selector = oa_ui_html_autoescape;
    impl->env = jinja_cmeta_env_create(&env_options, &jerror);
    if (!impl->env) {
        result = oa_ui_renderer_from_jinja(error, jerror.status, 1);
        goto fail;
    }

    for (size_t i = 0u; i < template_count; ++i) {
        impl->templates[i].templ = jinja_cmeta_env_load(
            impl->env, impl->templates[i].name, &jerror);
        if (!impl->templates[i].templ) {
            result = oa_ui_renderer_from_jinja(error, jerror.status, 1);
            goto fail;
        }
    }

    result = oa_ui_renderer_init_runtime(impl, config, error);
    if (result != OA_UI_RENDERER_OK) goto fail;

    /* Probe every compiled entry so include/extends dependencies are resolved
     * during startup rather than by the first request. */
    for (size_t i = 0u; i < template_count; ++i) {
        char *probe = NULL;
        size_t probe_size = 0u;
        const size_t selected =
            document->operations.count ? 0u : OA_UI_RENDERER_NO_SELECTION;
        result = oa_ui_renderer_render_compiled(
            impl, impl->templates[i].templ, selected,
            &probe, &probe_size, error);
        free(probe);
        if (result != OA_UI_RENDERER_OK) {
            if (result != OA_UI_RENDERER_CAPACITY &&
                result != OA_UI_RENDERER_OUT_OF_MEMORY)
                result = oa_ui_renderer_fail(
                    error, OA_UI_RENDERER_TEMPLATE,
                    "template dependency validation failed");
            goto fail;
        }
    }

    renderer->impl = impl;
    return oa_ui_renderer_fail(error, OA_UI_RENDERER_OK, NULL);

fail:
    oa_ui_renderer_impl_destroy(impl);
    return result;
}

oa_ui_renderer_status oa_ui_renderer_render_named(
    oa_ui_renderer *renderer,
    vstr template_name,
    size_t selected_operation,
    char **out_html,
    size_t *out_size,
    oa_ui_renderer_error *error) {
    oa_ui_renderer_impl *impl;
    oa_ui_renderer_template_entry *entry;
    if (out_html) *out_html = NULL;
    if (out_size) *out_size = 0u;
    if (!renderer || !renderer->impl || !out_html || !out_size ||
        !vstr_is_valid(template_name))
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                   "named render arguments are invalid");
    impl = (oa_ui_renderer_impl *)renderer->impl;
    if (!impl->templates)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_UNSUPPORTED,
                                   "renderer has no template bundle");
    entry = oa_ui_renderer_template_find(impl, template_name);
    if (!entry)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_NOT_FOUND,
                                   "template name was not found");
    return oa_ui_renderer_render_compiled(
        impl, entry->templ, selected_operation, out_html, out_size, error);
}

size_t oa_ui_renderer_operation_count(const oa_ui_renderer *renderer) {
    const oa_ui_renderer_impl *impl =
        renderer ? (const oa_ui_renderer_impl *)renderer->impl : NULL;
    return impl && impl->source ? impl->source->operations.count : 0u;
}

vstr oa_ui_renderer_operation_key(const oa_ui_renderer *renderer, size_t index) {
    const oa_ui_renderer_impl *impl =
        renderer ? (const oa_ui_renderer_impl *)renderer->impl : NULL;
    if (!impl || !impl->operation_keys ||
        index >= impl->source->operations.count)
        return (vstr){NULL, 0u};
    return impl->operation_keys[index];
}

oa_ui_renderer_status oa_ui_renderer_find_operation(
    const oa_ui_renderer *renderer, vstr key, size_t *out_index) {
    const oa_ui_renderer_impl *impl =
        renderer ? (const oa_ui_renderer_impl *)renderer->impl : NULL;
    if (out_index) *out_index = OA_UI_RENDERER_NO_SELECTION;
    if (!impl || !out_index || !vstr_is_valid(key))
        return OA_UI_RENDERER_INVALID_ARGUMENT;
    if (!impl->operation_keys) return OA_UI_RENDERER_UNSUPPORTED;
    for (size_t i = 0u; i < impl->source->operations.count; ++i) {
        if (!vstr_eq(impl->operation_keys[i], key)) continue;
        *out_index = i;
        return OA_UI_RENDERER_OK;
    }
    return OA_UI_RENDERER_NOT_FOUND;
}


enum { OA_UI_RENDERER_MAX_SEARCH_BYTES = 256 };

static unsigned char oa_ui_renderer_ascii_fold(unsigned char value) {
    return value >= (unsigned char)'A' && value <= (unsigned char)'Z'
        ? (unsigned char)(value + ((unsigned char)'a' - (unsigned char)'A'))
        : value;
}

static int oa_ui_renderer_contains(vstr value, vstr query) {
    if (query.len == 0u) return 1;
    if (!vstr_is_valid(value) || query.len > value.len) return 0;
    for (size_t offset = 0u; offset <= value.len - query.len; ++offset) {
        size_t index = 0u;
        while (index < query.len &&
               oa_ui_renderer_ascii_fold((unsigned char)value.data[offset + index]) ==
                   oa_ui_renderer_ascii_fold((unsigned char)query.data[index]))
            ++index;
        if (index == query.len) return 1;
    }
    return 0;
}

static int oa_ui_renderer_operation_matches(
    const oa_ui_operation *operation, vstr query) {
    if (!operation) return 0;
    if (oa_ui_renderer_contains(operation->method, query) ||
        oa_ui_renderer_contains(operation->path, query) ||
        oa_ui_renderer_contains(operation->summary, query))
        return 1;

    if (operation->tags.count != 0u &&
        (operation->tags.data == NULL || operation->tags.stride != sizeof(vstr)))
        return 0;
    const vstr *tags = (const vstr *)operation->tags.data;
    for (size_t i = 0u; i < operation->tags.count; ++i)
        if (oa_ui_renderer_contains(tags[i], query)) return 1;
    return 0;
}

oa_ui_renderer_status oa_ui_renderer_render_operation_list(
    oa_ui_renderer *renderer,
    vstr query,
    char **out_html,
    size_t *out_size,
    oa_ui_renderer_error *error) {
    oa_ui_renderer_impl *impl;
    oa_ui_renderer_template_entry *entry;
    const oa_ui_operation *source;
    oa_ui_operation *operations = NULL;
    vstr *keys = NULL;
    size_t matches = 0u;
    oa_ui_renderer_status result;

    if (out_html) *out_html = NULL;
    if (out_size) *out_size = 0u;
    if (!renderer || !renderer->impl || !out_html || !out_size ||
        !vstr_is_valid(query))
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                   "operation-list render arguments are invalid");
    if (query.len > OA_UI_RENDERER_MAX_SEARCH_BYTES)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_CAPACITY,
                                   "operation search query exceeds renderer limit");
    if (vstr_utf8_invalid_offset(query) != VSTR_NPOS)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                   "operation search query is not valid UTF-8");

    impl = (oa_ui_renderer_impl *)renderer->impl;
    if (!impl->templates)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_UNSUPPORTED,
                                   "renderer has no template bundle");
    entry = oa_ui_renderer_template_find(
        impl, vstr_from_cstr("operation_list.html"));
    if (!entry)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_NOT_FOUND,
                                   "operation-list template was not found");

    if (query.len == 0u)
        return oa_ui_renderer_render_compiled(
            impl, entry->templ, OA_UI_RENDERER_NO_SELECTION,
            out_html, out_size, error);

    source = (const oa_ui_operation *)impl->source->operations.data;
    for (size_t i = 0u; i < impl->source->operations.count; ++i)
        if (oa_ui_renderer_operation_matches(&source[i], query)) ++matches;

    if (matches != 0u) {
        operations = (oa_ui_operation *)calloc(matches, sizeof(*operations));
        keys = (vstr *)calloc(matches, sizeof(*keys));
        if (!operations || !keys) {
            free(operations);
            free(keys);
            return oa_ui_renderer_fail(error, OA_UI_RENDERER_OUT_OF_MEMORY,
                                       "unable to allocate filtered operation view");
        }

        size_t output = 0u;
        for (size_t i = 0u; i < impl->source->operations.count; ++i) {
            if (!oa_ui_renderer_operation_matches(&source[i], query)) continue;
            operations[output] = source[i];
            keys[output] = impl->operation_keys[i];
            ++output;
        }
    }

    const oa_ui_jinja_document saved = impl->root;
    impl->root.operations = (JINJA_CMETA_SEQUENCE_VIEW){
        operations, matches, sizeof(oa_ui_operation), &impl->operation_data};
    impl->root.operation_keys = (JINJA_CMETA_SEQUENCE_VIEW){
        keys, matches, sizeof(vstr), jinja_cmeta_vstr_data()};
    impl->root.selected_operations = (JINJA_CMETA_SEQUENCE_VIEW){
        NULL, 0u, sizeof(oa_ui_operation), &impl->operation_data};

    result = oa_ui_renderer_render_compiled(
        impl, entry->templ, OA_UI_RENDERER_NO_SELECTION,
        out_html, out_size, error);
    impl->root = saved;
    free(operations);
    free(keys);
    return result;
}
