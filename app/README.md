# CHttp::App

`CHttp::App` is CHTTP's native-C application module. One `chttp_app` shared
library implements generated IDL services and server-driven Web applications.

> **Product claim:** `CHttp::App` combines CHTTP routing, middleware,
> sessions, security, deferred responses, streaming, HTTP/1.1 and HTTP/2 with
> typed CMeta models and Jinja CMeta server-side rendering.

It is intended for admin UIs, dashboards, developer portals, CRUD/internal
tools, device consoles, and hybrid HTML/API services.

It is **not** a client-side framework, virtual DOM, browser runtime, or
Wt-style server widget toolkit. Browser behavior remains ordinary HTML plus
optional progressive enhancement such as HTMX.

## Architecture boundary

```text
Application
    |
    v
CHttp::App
    |-----------------------------.
    v                             |
CHttp::Server                     |
routes / middleware / sessions    |
JWT / CORS / rate limits          |
deferred / streaming / H2         |
    |                             |
    '----------> CMeta <----------'
                  |
                  v
             Jinja CMeta
             SSR / fragments
                  |
                  v
               Browser
```

The dependency direction is deliberate:

- `CHttp::Server` does not depend on Jinja, HTMX, or UI code.
- Jinja CMeta does not depend on CHTTP request/session types.
- `CHttp::App` is the bridge and may depend on both.
- Existing CHTTP routing, middleware, sessions, JWT, CORS, rate limiting,
  deferred replies, streaming, HTTP/2, WebSocket, file serving, and statistics
  remain owned by CHTTP rather than being reimplemented here.

## Package use

The installed CMake package exports one `CHttp::App` shared-library target.
Service and Web are capabilities within App, not separate libraries or build
targets. See the [merge and migration decision](../README.md#app-entry-point).

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_web_app LANGUAGES C)

find_package(Chttp CONFIG REQUIRED PATHS "$ENV{CHTTP_ROOT}" NO_DEFAULT_PATH)

add_executable(my_web_app main.c)
target_link_libraries(my_web_app PRIVATE CHttp::App)
```

Applications include the public entry point:

```c
#include <chttp_app/app.h>
```

### Schema-owned endpoint configuration (requires the updated SaltsUtils compiler)

The schema projection feature under development in SaltsUtils lets applications
keep HTTP/RPC mappings beside service declarations. SaltsUtils 4.3.0-rc.7 does
not yet provide `SCHEMA_PROJECTION`; use the updated compiler and CMake helper
together. Existing external `PROJECTION_CONFIG` projects remain supported.

```text
schema App;
message Lookup { uint32 id; }
message User { uint32 id; string name; }
service Users {
  [http("GET", "/users/{id}"),
   app_http_field("ingress", "id", "path", "id"),
   app_rpc("users.get")]
  Get: Lookup -> User;

  [http("POST", "/users"), app_http_status(201),
   app_http_formats("json", "xml"), app_rpc("users.create")]
  Create: User -> User;
}
```

These are structured annotations checked during generation. Ordinary comments
document the service. The example accepts JSON for `Create` and defaults its
response to XML; JSON-RPC continues to use JSON.

```cmake
salts_idl_target(
  TARGET users_contract
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/users.schema"
  ARTIFACT_NAME users
  ARTIFACTS NATIVE
  BINARY_CODEC
  TRANSPORTS HTTP RPC
  SCHEMA_PROJECTION)
target_sources(users_contract_native PRIVATE users_service.c)
target_link_libraries(my_web_app PRIVATE CHttp::App users_contract_native)
```

The build generates `users.projection.json`, HTTP/RPC projections, native types
and service bindings. Business code implements the declarations in
`users.service_native.h`; regeneration leaves `users_service.c` untouched.
Multiple services and operations can share one schema. Every operation needs an
explicit mapping for each selected transport. Missing mappings, unknown fields,
annotation typos and duplicate endpoints fail generation.

This removes manually maintained projection JSON. For a complete synchronous
HTTP application, use the CHttp helper below. Keep secrets and deployment
settings outside schema.

### Generate the HTTP application

`chttp_app_target` combines native service generation, schema projection and an
application host. It requires the updated SaltsUtils compiler/helper advertising
`SaltsUtils_IDL_APPLICATION_VERSION >= 2`; the released 4.3.0-rc.7 compiler does
not provide this capability. The helper reports this at configure time. The
runtime uses Salts 2.3.0-rc.9 Component and Interceptor contracts.

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_application LANGUAGES C)
find_package(Chttp CONFIG REQUIRED PATHS "$ENV{CHTTP_ROOT}" NO_DEFAULT_PATH)
chttp_app_target(TARGET my_application
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/app.schema"
  SOURCES services.c)
```

```text
schema App;
message Input { uint32 value; }
message Output { uint32 value; }
service Echo {
  [http("POST", "/echo"), app_http_policy("no_store")]
  Call: Input -> Output;
}
```

The only required C source implements the generated signature:

```c
#include "my_application.service_native.h"
int databind_3_App_4_Echo_4_Call(const Input_t *request, Output_t *response) {
  response->value = request->value;
  return 0;
}
```

Build with the project's existing configure/build presets, then run
`my_application --port 8080`. The generated executable binds loopback by default;
SIGINT/SIGTERM initiates shutdown. `--port 0` selects an ephemeral port. The
host embeds the codec, mounts every operation before listening, decodes JSON/XML
bodies or scalar path/query/header/cookie parameters, and encodes the declared
document response. No runtime schema file or hand-written `main.c` is needed.
The complete [multi-service example](examples/schema_app/app.schema) has three
endpoints and [three business functions](examples/schema_app/services.c).

The helper generates `${TARGET}_application`, a static library exposing
`${TARGET}_application_init`, alongside the executable `${TARGET}`. Tests or
custom hosts link this library and use `chttp_application_start`, `port`,
`get_stats` and `close`. Generated files live in the build directory; regeneration
never overwrites business sources. `LIBRARIES` adds business implementation
dependencies. Existing `salts_idl_target` and manual mounting remain supported.

### Automatic service dependency injection

Declare application dependencies on a service. The three arguments name the
generated member, an existing CMeta Interface, and the header declaring it:

Use `inject` and `http` in schemas; migrate the earlier experimental `app_inject`
and `app_http` spellings and regenerate. Companion annotations such as
`app_http_policy` and `app_http_field` retain their names.

```text
schema Injected;
message Numbers { uint32 value; }
message Answer { uint64 value; }
[inject("factor", "FactorSource", "factor_source.h")]
service Calc {
  [http("POST", "/scale")]
  Scale: Numbers -> Answer;
}
```

