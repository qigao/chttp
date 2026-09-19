#ifndef OPENAPI_UI_MODEL_INTERNAL_H
#define OPENAPI_UI_MODEL_INTERNAL_H

#include <openapi/ui_model.h>

typedef struct oa_ui_allocator {
    void *(*calloc_fn)(void *userdata, size_t count, size_t size);
    void (*free_fn)(void *userdata, void *memory);
    void *userdata;
} oa_ui_allocator;

/* Testable/internal construction path. The allocator is copied into a
 * successful model and therefore only its userdata lifetime must extend until
 * oa_ui_model_free(). JSON snapshot and serializer storage remain owned by the
 * JSON subsystem; this allocator covers UI-model record/sequence storage. */
oa_ui_model *oa_ui_model_create_json_with_allocator(
    const json_value_t *root, const oa_ui_allocator *allocator, oa_error *error);

#endif
