#ifndef CHTTP_WEB_WEB_H
#define CHTTP_WEB_WEB_H

#include <cmeta/data.h>
#include <http_server/http.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
  CHTTP_WEB_FLASH = -12,
  CHTTP_WEB_MULTIPART = -13
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

/**
 * One Server-Sent Event. Presence flags distinguish an omitted field from an
 * explicitly empty field. Text is borrowed and must be valid UTF-8.
 */
typedef struct chttp_web_sse_event {
  size_t size;
  chttp_web_string_view event;
  chttp_web_string_view data;
  chttp_web_string_view id;
  uint64_t retry_ms;
  bool has_event;
  bool has_data;
  bool has_id;
  bool has_retry;
} chttp_web_sse_event;

#define CHTTP_WEB_SSE_EVENT_INIT \
  {sizeof(chttp_web_sse_event), {NULL, 0u}, {NULL, 0u}, {NULL, 0u}, \
   0u, false, false, false, false}

/**
 * Produces the next event synchronously on the CHTTP owner thread.
 *
 * Return SALTS_OK with out_event populated, SALTS_ENOENT for normal EOF, or a
 * negative Salts status to fail the response. Returned views must remain valid
 * until the next producer call or stream cleanup.
 */
typedef int (*chttp_web_sse_next_fn)(
    void *user, chttp_web_sse_event *out_event);

/** Exactly-once stream terminal callback; status follows CHTTP source cleanup. */
typedef void (*chttp_web_sse_close_fn)(void *user, int status);

/**
 * Caller-owned SSE source state. No heap is allocated by the SSE layer.
 * Initialize it with CHTTP_WEB_SSE_STREAM_INIT (or all-zero storage) before
 * first use. An active stream cannot be reinitialized.
 *
 * scratch is the hard per-event formatted-byte bound. The complete stream is
 * additionally bounded by chttp_server_config.max_response_body_bytes.
 * Treat fields after scratch_capacity as implementation state.
 */
typedef struct chttp_web_sse_stream {
  size_t size;
  chttp_web_sse_next_fn next;
  chttp_web_sse_close_fn close;
  void *user;
  char *scratch;
  size_t scratch_capacity;
  size_t buffered_offset;
  size_t buffered_size;
  int terminal_status;
  bool active;
  bool eof;
} chttp_web_sse_stream;

#define CHTTP_WEB_SSE_STREAM_INIT \
  {sizeof(chttp_web_sse_stream), NULL, NULL, NULL, NULL, 0u, 0u, 0u, \
   0, false, false}

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

/** One structured validation error copied into caller-owned storage. */
typedef struct chttp_web_validation_error {
  chttp_web_string_view field;
  chttp_web_string_view message;
  bool global;
} chttp_web_validation_error;

/**
 * Request-local bounded validation result.
 *
 * Error entries and their field/message bytes are copied into caller-owned
 * storage supplied to chttp_web_validation_init(). The public `valid` and
 * `errors` fields form the template-facing typed view; storage fields are
 * lifecycle state and are not exposed through the CMeta descriptor.
 */
typedef struct chttp_web_validation {
  size_t size;
  bool valid;
  chttp_web_sequence_view errors;
  chttp_web_validation_error *error_storage;
  size_t error_capacity;
  char *byte_storage;
  size_t byte_capacity;
  size_t byte_used;
} chttp_web_validation;

#define CHTTP_WEB_VALIDATION_INIT \
  {sizeof(chttp_web_validation), true, {NULL, 0u, 0u, NULL}, \
   NULL, 0u, NULL, 0u, 0u}

const cmeta_data_desc *chttp_web_validation_error_data(void);
const cmeta_data_desc *chttp_web_validation_data(void);

chttp_web_status chttp_web_validation_init(
    chttp_web_validation *validation,
    chttp_web_validation_error *error_storage,
    size_t error_capacity,
    char *byte_storage,
    size_t byte_capacity,
    chttp_web_error *error);

chttp_web_status chttp_web_validation_reset(
    chttp_web_validation *validation,
    chttp_web_error *error);

chttp_web_status chttp_web_validation_add_field(
    chttp_web_validation *validation,
    const char *field,
    const char *message,
    chttp_web_error *error);

chttp_web_status chttp_web_validation_add_global(
    chttp_web_validation *validation,
    const char *message,
    chttp_web_error *error);

size_t chttp_web_validation_field_count(
    const chttp_web_validation *validation,
    const char *field);

const chttp_web_validation_error *chttp_web_validation_field_get(
    const chttp_web_validation *validation,
    const char *field,
    size_t occurrence);

size_t chttp_web_validation_global_count(
    const chttp_web_validation *validation);

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

enum {
  CHTTP_WEB_MULTIPART_BOUNDARY_HARD_MAX = 70,
  CHTTP_WEB_MULTIPART_HEADER_COUNT_HARD_MAX = 64,
  CHTTP_WEB_MULTIPART_HEADER_BYTES_HARD_MAX = 8192,
  CHTTP_WEB_MULTIPART_NAME_HARD_MAX = 256,
  CHTTP_WEB_MULTIPART_FILENAME_HARD_MAX = 1024,
  CHTTP_WEB_MULTIPART_CONTENT_TYPE_HARD_MAX = 256
};

typedef struct chttp_web_multipart_part {
  size_t size;
  chttp_web_string_view name;
  chttp_web_string_view filename;
  chttp_web_string_view content_type;
  bool has_filename;
  bool has_content_type;
} chttp_web_multipart_part;

#define CHTTP_WEB_MULTIPART_PART_INIT \
  {sizeof(chttp_web_multipart_part), {NULL, 0u}, {NULL, 0u}, \
   {NULL, 0u}, false, false}

