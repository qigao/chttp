#ifndef CHTTP_WEB_WEB_H
#define CHTTP_WEB_WEB_H

#include <cmeta/data.h>
#include <http_server/http.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum chttp_web_status {
  CHTTP_WEB_OK = 0,
  CHTTP_WEB_INVALID_ARGUMENT = -1,
  CHTTP_WEB_TEMPLATE = -2,
  CHTTP_WEB_CAPACITY = -3,
  CHTTP_WEB_OUT_OF_MEMORY = -4,
  CHTTP_WEB_METADATA = -5,
  CHTTP_WEB_RENDER = -6,
  CHTTP_WEB_NOT_FOUND = -7,
  CHTTP_WEB_SERVER = -8
} chttp_web_status;

typedef struct chttp_web_renderer {
  void *impl;
} chttp_web_renderer;

typedef struct chttp_web_template {
  const char *name;
  const char *source;
  size_t source_size;
} chttp_web_template;

typedef struct chttp_web_renderer_config {
  size_t max_template_bytes;
  size_t max_output_bytes;
  size_t max_nodes;
  size_t max_value_visits;
  unsigned max_render_depth;
} chttp_web_renderer_config;

#define CHTTP_WEB_RENDERER_CONFIG_INIT \
  {256u * 1024u, 2u * 1024u * 1024u, 65536u, 1024u * 1024u, 64u}

typedef struct chttp_web_error {
  chttp_web_status status;
  int native_status;
  size_t offset;
  char template_name[256];
  char message[192];
} chttp_web_error;

#define CHTTP_WEB_ERROR_INIT {CHTTP_WEB_OK, 0, 0u, {0}, {0}}

/**
 * Builds one synchronous, non-reentrant renderer from an application-owned
 * fixed template bundle. Names and sources are copied before return.
 *
 * Every named template is HTML-autoescaped. There is no filesystem loader and
 * render calls may select only names frozen into this bundle.
 *
 * One renderer belongs to one execution context and must not be used
 * concurrently. A later worker-oriented ownership model is tracked separately.
 */
chttp_web_status chttp_web_renderer_init(
    chttp_web_renderer *renderer,
    const chttp_web_template *templates,
    size_t template_count,
    const chttp_web_renderer_config *config,
    chttp_web_error *error);

/**
 * Renders one frozen template against a typed CMeta root.
 *
 * On success, *out_html is malloc-owned, NUL-terminated, and may contain
 * embedded NUL bytes before its terminator; *out_size is authoritative.
 * On failure, *out_html is NULL and *out_size is zero. No partial output is
 * published.
 */
chttp_web_status chttp_web_render(
    chttp_web_renderer *renderer,
    const char *template_name,
    const cmeta_data_desc *model_desc,
    const void *model,
    char **out_html,
    size_t *out_size,
    chttp_web_error *error);

/**
 * Renders fully before committing an in-memory CHTTP response. A zero status
 * code selects 200. A NULL content type selects "text/html; charset=utf-8".
 * Render failures therefore leave the response uncommitted.
 */
chttp_web_status chttp_web_render_response(
    chttp_web_renderer *renderer,
    chttp_server_response *response,
    const char *template_name,
    const cmeta_data_desc *model_desc,
    const void *model,
    unsigned int status_code,
    const char *content_type,
    chttp_web_error *error);

void chttp_web_output_free(char *html);
void chttp_web_renderer_destroy(chttp_web_renderer *renderer);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_WEB_WEB_H */
