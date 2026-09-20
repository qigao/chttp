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
  CHTTP_WEB_SERVER = -8,
  CHTTP_WEB_FORM = -9,
  CHTTP_WEB_BIND = -10,
  CHTTP_WEB_CSRF = -11,
  CHTTP_WEB_FLASH = -12
} chttp_web_status;

typedef struct DataBind DataBind;
typedef struct TbeTypedType TbeTypedType;
typedef struct TbeTypedDescriptor TbeTypedDescriptor;

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
 * Renderer-compatible CMeta descriptors for borrowed vstr and contiguous
 * sequence views. These keep Jinja-specific descriptor details inside
 * CHttp::Web so application/presentation code does not depend on Jinja CMeta.
 */
const cmeta_data_desc *chttp_web_vstr_cmeta_data(void);
const cmeta_data_desc *chttp_web_sequence_cmeta_data(void);

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
  chttp_web_string_view csrf_token;
  bool csrf_available;
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

/** One decoded application/x-www-form-urlencoded key/value pair. */
typedef struct chttp_web_form_pair {
  chttp_web_string_view name;
  chttp_web_string_view value;
} chttp_web_form_pair;

/**
 * Borrowed parsed form view. Pair names/values point into caller-owned byte
 * storage supplied to chttp_web_form_parse().
 */
typedef struct chttp_web_form {
  const chttp_web_form_pair *pairs;
  size_t pair_count;
  size_t decoded_bytes;
} chttp_web_form;

/**
 * Hard parser limits plus caller-owned storage. Parsing never allocates.
 * max_decoded_bytes counts decoded name+value bytes, excluding separators.
 */
typedef struct chttp_web_form_parse_options {
  size_t size;
  size_t max_input_bytes;
  size_t max_pairs;
  size_t max_decoded_bytes;
  chttp_web_form_pair *pair_storage;
  size_t pair_capacity;
  char *byte_storage;
  size_t byte_capacity;
} chttp_web_form_parse_options;

#define CHTTP_WEB_FORM_PARSE_OPTIONS_INIT \
  {sizeof(chttp_web_form_parse_options), 64u * 1024u, 128u, 64u * 1024u, \
   NULL, 0u, NULL, 0u}

/**
 * Caller-owned JSON bridge storage used before DataBind performs transactional
 * native conversion. No destination mutation occurs until the complete bridge
 * document has been produced.
 */
typedef struct chttp_web_form_bind_options {
  size_t size;
  char *json_storage;
  size_t json_capacity;
} chttp_web_form_bind_options;

#define CHTTP_WEB_FORM_BIND_OPTIONS_INIT \
  {sizeof(chttp_web_form_bind_options), NULL, 0u}

enum {
  CHTTP_WEB_CSRF_RANDOM_BYTES = 32,
  CHTTP_WEB_CSRF_TOKEN_BYTES = 64,
  CHTTP_WEB_FLASH_HARD_MAX_MESSAGES = 16,
  CHTTP_WEB_FLASH_HARD_MAX_SERIALIZED_BYTES = 1024
};

#define CHTTP_WEB_CSRF_FORM_FIELD "_csrf"
#define CHTTP_WEB_CSRF_HEADER "X-CSRF-Token"

typedef struct chttp_web_flash_message {
  chttp_web_string_view level;
  chttp_web_string_view text;
} chttp_web_flash_message;

typedef struct chttp_web_flash_config {
  size_t size;
  size_t max_messages;
  size_t max_level_bytes;
  size_t max_text_bytes;
  size_t max_serialized_bytes;
} chttp_web_flash_config;

#define CHTTP_WEB_FLASH_CONFIG_INIT \
  {sizeof(chttp_web_flash_config), 4u, 32u, 256u, 1024u}

typedef struct chttp_web_flash_buffer {
  size_t size;
  chttp_web_flash_message *message_storage;
  size_t message_capacity;
  char *byte_storage;
  size_t byte_capacity;
} chttp_web_flash_buffer;

#define CHTTP_WEB_FLASH_BUFFER_INIT \
  {sizeof(chttp_web_flash_buffer), NULL, 0u, NULL, 0u}

/**
 * Borrowed browser security policy. Non-NULL header values must remain valid
 * until the server is destroyed. NULL omits that header.
 *
 * strict_transport_security is deliberately NULL in the reference profiles.
 * Applications must enable HSTS explicitly only for deployments that are
 * actually HTTPS at the browser boundary.
 */
typedef struct chttp_web_security_policy {
  size_t size;
  const char *content_security_policy;
  const char *referrer_policy;
  const char *frame_options;
  const char *strict_transport_security;
  const char *cache_control;
  bool nosniff;
} chttp_web_security_policy;

#define CHTTP_WEB_SECURITY_POLICY_INIT \
  {sizeof(chttp_web_security_policy), NULL, NULL, NULL, NULL, NULL, false}



/**
 * Builds one synchronous, non-reentrant renderer from an application-owned
 * fixed template bundle. Names and sources are copied before return.
 *
 * Every named template is HTML-autoescaped. There is no filesystem loader and
 * render calls may select only names frozen into this bundle.
 *
 * A renderer is synchronous and non-reentrant. It may be used on an
 * application worker thread, but the application must ensure that render,
 * init, and destroy never overlap for the same renderer. Parallel workers
 * should own independent renderers (or externally serialize one renderer).
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
 * published. The owned buffer is suitable for cross-thread deferred reply:
 * chttp_server_deferred_reply() copies it before returning.
 *
 * model_desc/model are borrowed only for this call. Handler-scoped request,
 * route-param, header, session, and JWT views must be copied before leaving
 * the owner-thread callback and may not be retained by a worker.
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

/**
 * Worker-oriented terminal helper for a previously deferred response.
 *
 * Renders completely into CHttp::Web-owned temporary output, then calls the
 * thread-safe generation-checked chttp_server_deferred_reply(). The server
 * copies the body before this function frees the render buffer. A zero status
 * selects 200; NULL content_type selects "text/html; charset=utf-8".
 *
 * Success consumes the deferred handle. Server-side stale/cancelled/bounds
 * failures return CHTTP_WEB_SERVER with error->native_status preserving the
 * Salts status; the helper never retries or cancels implicitly.
 */
