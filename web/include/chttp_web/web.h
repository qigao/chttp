#ifndef CHTTP_WEB_WEB_H
#define CHTTP_WEB_WEB_H

#include <cmeta/data.h>
#include <http_server/http.h>

#include <stdbool.h>
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
 * Borrowed immutable bytes used by the typed request context. The pointed
 * bytes are never owned by CHttp::Web and must outlive the render using them.
 */
typedef struct chttp_web_string_view {
  const char *data;
  size_t size;
} chttp_web_string_view;

/** One named borrowed request value exposed to templates. */
typedef struct chttp_web_named_value {
  chttp_web_string_view name;
  chttp_web_string_view value;
  bool present;
} chttp_web_named_value;

/**
 * Public CHttp::Web sequence layout. The element descriptor and pointed
 * storage are borrowed. Applications should treat this as read-only after
 * chttp_web_request_context_init() until rendering completes.
 */
typedef struct chttp_web_sequence_view {
  const void *data;
  size_t count;
  size_t stride;
  const cmeta_data_desc *element;
} chttp_web_sequence_view;

/**
 * Handler-scoped typed request context.
 *
 * Every string ultimately borrows either the active CHTTP request/session or
 * application-owned selector strings. Nothing in this object may be retained
 * past the route handler unless the application copies the underlying bytes.
 */
typedef struct chttp_web_request_context {
  chttp_web_string_view method;
  chttp_web_string_view target;
  chttp_web_string_view path;
  bool htmx;
  bool session_available;
  chttp_web_sequence_view params;
  chttp_web_sequence_view headers;
  chttp_web_sequence_view session;
} chttp_web_request_context;

/**
 * Bounded caller-owned scratch storage and explicit exposure policy.
 *
 * Params are all route params already bounded by CHTTP configuration. Headers
 * and session values are exposed only for the explicitly selected names/keys.
 * Missing selected values still occupy one row with present=false.
 */
typedef struct chttp_web_request_context_options {
  size_t size;
  const char *const *header_names;
  size_t header_name_count;
  const char *const *session_keys;
  size_t session_key_count;
  chttp_web_named_value *param_storage;
  size_t param_capacity;
  chttp_web_named_value *header_storage;
  size_t header_capacity;
  chttp_web_named_value *session_storage;
  size_t session_capacity;
} chttp_web_request_context_options;

#define CHTTP_WEB_REQUEST_CONTEXT_OPTIONS_INIT \
  {sizeof(chttp_web_request_context_options), NULL, 0u, NULL, 0u, \
   NULL, 0u, NULL, 0u, NULL, 0u}

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

/** True only for an HX-Request header whose value is exactly "true". */
bool chttp_web_request_is_htmx(const chttp_server_request_view *request);

/**
 * Snapshots handler-borrowed request/session pointers into a bounded typed
 * context. The function performs no allocation and never retains request.
 */
chttp_web_status chttp_web_request_context_init(
    chttp_web_request_context *context,
    const chttp_server_request_view *request,
    const chttp_web_request_context_options *options,
    chttp_web_error *error);

/** CMeta descriptor for embedding chttp_web_request_context in application models. */
const cmeta_data_desc *chttp_web_request_context_data(void);

/**
 * Sends an empty ordinary HTTP redirect. Only 301/302/303/307/308 are
 * accepted. Location is copied by CHTTP before return.
 */
chttp_web_status chttp_web_redirect(
    chttp_server_response *response,
    unsigned int status_code,
    const char *location,
    chttp_web_error *error);

/** Non-terminal HTMX response-header helpers; values are copied by CHTTP. */
chttp_web_status chttp_web_hx_redirect(
    chttp_server_response *response,
    const char *location,
    chttp_web_error *error);
chttp_web_status chttp_web_hx_trigger(
    chttp_server_response *response,
    const char *trigger,
    chttp_web_error *error);
chttp_web_status chttp_web_hx_retarget(
    chttp_server_response *response,
    const char *selector,
    chttp_web_error *error);

/** Renders one HTML error page; status_code must be in the 400..599 range. */
chttp_web_status chttp_web_render_error(
    chttp_web_renderer *renderer,
    chttp_server_response *response,
    unsigned int status_code,
    const char *template_name,
    const cmeta_data_desc *model_desc,
    const void *model,
    chttp_web_error *error);

void chttp_web_output_free(char *html);
void chttp_web_renderer_destroy(chttp_web_renderer *renderer);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_WEB_WEB_H */