typedef int (*chttp_web_multipart_part_begin_fn)(
    void *user, const chttp_web_multipart_part *part);
typedef int (*chttp_web_multipart_part_data_fn)(
    void *user, const void *data, size_t size);
typedef int (*chttp_web_multipart_part_end_fn)(void *user);

typedef struct chttp_web_multipart_callbacks {
  size_t size;
  chttp_web_multipart_part_begin_fn part_begin;
  chttp_web_multipart_part_data_fn part_data;
  chttp_web_multipart_part_end_fn part_end;
} chttp_web_multipart_callbacks;

#define CHTTP_WEB_MULTIPART_CALLBACKS_INIT \
  {sizeof(chttp_web_multipart_callbacks), NULL, NULL, NULL}

typedef struct chttp_web_multipart_limits {
  size_t size;
  size_t max_parts;
  size_t max_header_count;
  size_t max_header_bytes;
  size_t max_name_bytes;
  size_t max_filename_bytes;
  size_t max_content_type_bytes;
  size_t max_field_bytes;
  size_t max_total_bytes;
} chttp_web_multipart_limits;

#define CHTTP_WEB_MULTIPART_LIMITS_INIT \
  {sizeof(chttp_web_multipart_limits), 128u, 16u, 4096u, 128u, 512u, \
   256u, 64u * 1024u, 16u * 1024u * 1024u}

/**
 * Caller-owned incremental multipart/form-data parser.
 *
 * The parser never allocates and never owns persistence. Header/metadata
 * storage and the boundary-prefix holdback are embedded in this caller-owned
 * object and are bounded by the hard maxima above. Part metadata views passed
 * to part_begin are borrowed until the matching part_end callback returns.
 * Part-data bytes are borrowed only for the duration of part_data.
 *
 * Fields after user are implementation state and must not be modified.
 */
typedef struct chttp_web_multipart_parser {
  size_t size;
  chttp_web_multipart_limits limits;
  chttp_web_multipart_callbacks callbacks;
  void *user;

  char boundary[CHTTP_WEB_MULTIPART_BOUNDARY_HARD_MAX + 1u];
  size_t boundary_size;
  char header_bytes[CHTTP_WEB_MULTIPART_HEADER_BYTES_HARD_MAX];
  size_t header_size;
  char part_name[CHTTP_WEB_MULTIPART_NAME_HARD_MAX + 1u];
  size_t part_name_size;
  char part_filename[CHTTP_WEB_MULTIPART_FILENAME_HARD_MAX + 1u];
  size_t part_filename_size;
  char part_content_type[CHTTP_WEB_MULTIPART_CONTENT_TYPE_HARD_MAX + 1u];
  size_t part_content_type_size;
  char pending[CHTTP_WEB_MULTIPART_BOUNDARY_HARD_MAX + 4u];
  size_t pending_size;
  size_t total_bytes;
  size_t part_count;
  size_t field_bytes;
  size_t initial_index;
  unsigned int state;
  unsigned int suffix_state;
  bool active;
  bool failed;
  bool current_file;
  bool has_filename;
  bool has_content_type;
} chttp_web_multipart_parser;

#define CHTTP_WEB_MULTIPART_PARSER_INIT {0}

/**
 * Initializes a strict RFC 7578 browser multipart parser from the outer
 * Content-Type. A quoted boundary is accepted. The boundary is limited to the
 * MIME 70-byte maximum and must use valid boundary characters.
 */
chttp_web_status chttp_web_multipart_init(
    chttp_web_multipart_parser *parser,
    const char *content_type,
    const chttp_web_multipart_limits *limits,
    const chttp_web_multipart_callbacks *callbacks,
    void *user,
    chttp_web_error *error);

/**
 * Incrementally consumes arbitrary body chunks. Boundary delimiters may split
 * at any byte. Callback failures terminate the parser and are preserved in
 * error->native_status.
 */
chttp_web_status chttp_web_multipart_feed(
    chttp_web_multipart_parser *parser,
    const void *data,
    size_t data_size,
    chttp_web_error *error);

/**
 * Completes the message. Success requires a fully received closing delimiter;
 * truncated or malformed input fails closed.
 */
chttp_web_status chttp_web_multipart_finish(
    chttp_web_multipart_parser *parser,
    chttp_web_error *error);

/** Resets a parser for another body using the same boundary/config/callbacks. */
chttp_web_status chttp_web_multipart_reset(
    chttp_web_multipart_parser *parser,
    chttp_web_error *error);

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
 * Formats one event canonically into caller-owned storage.
 *
 * event/id values reject CR/LF (and NUL) to prevent field injection. Data may
 * contain CR, LF, or CRLF; they are normalized into one "data:" field per
 * logical line while preserving leading/trailing empty lines. The event always
 * ends with one blank line.
 */
chttp_web_status chttp_web_sse_format_event(
    const chttp_web_sse_event *event,
    char *buffer,
    size_t buffer_capacity,
    size_t *out_size,
    chttp_web_error *error);

/**
 * Initializes a no-allocation SSE stream over a synchronous producer.
 * Reinitializing an active stream is invalid application behavior.
 */
chttp_web_status chttp_web_sse_stream_init(
    chttp_web_sse_stream *stream,
    chttp_web_sse_next_fn next,
    chttp_web_sse_close_fn close,
    void *user,
    char *scratch,
    size_t scratch_capacity,
    chttp_web_error *error);

/**
 * Commits a 200 text/event-stream response over CHTTP's existing response
 * source engine. Adds Cache-Control: no-cache. HTTP framing remains fully
 * owned by CHTTP for both HTTP/1.1 and HTTP/2.
 */
chttp_web_status chttp_web_sse_response(
    chttp_server_response *response,
    chttp_web_sse_stream *stream,
    chttp_web_error *error);

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