The generated business signature receives a typed, immutable dependency record:

```c
#include "my_application.service_native.h"
int databind_8_Injected_4_Calc_5_Scale(
    const databind_8_Injected_4_Calc_dependencies *dependencies,
    const Numbers_t *request, Answer_t *response) {
  FactorSource factor = dependencies->factor;
  response->value = (uint64_t)request->value * FactorSource_value(&factor);
  return 0;
}
```

The provider implements and publishes `FactorSource` using an ordinary
`salts_component_provider_binding`. Deployment supplies providers and their
configuration once through `options.providers`/`provider_count`; it need not
allocate graph storage, resolve dependencies, start components or bind methods.
The [complete example](examples/injected_app/CMakeLists.txt) supplies two
interfaces from one [provider](examples/injected_app/factor_source.c), declares
both in the [schema](examples/injected_app/app.schema), and configures the
provider in [configure.c](examples/injected_app/configure.c). `INCLUDES` makes
external Interface headers available to generated and business code;
`LIBRARIES` links provider implementations.

Init appends generated consumers to the deployment graph, resolves dependencies,
creates providers in dependency order, and binds each receiver before mounting
routes. All methods of one service share a dependency record. Separate apps own
separate graphs and instances. Factories may themselves require other interfaces;
the existing Component resolver handles these transitively. Exactly one provider
must match each requirement. Use `options.selections` with consumer component ID,
Interface descriptor and provider component ID to resolve intentional ambiguity.
The generated `<dependencies_type>_component()->component->stable_id` supplies the
consumer ID without duplicating its spelling.

Missing providers return `SALTS_ENOENT`; ambiguity, cycles, metadata mismatch and
provider callback failures return `SALTS_EINVAL`. Set `component_diagnostic` to
receive the precise Component status, phase, indices and callback status during
init, including after rollback. Failed init never starts the listener. There is
no default provider selection or global service locator.

Dependencies are borrowed Interface carriers, not request/response fields; JSON,
XML and MethodPlan schemas are unchanged. The generated native adapter proves
the three-argument C signature and validates CMeta receiver/Function ABI
projection to the logical two-argument method. Dispatch uses the bound context
directly. Copying an Interface carrier does not retain its provider; business
code must not keep it past application close or start untracked asynchronous work.

The application owns bounded graph storage (128 total providers/consumers and
4096 dependency edges/selections). It copies deployment/selection rows; provider
metadata, Interface descriptors, selection strings and provider contexts must
outlive close. Configuration values are borrowed only during factory creation;
providers must copy anything needed later. Caller-supplied `options.components`
and automatic graphs are mutually exclusive. Method dependencies support at most
16 distinct member names and Interface tokens per service; two members of the
same Interface are rejected. All Interface includes precede C linkage declarations
so public generated headers work in C11 and C++17.

This is an opt-in application-scoped injection contract. Existing stateless
service prototypes are unchanged. Providers still own resource creation, failure
handling and synchronization; reflection does not construct arbitrary types or
choose configuration. Request scopes, field injection, dynamic provider swapping,
Plugin/Wasm publication and typed-error injected services are outside this
contract. Unsupported publication ABIs fail generation. To roll back, remove
`inject` and restore the stateless signature, or keep the generated receiver
and host its Component graph explicitly using `salts_idl_target`.

### Method policies and deployment configuration

Repeat `app_http_policy("name")` on a method to select hooks in declaration order.
Names contain ASCII letters, digits, `_`, `.` or `-`; duplicates and more than 16
policies fail generation. `no_store` is built in and adds `Cache-Control: no-store`.
Other names must resolve in `chttp_application_options.policies`; missing names
fail initialization with `SALTS_ENOENT` before the listener starts. Names alone
do not implement authentication or authorization.

An optional `CONFIGURE configure_app` CMake argument calls
`int configure_app(chttp_application_options *)` before host initialization.
Declare it in a user source passed to `SOURCES`; return `SALTS_OK` on success.
This is the deployment boundary for host/port, limits, TLS configuration,
custom hook contexts and an optional READY `salts_component_context`:

```c
static cmeta_status authorize(void *context,
    const chttp_service_call *call, bool *proceed); /* supplied policy implementation */
static const chttp_application_policy policies[] = {
  {"authorize", {NULL, authorize, NULL, NULL}}
};
int configure_app(chttp_application_options *options) {
  options->policies = policies;
  options->policy_count = sizeof(policies) / sizeof(policies[0]);
  return SALTS_OK;
}
```

Policy selection runs once after native admission. The existing typed CMeta
Interceptor executes before hooks forward, then after/on_error hooks in reverse.
Hook code and contexts must outlive close. Hooks cannot publish their own replies
or retain callback views. Policy declarations are emitted in the HTTP header;
the version-1 projection JSON deliberately retains its existing route/format
schema and does **not** carry policy names. Deploy the generated application,
not that JSON alone as a complete policy configuration.

### Host ownership and scope

The host is the sole owner of its server, Service, native binding storage and
method plans. The codec owns acquired message plans. All records have stable
addresses, bounded by 1024 methods and 64 registered policy names. Configuration
defaults bound network, body and native staging storage; callers can adjust them
before init. Inline services and hooks must not block the HTTP owner thread.

An optional Component graph resolves dependencies and activates before routes
are admitted; errors unwind acquired resources. Normal close stops/joins the
server, destroys Service and codec resources, then stops components in reverse
dependency order. A close timeout retains the whole application for retry.
For manual graphs, graph/provider storage stays caller-owned. For automatic
injection, the host owns graph storage and stops it before freeing it. Component
failure diagnostics remain available through the optional init output. Already
active manual graphs are rejected rather than adopted.

This design reuses the native producer's operation catalog instead of duplicating
IDL parsing or symbol mangling in CMake. Runtime metadata and invocation remain
DataBind/CMeta-owned; CHttp owns transport, policy selection and lifetime. The
alternative of generating an independent server implementation would duplicate
those contracts and make shutdown fixes inconsistent across projects.

The generated host currently supports HTTP with synchronous direct execution.
RPC, deferred executors, Plugin leases and content negotiation still use their
existing explicit mounting APIs. Mixed document-body and scalar parameter input,
typed error documents and unsupported JSON/XML shapes fail mount admission;
there is no silent downgrade. Native service dependencies use the opt-in receiver
described above. Nested XML work is unchanged. Stateless migration is additive:
adopt the helper per target, or return to `salts_idl_target` plus explicit hosting
without changing stateless service signatures.

