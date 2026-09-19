#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

oa_ui_renderer_status oa_ui_renderer_init(
    oa_ui_renderer *renderer,
    const oa_ui_document *document,
    vstr template_name,
    vstr source,
    const oa_ui_renderer_config *config,
    oa_ui_renderer_error *error) {
    (void)document;
    (void)template_name;
    (void)source;
    (void)config;
    if (!renderer) return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                               "renderer is required");
    renderer->impl = NULL;
    return oa_ui_renderer_fail(error, OA_UI_RENDERER_UNSUPPORTED,
                               "Jinja OpenAPI UI renderer is not implemented");
}

oa_ui_renderer_status oa_ui_renderer_render(
    oa_ui_renderer *renderer,
    char **out_html,
    size_t *out_size,
    oa_ui_renderer_error *error) {
    if (out_html) *out_html = NULL;
    if (out_size) *out_size = 0u;
    if (!renderer || !out_html || !out_size)
        return oa_ui_renderer_fail(error, OA_UI_RENDERER_INVALID_ARGUMENT,
                                   "render arguments are invalid");
    return oa_ui_renderer_fail(error, OA_UI_RENDERER_UNSUPPORTED,
                               "Jinja OpenAPI UI renderer is not implemented");
}

void oa_ui_renderer_output_free(char *html) {
    free(html);
}

void oa_ui_renderer_destroy(oa_ui_renderer *renderer) {
    if (renderer) renderer->impl = NULL;
}
