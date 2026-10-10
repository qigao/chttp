#ifndef CHTTP_APP_INTERCEPTOR_H
#define CHTTP_APP_INTERCEPTOR_H

#include <chttp_app/service.h>
#include <cmeta/ace_interceptor.h>
#include <data_bind_message_plan.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Callback-scoped HTTP dispatch view. Never retain these pointers or defer
 * from a hook. Headers may be added before dispatch; the Service owns replies.
 * Native values and execution remain owned by DataBind and the Service. */
typedef struct chttp_service_call {
  const DataBindBindingPlan *binding;
  const DataBindServiceNativeBinding *native;
  const chttp_server_request_view *request;
  chttp_server_response *response;
} chttp_service_call;

typedef struct chttp_service_dispatch_result {
  int status;
  bool entered;
} chttp_service_dispatch_result;

CMETA_INTERCEPTOR_TYPE(chttp_service_interceptor,
    chttp_service_call, chttp_service_dispatch_result);

enum { CHTTP_SERVICE_MAX_INTERCEPTORS = 16 };

/** Startup-only policy selection over producer-owned IDL/Function metadata.
 * Fill at most capacity hooks and always set count, including zero when deliberately
 * selecting no policy. Unknown required policies must return an error.
 * Output storage is callback-scoped and must not be retained. Hooks are copied;
 * their contexts and code are borrowed through Service
 * destruction. Selection runs after native admission, before route publication.
 * Failure publishes no route. No selector or metadata search runs per request.
 */
typedef int (*chttp_service_policy_select_fn)(
    void *context, const DataBindBindingPlan *binding,
    const DataBindServiceNativeBinding *native,
    chttp_service_interceptor_hook *hooks, size_t capacity, size_t *count);

/** Additive mount API; the existing mount ABI and middleware remain unchanged.
 * Global/route middleware surrounds this chain and retains its usual successful
 * short-circuit semantics. Hooks run before in registration order, and after or
 * on_error in reverse. A rejected before (proceed=false), or CALLBACK_ERROR
 * before dispatch, produces HTTP 403; other hook errors produce HTTP 500.
 * Hooks must not publish replies. Native dispatch errors retain their original
 * status. In deferred modes after observes submission, NOT worker completion.
 * Selection must be non-NULL; malformed/over-capacity chains fail at mount.
 */
int chttp_service_mount_http_with_policies(
    chttp_service *service, chttp_server *server,
    const chttp_service_http_mount *mount,
    chttp_service_policy_select_fn select, void *context);

/** Opt-in document response: JSON or XML object, selected by the
 * MethodPlan's egress_format. XML root is the response IDL type name. Nested
 * output names follow IDL [name]; aliases are input-only. Egress projection
 * names must remain canonical, and all fields must target response_body.
 * XML supports nested records and required list/set fields of scalars/records.
 * Typed errors, XML bytes/nullable/optional-sequence/map/variant shapes and
 * sequences of sequences are rejected before publication. Operational errors
 * retain text/plain replies.
 * select may be NULL. Ingress and lifetime rules are unchanged. Encoded bytes
 * obey max_response_body_bytes; format work also obeys native_max_items and
 * native_max_owned_bytes. Recursive names use at most
 * DATA_BIND_FORMAT_CURSOR_MAX_DEPTH aggregate frames (64 in SaltsUtils rc.7).
 * Requires SaltsUtils 4.3.0-rc.7 or newer with Salts 2.3.0-rc.9 or newer.
 * No Accept negotiation or format fallback occurs.
 * Returns SALTS_OK on registration, SALTS_ENOTSUP for an unsupported plan,
 * SALTS_EINVAL for invalid arguments, or the existing mount/selector error.
 */
int chttp_service_mount_http_document(
    chttp_service *service, chttp_server *server,
    const chttp_service_http_mount *mount,
    chttp_service_policy_select_fn select, void *context);

/** Document request and response, with the response rules above. All ingress
 * fields must target http.body with canonical projection names; mixed body and
 * query/path/header/cookie bindings are rejected. request_plan must be prepared
 * from the same immutable IDL contract and native request binding as method_plan.
 * The Service borrows both plans and descriptor domains through destruction.
 * It checks native type/state compatibility before publishing the route.
 *
 * ingress_format selects JSON or XML. A single Content-Type is required:
 * application/json for JSON; application/xml or text/xml for XML. An optional
 * charset=utf-8 parameter is supported (case-insensitive, optionally quoted).
 * Other media types/parameters return 415. No sniffing or Accept negotiation.
 * XML root labeling follows the DataBind reader; it is not type identity.
 * Required list/set fields collect repeated child elements, including aliases;
 * no elements means an empty sequence. Each invocation owns its reader cursor.
 *
 * The buffered body obeys Server max_request_body_bytes. Parsing/decoding obey
 * native depth/items/owned-byte/workspace limits. Owned native input is decoded
 * and validated on the HTTP owner before dispatch, and retained through deferred
 * finalization. Parser leases and request views never cross the worker boundary.
 * Invalid input returns 400, schema validation 422, limits 413, runtime errors
 * 500 (text/plain). select may be NULL. Mount errors follow the document API;
 * a NULL or mismatched request_plan returns SALTS_EINVAL without registration.
 */
int chttp_service_mount_http_document_body(
    chttp_service *service, chttp_server *server,
    const chttp_service_http_mount *mount,
    const DataBindMessagePlan *request_plan,
    chttp_service_policy_select_fn select, void *context);

/** Opt-in JSON/XML response negotiation. method_plan supplies the default
 * response format; alternate_response supplies the other format for the same
 * response IDL contract. Compile it using data_bind_format_plan_compile from
 * the same immutable codec/contract (not the reader-only compile entry).
 * Both plans are borrowed through Service destruction and admitted at mount.
 * NULL or same-format/type-name mismatch returns SALTS_EINVAL; unsupported
 * shapes return SALTS_ENOTSUP. No route is published on failure.
 *
 * request_plan NULL keeps scalar query/path/header/cookie input; otherwise the
 * document_body input rules apply. Only responses are negotiated. Missing
 * Accept or equal offer weights selects the MethodPlan default. Media ranges,
 * q weights (0..1, at most three decimal places), wildcards and repeated header
 * lines are supported. More specific matching ranges override broad ranges,
 * including q=0. The first equally specific range wins for each offer.
 * Offers are parameterless application/json and application/xml; additional
 * media parameters do not match them. Empty or unmatched Accept returns 406;
 * malformed syntax returns 400. Vary: Accept is appended before dispatch,
 * retaining existing Vary values and deferred lifetime. Errors remain plain
 * text. Old mount APIs keep fixed-format behavior. select may be NULL.
 */
int chttp_service_mount_http_negotiated_document(
    chttp_service *service, chttp_server *server,
    const chttp_service_http_mount *mount,
    const DataBindMessagePlan *request_plan,
    const DataBindFormatPlan *alternate_response,
    chttp_service_policy_select_fn select, void *context);

#ifdef __cplusplus
}
#endif
#endif
