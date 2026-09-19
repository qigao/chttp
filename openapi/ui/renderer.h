#ifndef OPENAPI_UI_RENDERER_H
#define OPENAPI_UI_RENDERER_H

#include <openapi/ui_model.h>
#include <vstr.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum oa_ui_renderer_status {
    OA_UI_RENDERER_OK = 0,
    OA_UI_RENDERER_INVALID_ARGUMENT = -1,
    OA_UI_RENDERER_TEMPLATE = -2,
    OA_UI_RENDERER_CAPACITY = -3,
    OA_UI_RENDERER_OUT_OF_MEMORY = -4,
    OA_UI_RENDERER_RENDER = -5,
    OA_UI_RENDERER_UNSUPPORTED = -6
} oa_ui_renderer_status;

typedef struct oa_ui_renderer_config {
    size_t max_template_bytes;
    size_t max_output_bytes;
    size_t max_nodes;
    size_t max_value_visits;
    unsigned max_render_depth;
} oa_ui_renderer_config;

#define OA_UI_RENDERER_CONFIG_INIT \
    {256u * 1024u, 2u * 1024u * 1024u, 65536u, 1024u * 1024u, 64u}

typedef struct oa_ui_renderer_error {
    oa_ui_renderer_status status;
    char message[192];
} oa_ui_renderer_error;

#define OA_UI_RENDERER_ERROR_INIT {OA_UI_RENDERER_OK, {0}}

typedef struct oa_ui_renderer {
    void *impl;
} oa_ui_renderer;

/* Compiles one named HTML template and borrows document until destroy.
 * Source/name bytes are copied by Jinja during compilation. */
oa_ui_renderer_status oa_ui_renderer_init(
    oa_ui_renderer *renderer,
    const oa_ui_document *document,
    vstr template_name,
    vstr source,
    const oa_ui_renderer_config *config,
    oa_ui_renderer_error *error);

/* Synchronous, non-reentrant render. One renderer belongs to one execution
 * context and must not be used concurrently. Current CHTTP route handlers run
 * serially on the server owner thread; deferred/external-thread use requires a
 * separate synchronization design.
 *
 * Fully buffered render: failure publishes no output. */
oa_ui_renderer_status oa_ui_renderer_render(
    oa_ui_renderer *renderer,
    char **out_html,
    size_t *out_size,
    oa_ui_renderer_error *error);

void oa_ui_renderer_output_free(char *html);
void oa_ui_renderer_destroy(oa_ui_renderer *renderer);

#ifdef __cplusplus
}
#endif
#endif