The installed App behavior test links only `CHttp::App` and
executes Web parsing/validation/upload adapters plus Service initialization
and destruction against the installed SDK. From `app/tests/installed`, use
`installed-release-user` for configure/build/CTest after the root release
preset installs the SDK. The test preset owns `CHTTP_ROOT`, deriving it from
`${sourceDir}/../../../stage/sdk/$env{QIGAO_SDK_RID}` with forward slashes,
including on Windows; it does not consume the parent process's `CHTTP_ROOT`.
macOS uses `installed-macos-release-user`. Both share the root vcpkg manifest.
Consumers outside this formal test set their own `CHTTP_ROOT` in their preset
environment, using forward slashes on Windows and preserving the package
discovery expression above.
Full HTTP MethodPlan, deferred execution and DSO behavior remain covered by
the formal integration tests under `app/tests/service` and `app/tests/web`.

## Generated service capability

`<chttp_app/service.h>` exposes the existing `chttp_service` API. App admits
generated DataBind MethodPlan/native capabilities at mount, materializes
bounded per-invocation request/response storage, and supports inline direct,
deferred direct, CFlow and Plugin execution. Plugin mounts retain their DSO
lease until accepted work and HTTP terminal obligations have finished.

The service owner and renderer remain explicit independent resources within
the same module. Initialize and configure them before server startup. Stop
servers and drain accepted work before destroying mounted service state,
borrowed providers or renderers. `chttp_service_destroy` reports busy rather
than freeing live deferred work. Rendering is synchronous and non-reentrant;
parallel workers need independent renderers or external serialization.

The merge preserves feature API names, layouts and error semantics. Consumers
must replace the old includes/link targets, rebuild, and deploy `chttp_app`.
No Service/Web compatibility DLLs or target aliases are installed.

## JSON and XML service responses

`chttp_service_mount_http_document()` opts one operation into a structured
response. The compiled MethodPlan's `egress_format` selects JSON or XML and
sets `Content-Type` to `application/json` or `application/xml`. Business code
continues to fill its generated C response; it does not assemble wire text.

```c
/* Copy the generated projection before compiling the MethodPlan. */
DataBindHttpProjectionConfig projection = *generated_projection;
projection.egress_format = DATA_BIND_FORMAT_JSON; /* or DATA_BIND_FORMAT_XML */
DataBindHttpMethodPlan *plan = NULL;
DataBindBindingPlanDiagnostic diagnostic = DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
DataBindStatus compiled = data_bind_http_method_plan_compile_service(
    contract, "Calculator", "Add", &projection, &native, &plan, &diagnostic);
if (compiled != DATA_BIND_OK) return SALTS_EINVAL;
mount.method_plan = plan;
/* Keep plan/native/execution alive through service destruction. */
int status = chttp_service_mount_http_document(
    &service, server, &mount, NULL, NULL);
/* Optional final arguments select the same reflected policies described below. */
```

For `AddResponse { uint32 sum; }`, JSON emits `{"sum":7}` and XML emits an
`<AddResponse><sum>7</sum></AddResponse>` document. XML may include a declaration
and whitespace. The existing scalar mount still emits plain `7`.

The private App/RpcService bridge consumes the admitted BindingPlan and feeds
`data_bind_native_encode()` into DataBind's canonical FormatPlan writer.
Output field names, including nested records and record elements, follow IDL
`[name(...)]`; aliases remain input-only.
Egress entries must all target `response_body` with canonical projection names.
JSON shapes are subject to the SDK's Native and FormatPlan admission; the
end-to-end fixture qualifies generated multi-field and nested records with exact
uint64 values, owned strings, lists of records and scalar sets.
Document adapters require SaltsUtils **4.3.0-rc.7** and Salts **2.3.0-rc.9**
or compatible newer SDKs. Regenerate bindings and rebuild consumers together.

XML uses the existing explicit-root DataBind writer: its root is the IDL
response type. It supports nested records and required list/set fields whose
elements are scalars or records. Collection elements repeat the field element;
an empty sequence emits no elements. Bytes, nullable fields, optional sequences,
sequences of sequences, maps and variants are rejected by plan compilation or
mount admission. Typed-error operations are rejected by
both document mounts because the SDK does not expose the corresponding
producer-owned per-error FormatPlan. Ordinary operational HTTP errors keep
their existing plain-text responses. This response-only API keeps existing
request binding. The body mount below adds document input; neither API performs
`Accept` negotiation or fallback.

Encoded HTTP bytes obey `max_response_body_bytes`; format token work obeys
`native_max_items` and `native_max_owned_bytes`. Recursive field-name projection
also has a fixed ceiling of `DATA_BIND_FORMAT_CURSOR_MAX_DEPTH` (64 aggregate
frames in rc.7); a larger native depth setting does not raise that ceiling.
Output is invocation-owned and
published only after all writers finish successfully. Failure discards the
staged document. Deferred direct execution uses the same value lifetime and
terminal ownership as scalar responses. Stop and destroy the server, drain
accepted work, destroy Service, then free its borrowed MethodPlans.