chttp_web_status chttp_web_deferred_render_reply(
    chttp_web_renderer *renderer,
    chttp_server_deferred *deferred,
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

const char *chttp_web_csrf_token(const chttp_session *session);

chttp_web_status chttp_web_csrf_ensure(
    chttp_session *session,
    const char **out_token,
    chttp_web_error *error);

chttp_web_status chttp_web_csrf_rotate(
    chttp_session *session,
    const char **out_token,
    chttp_web_error *error);

chttp_web_status chttp_web_csrf_clear(
    chttp_session *session,
    chttp_web_error *error);

/**
 * Validates POST/PUT/PATCH/DELETE. GET/HEAD/OPTIONS pass without a token.
 * Ordinary requests use one _csrf form field. Exact HTMX requests may use
 * X-CSRF-Token; when that header is present it is authoritative. This helper
 * is opt-in, so JWT-only API routes remain outside CSRF scope by not invoking it.
 */
chttp_web_status chttp_web_csrf_validate(
    const chttp_server_request_view *request,
    const chttp_web_form *form,
    chttp_web_error *error);

chttp_web_status chttp_web_flash_push(
    chttp_session *session,
    const chttp_web_flash_config *config,
    const char *level,
    const char *text,
    chttp_web_error *error);

chttp_web_status chttp_web_flash_consume(
    chttp_session *session,
    const chttp_web_flash_config *config,
    const chttp_web_flash_buffer *buffer,
    size_t *out_count,
    chttp_web_error *error);

chttp_web_status chttp_web_flash_clear(
    chttp_session *session,
    chttp_web_error *error);

/**
 * Strict same-origin browser profile:
 * - no inline-script/style allowance;
 * - no object embedding;
 * - no framing;
 * - same-origin forms, scripts, styles, images and connections.
 *
 * Cross-origin OpenAPI Try-it requires an explicit application CSP override
 * for connect-src; the reference profile does not silently widen it.
 */
chttp_web_security_policy chttp_web_security_reference_policy(void);

/** Reference browser profile plus Cache-Control: private, no-store. */
chttp_web_security_policy chttp_web_security_authenticated_policy(void);

/**
 * Appends security middleware to the existing CHTTP middleware chain.
 *
 * The policy is validated before middleware capacity is consumed and is then
 * borrowed until server destruction. Security headers are installed before
 * calling next, so later middleware or the terminal handler may explicitly
 * replace a header using chttp_server_response_set_header(). Register this
 * middleware before CORS when security headers are also required on CORS
 * preflight responses. Responses rejected by JWT/global admission before the
 * middleware chain do not receive these headers.
 *
 * Jinja HTML autoescape and this HTTP security policy are independent
 * guarantees: neither replaces the other.
 */
int chttp_web_security_use(
    chttp_server *server,
    const chttp_web_security_policy *policy);


/**
 * Parses application/x-www-form-urlencoded bytes into bounded caller-owned
 * storage. '+' decodes to space; percent escapes require exactly two hex
 * digits. Empty field names, empty '&' segments, malformed escapes, and all
 * configured limit overflows fail closed. On failure out_form is zeroed.
 */
chttp_web_status chttp_web_form_parse(
    const void *data,
    size_t data_size,
    const chttp_web_form_parse_options *options,
    chttp_web_form *out_form,
    chttp_web_error *error);

/** Returns the number of exact case-sensitive occurrences of name. */
size_t chttp_web_form_count(
    const chttp_web_form *form,
    const char *name);

/** Returns the zero-based exact occurrence of name, or NULL. */
const chttp_web_form_pair *chttp_web_form_get(
    const chttp_web_form *form,
    const char *name,
    size_t occurrence);

/**
 * Transactionally binds a parsed form into an initialized DataBind typed
 * object. Scalar fields require exactly one occurrence. LIST/SET/FIXED_ARRAY
 * fields consume repeated form keys as array elements. Flat form binding
 * intentionally rejects OBJECT/MAP and object-valued collections.
 *
 * The JSON bridge is fully materialized inside options->json_storage before
 * DataBind is invoked. DataBind's typed parse contract keeps the previous
 * destination object unchanged on every failure.
 */
chttp_web_status chttp_web_form_bind_typed(
    const chttp_web_form *form,
    DataBind *codec,
    const char *type_name,
    const TbeTypedType *type,
    void *destination,
    const chttp_web_form_bind_options *options,
    chttp_web_error *error);

/**
 * Canonical CMeta/DataBind descriptor variant of chttp_web_form_bind_typed().
 * Only descriptor shapes already supported by DataBind are accepted.
 */
chttp_web_status chttp_web_form_bind_descriptor(
    const chttp_web_form *form,
    DataBind *codec,
    const char *type_name,
    const TbeTypedDescriptor *descriptor,
    void *destination,
    const chttp_web_form_bind_options *options,
    chttp_web_error *error);


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
