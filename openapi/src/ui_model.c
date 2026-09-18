#include <openapi/ui_model.h>
#include "internal.h"

struct oa_ui_model {
    oa_ui_document document;
};

oa_ui_model *oa_ui_model_create(const oa_document *document, oa_error *error) {
    (void)document;
    oa_fail(error, "OpenAPI UI model is not implemented");
    return NULL;
}

void oa_ui_model_free(oa_ui_model *model) {
    free(model);
}

const oa_ui_document *oa_ui_model_document(const oa_ui_model *model) {
    return model ? &model->document : NULL;
}

const cmeta_data_desc *oa_ui_parameter_cmeta_data(void) { return NULL; }
const cmeta_data_desc *oa_ui_operation_cmeta_data(void) { return NULL; }
const cmeta_data_desc *oa_ui_document_cmeta_data(void) { return NULL; }
