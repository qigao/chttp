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

typedef struct json_value_s json_value_t;

/*
 * Renderer-neutral immutable sequence view.
 *
 * The model owns the backing storage. A non-empty view has non-NULL data,
 * nonzero stride, and a valid element descriptor. All views and elements
 * remain stable until oa_ui_model_free().
 */
typedef struct oa_ui_sequence_view {
    const void *data;
    size_t count;
    size_t stride;
    const cmeta_data_desc *element;
} oa_ui_sequence_view;

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
    oa_ui_sequence_view tags;
    oa_ui_sequence_view parameters;
    vstr request_body_json;
    vstr responses_json;
    bool deprecated;
} oa_ui_operation;

typedef struct oa_ui_document {
    vstr title;
    vstr version;
    vstr openapi_version;
    oa_ui_sequence_view operations;
} oa_ui_document;

typedef struct oa_ui_model oa_ui_model;

/*
 * Create an immutable presentation snapshot.
 *
 * The returned model owns all storage needed by its views. The source document
 * or JSON DOM may be changed or destroyed after this call returns successfully.
 */
oa_ui_model *oa_ui_model_create(const oa_document *document, oa_error *error);
oa_ui_model *oa_ui_model_create_json(const json_value_t *root, oa_error *error);
const oa_ui_document *oa_ui_model_view(const oa_ui_model *model);
void oa_ui_model_free(oa_ui_model *model);

const cmeta_data_desc *oa_ui_parameter_cmeta_data(void);
const cmeta_data_desc *oa_ui_operation_cmeta_data(void);
const cmeta_data_desc *oa_ui_document_cmeta_data(void);

#ifdef __cplusplus
}
#endif
#endif
