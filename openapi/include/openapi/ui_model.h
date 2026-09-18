#ifndef OPENAPI_UI_MODEL_H
#define OPENAPI_UI_MODEL_H

#include <openapi/generator.h>
#include <cmeta/data.h>
#include <vstr.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oa_ui_parameter {
    vstr name;
    vstr location;
    vstr description;
    vstr schema_json;
    bool required;
} oa_ui_parameter;

typedef struct oa_ui_operation {
    vstr method;
    vstr path;
    vstr operation_id;
    vstr summary;
    vstr description;
    bool deprecated;
    const oa_ui_parameter *parameters;
    size_t parameter_count;
    vstr request_body_json;
    vstr responses_json;
} oa_ui_operation;

typedef struct oa_ui_document {
    vstr title;
    vstr version;
    vstr openapi_version;
    const oa_ui_operation *operations;
    size_t operation_count;
} oa_ui_document;

typedef struct oa_ui_model oa_ui_model;

/* Creates a deep-owned immutable presentation model. The returned model does
 * not borrow from document and remains valid after oa_document_free(document).
 * Optional text is represented by an empty vstr. */
oa_ui_model *oa_ui_model_create(const oa_document *document, oa_error *error);
void oa_ui_model_free(oa_ui_model *model);
const oa_ui_document *oa_ui_model_document(const oa_ui_model *model);

/* Canonical scalar/record descriptors. Sequence storage remains an explicit
 * typed pointer/count boundary; a renderer may adapt it without inspecting the
 * generator's json_value_t representation. */
const cmeta_data_desc *oa_ui_parameter_cmeta_data(void);
const cmeta_data_desc *oa_ui_operation_cmeta_data(void);
const cmeta_data_desc *oa_ui_document_cmeta_data(void);

#ifdef __cplusplus
}
#endif
#endif
