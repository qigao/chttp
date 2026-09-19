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

typedef struct oa_ui_renderer_template {
    vstr name;
    vstr source;
} oa_ui_renderer_template;

#define OA_UI_RENDERER_NO_SELECTION ((size_t)-1)

/* Compile a fixed bundle through one bounded Jinja environment. Every named
 * template and dependency must compile before this function succeeds. The
 * source/name views are borrowed only until return. */
oa_ui_renderer_status oa_ui_renderer_init_bundle(
    oa_ui_renderer *renderer,
    const oa_ui_document *document,
    const oa_ui_renderer_template *templates,
    size_t template_count,
    const oa_ui_renderer_config *config,
    oa_ui_renderer_error *error);

/* Render one already-compiled named template. selected_operation is either a
 * document operation index or OA_UI_RENDERER_NO_SELECTION. */
oa_ui_renderer_status oa_ui_renderer_render_named(
    oa_ui_renderer *renderer,
    vstr template_name,
    size_t selected_operation,
    char **out_html,
    size_t *out_size,
    oa_ui_renderer_error *error);

/* Startup-frozen operation route keys used by Jinja templates and HTTP lookup. */
size_t oa_ui_renderer_operation_count(const oa_ui_renderer *renderer);
vstr oa_ui_renderer_operation_key(const oa_ui_renderer *renderer, size_t index);
oa_ui_renderer_status oa_ui_renderer_find_operation(
    const oa_ui_renderer *renderer, vstr key, size_t *out_index);

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