JSON-RPC shares this encoding path through
`chttp_rpc_service_mount_document()`, while retaining the JSON-RPC envelope.
Its matching `chttp_rpc_service_client_call_document()` accepts a prepared
response MessagePlan and returns an owning native C record with schema
validation and rollback, including optional/null state and field aliases.
See [RPC document results](../docs/RPC.md#generated-rpcservice-json-对象结果).

### JSON and XML request bodies

Use `chttp_service_mount_http_document_body()` for a complete document request
and response. Prepare the request MessagePlan once, using the same IDL contract
and generated native binding as the HTTP MethodPlan:

```c
const DataBindMessagePlan *request_plan = NULL;
DataBindError error = DATA_BIND_ERROR_INIT;
DataBindStatus prepared = data_bind_message_plan_acquire_generated(
    codec, AddRequest_native_artifact(), &request_plan, &error);
if (prepared != DATA_BIND_OK) return SALTS_EINVAL;

/* Compile method_plan with method="POST", route="/add",
 * ingress_format=JSON, egress_format=JSON (or XML), and canonical body
 * projections. The default HTTP field projection targets the body. */
chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
mount.method_plan = method_plan;
mount.native_binding = native;
mount.execution = execution;
int status = chttp_service_mount_http_document_body(
    &service, &server, &mount, request_plan, NULL, NULL);
```

For `AddRequest { uint32 a; uint32 b; }`, send
`Content-Type: application/json` with `{"a":3,"b":4}`, or configure XML ingress
and send `Content-Type: application/xml` with
`<AddRequest><a>3</a><b>4</b></AddRequest>`. The business function receives its
generated `const AddRequest_t *`; it does not parse HTTP, JSON or XML. Input and
output formats are independent fixed MethodPlan choices. XML ingress uses the
upstream plan-aware reader to group repeated elements, including field aliases,
into required lists/sets; missing elements produce empty sequences. Nested
record names are canonicalized recursively with an invocation-local cursor.

All input fields must target `http.body` with canonical projection names.
Mixed body/query/path/header/cookie projections fail at mount rather than
silently changing binding precedence. IDL `[name]` and aliases are handled by
FormatPlan; MessagePlan handles required/default/optional/null state and schema
validation. XML root labeling follows the DataBind reader and does not select a
native type. Native type, state layout and field-count mismatches fail before
route publication; callers must supply plans from the same immutable contract.

The route requires a single matching `Content-Type`: `application/json`, or
`application/xml`/`text/xml` for XML ingress. It accepts an optional UTF-8 charset
parameter, including quoted and case-insensitive forms. Missing/mismatched
media types and unsupported parameters return 415. Invalid syntax, unknown or
duplicate fields, missing required fields and wrong value types return 400;
schema constraints return 422; decoding limits return 413. Operational errors
retain text/plain responses. There is no content sniffing or format fallback.

Server `max_request_body_bytes` bounds the buffered wire body. Parser depth and
native workspace/items/owned-byte budgets remain explicit. The HTTP owner fully
decodes and validates input, closes the parser lease, then submits deferred work.
Only invocation-owned native values cross to the worker; cleanup runs at the
existing exactly-once finalization boundary. Stop the server and drain accepted
work before destroying Service, its borrowed MethodPlans and the codec owning
the prepared MessagePlan. No per-request schema compilation occurs.

This additive API reuses the SDK's whole-message decoder because BindingPlan's
existing per-field provider cannot consume a document in one pass. It avoids
adding a transport-owned field tree or another parser. The explicit prepared
MessagePlan supplies the producer's validation and state semantics; public
configuration layouts and scalar mounts stay unchanged. Rollback removes the
body route or restores its previous explicit client/server contract.

### Select JSON or XML with Accept

`chttp_service_mount_http_negotiated_document()` explicitly enables response
negotiation for one route. Prepare the alternate response FormatPlan at startup
from the same immutable IDL contract as the MethodPlan. The MethodPlan's egress
format is the default; the alternate must be the other supported format for the
same response type. Both must pass document admission before registration.

```c
DataBindFormatPlan *xml_response = NULL;
DataBindError error = DATA_BIND_ERROR_INIT;
DataBindStatus prepared = data_bind_format_plan_compile(
    codec, "AddResponse", DATA_BIND_FORMAT_XML, &xml_response, &error);
if (prepared != DATA_BIND_OK) return SALTS_EINVAL;

/* mount.method_plan has JSON egress; request_plan enables document body input.
 * Pass NULL instead for the existing scalar query/path/header/cookie input. */
int status = chttp_service_mount_http_negotiated_document(
    &service, &server, &mount, request_plan, xml_response, NULL, NULL);
if (status != SALTS_OK) data_bind_format_plan_free(xml_response);
/* On success, free xml_response only after Service destruction. */
```

A client can POST JSON with `Accept: application/xml` and receive XML, or use
`Accept: application/json;q=0.4, application/xml;q=0.9`. Request Content-Type
still follows the fixed ingress format; it never changes the output preference.
XML shapes follow the nested-record and required-sequence rules described above.

The selection follows the weight and specificity rules of
[RFC 9110 sections 12.4–12.5.1](https://www.rfc-editor.org/rfc/rfc9110.html#name-accept):

- Missing Accept uses the MethodPlan default. An empty Accept accepts neither
  representation and returns 406.
- Supported offers are parameterless `application/json` and `application/xml`.
  A range with additional media parameters does not match either offer;
  `text/xml` is an accepted request-body media type but is not an output offer.
- Type/subtype matching is case-insensitive. Exact matches override `type/*`,
  which overrides `*/*`, including explicit `q=0` exclusions. Weight is 0–1
  with at most three fractional digits and defaults to 1.
- Repeated Accept lines form one logical list. For equally specific duplicate
  ranges the first wins. Equal final offer weights prefer the MethodPlan default.
- Unsupported or excluded offers return 406; malformed syntax returns 400.
  Both happen before body decoding or business invocation. Policy hooks and
  middleware still run first. Operational errors remain text/plain.

Every response reaching negotiation appends `Vary: Accept`, including rejection
responses. Server's public `chttp_server_response_append_vary()` preserves prior
Vary fields such as Origin, validates appended field lists, and respects header
capacity limits. Existing `Vary: *` remains unchanged. Headers are copied by the
Server and survive deferred completion; App never inspects private Server state.

The selected immutable plan pointer belongs to one invocation and remains valid
under the existing mount lifetime. The HTTP owner scans already bounded request
headers in linear time with constant storage, then the worker uses that selection.
No request changes shared method metadata, recompiles a schema, or retries another
format after encoding failure. Output buffers, task admission, cancellation and
drain ownership retain their existing limits and finalizers.

This is an additive choice because changing fixed-format mounts would change
their wire contract. Supplying a prepared alternate plan keeps format capability
and schema ownership in DataBind; adding format flags and runtime codec creation
would duplicate that authority. App owns selection, DataBind owns encoding, and
Server owns Vary storage. To migrate, compile both representations and replace
the mount call; to roll back, use the fixed-format mount with its original client
contract. JSON-RPC retains its required JSON envelope and is not negotiated.

### Compatibility decision and verification

Existing projections default to JSON even though the legacy HTTP adapter
returns scalar text. Changing that adapter implicitly would break existing
clients. The additive document mount makes the wire change explicit and keeps
existing public configuration layouts and mount behavior stable. It reuses
the installed DataBind/CSerde codecs rather than introducing another serializer
or a dependency from Server to App. Rollback selects the old mount together
with the matching scalar client contract; it is not a runtime fallback.

The generated fixture in `tests/service/chttp_document.schema` and
`chttp_document_test.c` exercises real HTTP JSON, deferred XML, JSON-RPC result
objects, canonical name mapping, escaping, exact uint64 output, bounded output
failure and subsequent request recovery. Typed-client cases additionally cover
owned result lifetime, embedded NUL, aliases, optional/null state, schema
validation, malformed/duplicate/unknown fields, decode budgets and remote errors.
Request-body cases additionally cover inline JSON, deferred XML and JSON
state round trips, media-type rejection, mount incompatibility, input validation,
wire/native limits and recovery after failure. The installed App consumer links
the additive body mount through `CHttp::App` alone.
Negotiation cases cover weighted/specific/duplicate ranges, invalid and empty
Accept, mount rejection, fixed-route compatibility, Vary merging/validation,
and concurrent deferred JSON/XML responses over both HTTP/1.1 and HTTP/2.
Nested cases cover record and collection aliases, repeated XML elements, empty
sequences, deferred ownership, format negotiation, malformed-input recovery,
binary-leaf rejection and owned RPC response lifetime.
Run the repository build preset, then
`ctest --preset ci-sdk-release-user -R chttp_document_test --output-on-failure`.

## Reflected method policies and Component assembly

`<chttp_app/interceptor.h>` adds `chttp_service_mount_http_with_policies()`.
Its startup selector receives the admitted DataBind BindingPlan and canonical
native binding. Use `data_bind_binding_plan_operation_id()` (for example,
`Calculator.Add`), binding entries, and `native->function` to select method
policies. The selector fills a bounded array of `chttp_service_interceptor_hook`;
App validates and copies up to 16 hooks before publishing the route. Returning
an error, exceeding capacity, or returning an empty hook publishes no route
and releases any mount-acquired Plugin lease. Explicitly selecting zero hooks
is allowed. Unknown required policies should fail selection.

```c
/* select_policies implements chttp_service_policy_select_fn. */
int status = chttp_service_mount_http_with_policies(
    &service, server, &mount, select_policies, policy_context);
```

The chain uses `CMETA_INTERCEPTOR_TYPE` over `chttp_service_call` and
`chttp_service_dispatch_result`. Native business Function/ABI admission still
uses the generated DataBind execution contract; it is not reinterpreted as the
different HTTP hook signature. Before hooks run forward; after/error hooks
unwind in reverse. Rejection (`proceed=false`) or `CMETA_CALLBACK_ERROR` before
dispatch produces 403; other hook failures produce 500. Both prevent native
execution. Hooks may add headers before dispatch but must not reply, defer,
retain request views, or stop the server. Existing outer HTTP middleware
retains its normal short-circuit behavior, including CORS.

These are **HTTP dispatch** hooks. A successful HTTP error response is still a
successful dispatch. For deferred direct, CFlow and Plugin execution, `after`
means the dispatch/submission callback returned; it does not signal worker,
native-value, or network completion. The existing deferred terminal and task
finalizer remain authoritative. Hook contexts/code are borrowed until Service
destruction; startup is exclusive, hooks execute on the HTTP owner, and shared
contexts across servers need application synchronization. Storage is bounded
by `method_capacity * 16` hook slots; no request-time policy lookup or hook
allocation occurs. This does not replace authentication, CSRF, or admission.

The real [HTTP example](../http_server/examples/http_example_configurator.c)
uses `Salts::Component` for `ExampleService requires example_contract` and
`ExampleContract provides example_contract`. It deliberately lists the consumer
first. The container resolves the graph, creates Contract before Service,
injects a typed Interface carrying the borrowed MethodPlan, and rolls back or
destroys in reverse order. Distinct owned resource records release the Service
and Contract; they do not own the enclosing application. The example's policy
selector installs `no-store` for the reflected `Calculator.Add` operation,
including Plugin mounts, without a manual route middleware array.

Server/executor/Plugin shutdown remains with the application: stop admission,
drain accepted work, destroy the server, then stop the Component graph before
unloading Plugin providers. Component callbacks cannot report asynchronous
`EBUSY`; calling graph stop before drain is outside this contract. A failed
Component start has already rolled back and must not be stopped a second time.
Renderer, limiter and network configuration remain explicit domain resources.

### Decision and migration

The existing middleware ABI has successful short-circuit semantics, whereas
CMeta Interceptor rejection is an error unwind. Replacing that ABI would change
CORS, replies and deferred ownership. The chosen additive App boundary keeps
those behaviors and reuses CMeta typed chains and Salts Component resolution;
there is no additional registry, scheduler, RTTI table or dependency resolver.
Component is an example/application dependency, not a new Server dependency.

The current SDK exposes operation identity, binding and native signature
metadata, **not arbitrary operation annotations**. Policies are selected by
application code over that metadata; this API does not claim an `@Authorize`
annotation compiler or automatic container discovery. Such annotations require
a producer-owned DataBind export contract before adding an adapter here.

Existing applications can keep `chttp_service_mount_http()` unchanged, adopt
the policy mount operation by operation, and optionally compose their startup
with `Salts::Component`. Rollback is to the existing mount API and explicit
assembly; no schema, wire format, existing struct layout or persisted data
changes. Rebuild App consumers that include the new header against the matching
Salts RC SDK. Real HTTP tests cover selection failure, bounded admission,
denial/error unwind, native and deferred execution, dependency order, rollback
and retry; the C++ header test checks the public typed hook signature.

## Core model

A Web application normally composes these pieces:

1. Initialize one or more bounded `chttp_web_renderer` instances from a fixed
   application-owned template bundle.
2. Register ordinary CHTTP routes and middleware.
3. Build typed CMeta models in route handlers.
4. Render a full page or fragment with `chttp_web_render_response()`.
5. Use `chttp_web_request_context_init()` when templates need selected
   request parameters, headers, session values, or HTMX state.
6. Parse and bind bounded form input with the form APIs.
7. Apply CSRF validation to cookie/session-authenticated mutation routes.
8. Use deferred replies for blocking business work and SSE for event streams.
9. Stop/drain the server and worker execution before destroying renderers.

Templates are frozen into the renderer. Request-controlled filesystem template
loading is not part of the default product surface.

## Browser authentication and application shell

Web III adds browser-authenticated application composition without turning
CHttp::App into an identity provider.

The intended flow is:

```text
anonymous browser
    |
    +-- GET /login
    |      -> Session + CSRF token
    |
    +-- POST /login
    |      -> bounded form parse
    |      -> application verifies credentials
    |      -> Session ID regeneration
    |      -> principal publication
    |      -> CSRF rotation
    |      -> safe local redirect / HX-Redirect
    |
    +-- protected route
    |      -> chttp_web_auth_middleware
    |      -> Session principal
    |      -> optional application authorization callback
    |      -> page / fragment
    |
    '-- POST /logout
           -> CSRF validation
           -> Session invalidation
           -> expired cookie
```

Credential storage, password hashing, account policy, MFA, user databases, and
OIDC/SAML/OAuth provider logic remain application or integration concerns.
CHttp::App never verifies passwords by itself. The application supplies a
verified identity to `chttp_web_principal_sign_in()`.

### Session fixation and CSRF

A successful privilege transition must not reuse the anonymous Session
identifier.

`chttp_web_principal_sign_in()` validates CSRF, regenerates the CHTTP Session
identifier, publishes the principal only under the regenerated Session, then
rotates CSRF. If principal publication or CSRF rotation fails after
regeneration, the Session is invalidated rather than leaving partially
authenticated state.

The old Session identifier stops authenticating immediately after successful
regeneration. Logout validates CSRF and invalidates the complete Session, so a
stale authenticated cookie does not keep principal state alive.

Do not copy authentication state into application cookies as a workaround for
Session lifecycle. Keep the CHTTP Session as the browser principal carrier and
treat principal views as request-scoped borrowed data.

### Protected routes and authorization

`chttp_web_auth_middleware()` requires an authenticated Session principal and
may invoke an application-owned authorization callback.

Callback semantics are explicit:

- `SALTS_OK` — authorize and continue to the protected handler;
- `SALTS_EPERM` — explicit authorization denial;
- every other error — fail closed and propagate as a server error.

The default authorization-denied response is HTTP 403. Applications may supply
a forbidden handler for rendered UX. Templates never make authorization
decisions.

JWT bearer admission remains a `CHttp::Server` capability. Browser Session
authorization and JWT bearer authorization are separate composition choices;
CHttp::App does not translate one into the other.

### Redirect safety

Post-login return targets are optional and always local.

`chttp_web_local_target_validate()` accepts only bounded origin-form targets
that begin with exactly one `/`. It rejects absolute URLs, scheme-relative
targets, controls, fragments, backslashes, malformed percent escapes, and
encoded bytes that can change authority/path interpretation.

When `include_return_target` is enabled, auth middleware percent-encodes the
validated request target into the configured login URL. It never constructs a
redirect from the request `Host` or `Origin` header.

Ordinary unauthenticated browser requests receive a 303 redirect. Exact HTMX
requests receive the same local destination through `HX-Redirect` on a
non-3xx response.

Applications should validate a submitted `return_to` again in the login
handler before the privilege transition. The authenticated reference
application does so before calling `chttp_web_principal_sign_in()`.

### Static assets and application shell

`chttp_web_assets_use()` is a thin URL-to-file mapping layer over
`chttp_server_serve_file()`; CHTTP remains the file streaming, conditional
request, range, metadata, and async-I/O owner.

A mount:

- owns one explicit URL prefix and one application-owned filesystem root;
- admits GET and HEAD only;
- percent-decodes into bounded storage and validates UTF-8;
- rejects traversal, encoded separators, controls, backslashes, drive-like
  separators, symlink components, and Windows reparse points;
- returns deterministic 404 for malformed, escaping, missing, or rejected
  paths;
- may apply conservative or immutable Cache-Control policy;
- may supply an application-selected strong ETag, otherwise inheriting CHTTP
  metadata validators.

SPA fallback is opt-in under a separate configured namespace. An asset miss or
an unrelated API miss is never silently converted into the application shell.

The current path-based CHTTP file API requires the selected file to remain
immutable through asynchronous completion. Therefore the asset root and its
topology must remain immutable from server start until stop. This is not a
mutable virtual filesystem API.

### Authenticated reference application

`app/examples/chttp_web_authenticated_app.c` is the Web III reference
application. It uses only bounded in-memory demo identity records so the
example demonstrates browser composition rather than database or password
infrastructure.

It covers anonymous login, bounded form + CSRF, application credential
verification, Session regeneration and CSRF rotation, protected and denied
routes, ordinary and HTMX transitions, local return-target validation, static
CSS through the asset mount, Jinja CMeta full-page/fragment rendering, hostile
principal autoescape, and CSRF-protected logout.

Build and run it interactively:

```text
cmake --build build/linux-gcc-debug --target chttp_web_authenticated_app_example
build/linux-gcc-debug/bin/chttp_web_authenticated_app_example
```

The same executable has a deterministic qualification mode:

```text
build/linux-gcc-debug/bin/chttp_web_authenticated_app_example --self-test
```

That mode drives a real CHTTP client through H1 and H2 and verifies stale
pre-login/post-logout cookies, redirect safety, protected-handler
non-execution, authorization denial, asset serving, autoescape, and HTMX
progressive fragments.

## Reference applications

The repository keeps reference programs under `app/examples/`. They are part
of the normal example build and must remain independently compilable.

| Example | What it demonstrates |
| --- | --- |
| `chttp_web_example.c` | Minimal SSR, typed CMeta model, template inheritance, HTML autoescape, normal CHTTP route lifecycle |
| `chttp_web_context_example.c` | Typed request context, route params, selected headers/session values, full-page vs HTMX fragment rendering, HX response helpers |
| `chttp_web_crud_example.c` | Bounded in-memory CRUD, typed user sequence, ordinary form fallback, HTMX-compatible fragment mutation, session CSRF, conservative Web security policy |
| `chttp_web_session_example.c` | Bounded form parsing, session-backed CSRF token lifecycle, mutation validation, flash messages |
| `chttp_web_security_example.c` | Web security middleware composed with rate limiting, CORS, JWT bearer routes, explicit header override |
| `chttp_web_deferred_example.c` | Owner-thread request-state copy, response defer, bounded Salts worker queue, worker-side render and generation-checked deferred reply |
| `chttp_web_form_upload_example.c` | Structured validation, ordinary + HTMX error UX, multipart upload staging, Session CSRF, flash/redirect success, hostile metadata escaping |
| `chttp_web_authenticated_app.c` | Complete browser-authenticated flow: login, Session regeneration, principal/authz middleware, safe return targets, asset mount, Jinja full-page/fragment rendering, logout, stale-cookie and autoescape qualification |

Configure with examples enabled and build the desired target:

```text
cmake --preset linux-dev-user -DBUILD_EXAMPLES=ON
cmake --build build/linux-gcc-debug --target chttp_web_example
cmake --build build/linux-gcc-debug --target chttp_web_context_example
cmake --build build/linux-gcc-debug --target chttp_web_crud_example
cmake --build build/linux-gcc-debug --target chttp_web_session_example
cmake --build build/linux-gcc-debug --target chttp_web_security_example
cmake --build build/linux-gcc-debug --target chttp_web_deferred_example
cmake --build build/linux-gcc-debug --target chttp_web_form_upload_example
cmake --build build/linux-gcc-debug --target chttp_web_authenticated_app_example
```

The repository presets require the same configured Salts, SaltsUtils, vcpkg,
and project roots documented in the top-level README.

## SSR and fragments

`chttp_web_render_response()` renders completely before the HTTP response is
committed. A render failure therefore cannot publish a partial HTML response.

The renderer is bounded by:

- maximum template bytes;
- maximum output bytes;
- maximum nodes/value visits;
- maximum render depth.

The same HTML autoescape policy applies to full-page and fragment rendering.

`chttp_web_context_example.c` shows the normal progressive-enhancement
pattern:

- ordinary request -> full page;
- `HX-Request: true` -> fragment;
- `HX-Trigger`, `HX-Retarget`, and `HX-Redirect` remain response-header
  conveniences rather than a second protocol stack.

## HTMX CRUD reference flow

The executable `chttp_web_crud_example.c` is the compact reference application.
It stores a bounded user set in memory so the example can focus on the Web
contract rather than a database layer. The rendered forms work as ordinary
HTML and also carry `hx-post`, `hx-target`, and `hx-swap` attributes for
progressive enhancement.

A server-driven CRUD application can stay on ordinary CHTTP routes:

| Route | Server behavior |
| --- | --- |
| `GET /users` | Render the full users page |
| `GET /users/:id` | Render full detail for an ordinary request or a detail fragment for HTMX |
| `POST /users` | Parse a bounded form, validate CSRF, bind a typed model, perform the application write, push a flash message, then redirect or emit an HX refresh trigger |
| `POST /users/:id/delete` | Validate CSRF, perform the delete, then return an ordinary redirect or HTMX fragment/trigger |
| `GET /events` | Optional SSE stream for live refresh notifications |

The Web layer intentionally does not own persistence or business transactions.
Validation and database/domain writes belong to the application. The Web layer
owns the HTTP-facing parsing, typed presentation context, browser security
helpers, and rendering bridge.

`chttp_web_crud_example.c` composes these mechanics end to end. The smaller
`chttp_web_context_example.c` and `chttp_web_session_example.c` remain useful
when studying request/fragment and session/form concerns independently.

## Forms, sessions, CSRF, and flash messages

`application/x-www-form-urlencoded` parsing is bounded and allocation-free
from the parser's point of view: the caller supplies pair storage and decoded
byte storage.

表单绑定统一使用 `chttp_web_form_bind_method_plan()` 与生成的 DataBind
HTTP MethodPlan。旧 typed/descriptor 入口及 JSON 中转存储已删除，不提供兼容接口。
标量、枚举与重复字段的标量集合直接转换为 cserde token；字段名称、默认值、
校验、集合构造及释放由 BindingPlan/CMeta 描述符统一负责。对象、map 和嵌套集合被拒绝。

调用者提供有界 `DataBindNativeOptions` 工作区、标量 scratch 和未持有资源的暂存帧。
绑定失败时，已初始化的帧字段恢复为语义空值；绑定成功后再提交业务写入，并按生成类型的
生命周期接口释放暂存资源。不能向绑定接口传入已有业务对象。完整示例与失败回归见
[`test_chttp_web_form_plan.c`](tests/test_chttp_web_form_plan.c)。

For cookie/session-authenticated mutations:

- call `chttp_web_csrf_ensure()` to make a token available;
- expose it through the request context/template;
- submit it as `_csrf` or, for an exact HTMX request, as
  `X-CSRF-Token`;
- call `chttp_web_csrf_validate()` before the application mutation;
- rotate or clear it when the application's session lifecycle requires it.

JWT-only API routes do not become CSRF-protected automatically; they remain
outside CSRF scope unless the application explicitly composes session/browser
semantics.

Flash messages are bounded session state and are consumed explicitly. They are
not a second persistence mechanism.

### Structured validation

`chttp_web_validation` is a request-local, caller-storage-backed result that
can be rendered directly through its CMeta descriptor. Applications may add
field errors and global errors after parsing/binding and after domain checks.

The validation object:

- never allocates hidden storage;
- preserves deterministic insertion order;
- copies field/message bytes into caller-supplied bounded storage;
- can be reset/reused between requests;
- exposes `valid` plus a typed error sequence to templates;
- does not interpret application/domain rules.

A failed validation result is presentation state, not an HTTP transport error.
Ordinary browser requests can re-render the complete page while exact HTMX
requests can render only the affected fragment. The same Jinja HTML autoescape
policy applies to validation messages and re-populated values.

### Streaming multipart uploads

`multipart/form-data` uses the incremental
`chttp_web_multipart_parser` and `chttp_web_upload_request` transaction on
top of CHTTP's existing route body-sink API.

The ownership chain is:

```text
application-owned bounded upload slot
    |
body_open
    v
chttp_web_upload_request
    |
    +-- incremental multipart parser
    +-- reserved bounded _csrf capture
    +-- application staging callbacks
    |
body_close
    v
terminal handler via request.body_sink_user
    |
    +-- CSRF validation
    +-- structured/application validation
    +-- commit exactly once
    '--- or abort exactly once
```

Important limits and lifecycle rules:

- multipart boundary length is capped by the MIME 70-byte maximum;
- part count, header count/bytes, field bytes, filename bytes, content-type
  bytes, and total body bytes are all explicit bounds;
- request body bytes and part metadata views are callback-scoped;
- application staging callbacks run synchronously on the CHTTP owner thread and
  must not perform unbounded blocking;
- disk/database/remote persistence must use application-owned bounded
  queueing/worker policy when blocking work is required;
- `part_end` closes only one staged part and never means final application
  commit;
- the final commit happens only after the complete body, CSRF, and application
  validation succeed;
- parser, sink, disconnect, server-stop, CSRF, validation, or commit failures
  abort staging exactly once;
- caller-owned upload state may be reused only after commit or abort.

The full browser-oriented composition is
`app/examples/chttp_web_form_upload_example.c`. It uses a fixed application
slot pool and bounded in-memory staging deliberately; CHttp::App does not
provide a hidden upload directory or persistence layer.

## Deferred worker rendering

CHTTP route handlers run on the server owner thread and must not block on
application work.

The supported pattern is:

```text
owner thread
    |
    +-- validate/authenticate
    +-- copy only the request values the worker needs
    +-- reserve bounded worker capacity
    +-- chttp_server_response_defer()
    |
    v
application worker
    |
    +-- business/database work
    +-- build typed model
    +-- chttp_web_deferred_render_reply()
```

`chttp_web_deferred_example.c` demonstrates this with a bounded
`salts_threadpool`. It deliberately uses one renderer with one worker thread,
which satisfies the renderer's synchronous non-reentrant ownership contract.
Applications that need parallel rendering should give workers independent
renderers or provide their own explicit serialization.

Handler-scoped request, route-param, header, session, and JWT views must never
be retained by a worker. Copy required values before returning from the owner
thread callback.

A stale/cancelled deferred handle fails through the existing generation-checked
CHTTP deferred API; the Web helper does not retry or invent fallback state.

## SSE route

`CHttp::App` adds a thin SSE layer over CHTTP response streaming rather than
a second streaming runtime.

A `GET /events` route should:

1. keep caller-owned `chttp_web_sse_stream` state alive for the response;
2. provide a synchronous bounded `next` callback;
3. initialize the stream with `chttp_web_sse_stream_init()`;
4. commit it with `chttp_web_sse_response()`;
5. release application stream state from the exactly-once close callback.

`app/tests/web/test_chttp_web_sse.c` is the executable protocol reference. It
proves canonical event formatting, newline normalization, the same finite event
sequence over HTTP/1.1 and HTTP/2, and exactly-once cleanup after disconnect.

SSE and WebSocket are independent choices. WebSocket remains a CHTTP server
capability and is not reimplemented by `CHttp::App`.

## Security deployment guidance

Jinja HTML autoescape and HTTP/browser security policy are separate guarantees.
Use both where appropriate.

For browser-facing applications:

- keep credential verification, password hashing, MFA and identity-provider
  integration in the application/integration layer;
- regenerate the Session identifier before publishing authenticated principal
  state;
- validate login return targets as bounded local origin-form targets and never
  reflect Host/Origin into redirects;
- keep authorization in explicit callbacks and treat callback errors as
  fail-closed;
- keep static asset roots/topology immutable while serving them, reject
  symlink/reparse escape, and scope SPA fallback to an explicit namespace;
- install `chttp_web_security_reference_policy()` as a conservative
  same-origin baseline;
- use `chttp_web_security_authenticated_policy()` for authenticated pages
  that should also be private/no-store;
- enable HSTS explicitly only when HTTPS is actually enforced at the browser
  boundary;
- register Web security middleware before CORS if security headers are required
  on CORS preflight responses;
- keep CSP widening explicit, especially `connect-src` for cross-origin
  browser operations;
- apply CSRF validation to session-authenticated mutation routes;
- use CHTTP admission/rate limiting and JWT middleware for the routes that need
  them;
- preserve configured request/response/template/body limits rather than
  replacing them with unbounded buffers;
- never select a filesystem template path directly from request input.
- treat multipart filenames and per-part `Content-Type` values as untrusted
  metadata; they are presentation hints, not authorization or path decisions;
- normalize/sanitize any application-selected persistence name independently
  and never join a client filename directly to a filesystem path;
- keep upload authorization, quota/accounting, persistence, retention, and
  rollback policy in the application;
- antivirus, malware scanning, content disarm/reconstruction, MIME sniffing,
  and domain-specific content inspection are outside CHttp::App and must run in
  the application's staging/commit pipeline where required.

The security reference application is
`app/examples/chttp_web_security_example.c`.

## Renderer ownership and lifecycle

A renderer is synchronous and non-reentrant.

Safe models include:

- one renderer used only on the CHTTP owner thread;
- one renderer owned by one application worker;
- one renderer per parallel worker;
- an externally serialized renderer when the application accepts that
  serialization point.

Do not overlap init, render, or destroy for the same renderer.

The lifecycle qualification in PR #45 covers deterministic rendering,
request isolation, repeated server start/stop/destroy, deferred lifecycle, SSE
cleanup, full regression, AddressSanitizer, and UndefinedBehaviorSanitizer.

## OpenAPI UI qualification application

The OpenAPI documentation UI runs on the generic Web layer and serves as a
real application workload for:

- full-page SSR;
- fragment SSR;
- typed CMeta models;
- hostile-string escaping;
- HTMX interaction;
- browser-only Try-it behavior;
- repeated request isolation;
- CHTTP routing and lifecycle.

OpenAPI-specific UI code should not bypass the generic Web rendering/security
boundary.

## Qualification evidence

The product-readiness gates are tracked by issue #28:

- #40 / PR #45 — correctness, lifecycle, full regression, ASan and UBSan;
- #41 / PR #44 — install/export and standalone installed-package consumer;
- #42 / PR #46 — reproducible renderer/OpenAPI benchmark evidence;
- #43 — reference applications, product documentation, and final claim.

Benchmark evidence records observations without introducing unsupported
performance thresholds.

### Web III browser-authentication evidence

The browser-authenticated application-shell expansion is tracked by #68:

- #69 / PR #75 — Session ID regeneration for privilege transitions;
- #70 / PR #76 — bounded browser principal plus login/logout helpers;
- PR #77 — SaltsUtils component/package ownership and current-master CI
  prerequisite;
- #71 / PR #78 — protected-route authentication/authorization middleware and
  bounded safe return-target semantics;
- #72 / PR #79 — static asset mount with traversal, UTF-8,
  symlink/reparse-root escape, range/conditional, H1/H2, and cache-policy
  qualification;
- #73 / PR #80 — independently runnable authenticated reference application
  with real-client self-test.

The final #74 gate reruns the complete focused Web suite, authenticated
reference smoke, full CTest, ASan, UBSan, exact-head clean-source check, and
the installed-package out-of-tree consumer against current Salts and
SaltsUtils `master` heads.

The installed consumer uses only:

```cmake
find_package(Chttp CONFIG REQUIRED)
target_link_libraries(app PRIVATE CHttp::App)
```

and links representative principal, auth middleware, local-target, and asset
mount public APIs without creating a second DataBind package or source-tree
fallback.

### Web II forms/upload evidence

The forms/upload expansion is tracked by #55:

- #56 / PR #61 — structured validation/error bag;
- #57 / PR #62 — bounded incremental multipart parser;
- #63 / PR #64 — per-request streamed-body context continuity;
- #58 / PR #65 — upload staging transaction, CSRF-safe commit, H1/H2 and
  disconnect/stop cleanup;
- #59 / PR #66 — complete ordinary + HTMX validation/upload reference
  application;
- #60 — final installed-package, sanitizer, regression, documentation, and
  security qualification.

Web II extends the product surface; it does not reopen or weaken the completed
base CHttp::App readiness gate.

## Product boundary

`CHttp::App` is a native-C **server-driven** application layer. It does not
provide:

- a virtual DOM;
- a browser-side state runtime;
- a React/Vue-style client framework;
- a Wt-style server widget tree;
- an ORM;
- a JavaScript build pipeline;
- a second router, session store, or event loop.

Those non-goals keep the package composable: CHTTP remains the HTTP/application
infrastructure, CMeta remains the typed data boundary, and Jinja CMeta remains
the rendering engine.
