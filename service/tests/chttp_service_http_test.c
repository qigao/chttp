#include <chttp_service/service.h>

#include <http_client/http.h>
#include <cmeta_cmeta_fixed_width.h>
#include "chttp_service.http.h"
#include "chttp_service_plugin.http.h"
#include "chttp_service_plugin.service_native.h"
#include "chttp_service_plugin_native.h"
#include "tinytest.h"

#include <salts/clock.h>
#include <salts/thread.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { CHTTP_SERVICE_TEST_TIMEOUT_MS = 5000 };

typedef struct AddRequest {
  uint32_t left;
  uint32_t right;
  uint32_t scale;
  uint8_t presence;
} AddRequest;

typedef struct AddResponse {
  uint32_t sum;
} AddResponse;

static const cmeta_type_identity ADD_REQUEST_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.AddRequest");
static const cmeta_type_identity ADD_RESPONSE_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.AddResponse");
static const cmeta_type_traits ADD_REQUEST_TRAITS = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_traits ADD_RESPONSE_TRAITS = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};


static const cmeta_type_desc ADD_REQUEST_TYPE = {
    "AddRequest", sizeof(AddRequest), _Alignof(AddRequest),
    CMETA_T_OBJECT, NULL, &ADD_REQUEST_TRAITS, &ADD_REQUEST_ID};
static const cmeta_type_desc ADD_RESPONSE_TYPE = {
    "AddResponse", sizeof(AddResponse), _Alignof(AddResponse),
    CMETA_T_OBJECT, NULL, &ADD_RESPONSE_TRAITS, &ADD_RESPONSE_ID};

static const cmeta_type_desc ADD_REQUEST_PTR_TYPE = {
    "const AddRequest *", sizeof(AddRequest *), _Alignof(AddRequest *),
    CMETA_T_POINTER, &ADD_REQUEST_TYPE, NULL, NULL};
static const cmeta_type_desc ADD_RESPONSE_PTR_TYPE = {
    "AddResponse *", sizeof(AddResponse *), _Alignof(AddResponse *),
    CMETA_T_POINTER, &ADD_RESPONSE_TYPE, NULL, NULL};

static const cmeta_field_desc ADD_REQUEST_LAYOUT_FIELDS[] = {
    {"left", "uint32_t", offsetof(AddRequest, left), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL},
    {"right", "uint32_t", offsetof(AddRequest, right), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL},
    {"scale", "uint32_t", offsetof(AddRequest, scale), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL}};
static const cmeta_struct_desc ADD_REQUEST_LAYOUT = {
    "AddRequest", sizeof(AddRequest), _Alignof(AddRequest),
    ADD_REQUEST_LAYOUT_FIELDS, 3u};
static const cmeta_data_field_desc ADD_REQUEST_FIELDS[] = {
    {"test.chttp.AddRequest.left", "left", offsetof(AddRequest, left),
     &cmeta_data_uint32},
    {"test.chttp.AddRequest.right", "right", offsetof(AddRequest, right),
     &cmeta_data_uint32},
    {"test.chttp.AddRequest.scale", "scale", offsetof(AddRequest, scale),
     &cmeta_data_uint32}};
static const cmeta_data_struct_shape ADD_REQUEST_SHAPE = {
    &ADD_REQUEST_LAYOUT, ADD_REQUEST_FIELDS, 3u};
static const cmeta_data_desc ADD_REQUEST_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.AddRequest.data",
    .display_name = "AddRequest",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &ADD_REQUEST_TYPE,
    .shape = &ADD_REQUEST_SHAPE};

static const cmeta_field_desc ADD_RESPONSE_LAYOUT_FIELDS[] = {
    {"sum", "uint32_t", offsetof(AddResponse, sum), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL}};
static const cmeta_struct_desc ADD_RESPONSE_LAYOUT = {
    "AddResponse", sizeof(AddResponse), _Alignof(AddResponse),
    ADD_RESPONSE_LAYOUT_FIELDS, 1u};
static const cmeta_data_field_desc ADD_RESPONSE_FIELDS[] = {
    {"test.chttp.AddResponse.sum", "sum", offsetof(AddResponse, sum),
     &cmeta_data_uint32}};
static const cmeta_data_struct_shape ADD_RESPONSE_SHAPE = {
    &ADD_RESPONSE_LAYOUT, ADD_RESPONSE_FIELDS, 1u};
static const cmeta_data_desc ADD_RESPONSE_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.AddResponse.data",
    .display_name = "AddResponse",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &ADD_RESPONSE_TYPE,
    .shape = &ADD_RESPONSE_SHAPE};

static const DataBindNativeStateBinding ADD_REQUEST_PRESENCE[] = {
    {sizeof(DataBindNativeStateBinding), "scale",
     offsetof(AddRequest, presence), 0u}};

static const DataBindNativeTypeBinding ADD_REQUEST_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_NATIVE_BINDING_ABI_VERSION,
    .idl_type_name = "AddRequest",
    .data = &ADD_REQUEST_DATA,
    .presence = ADD_REQUEST_PRESENCE,
    .presence_count = 1u};

static const DataBindNativeTypeBinding ADD_RESPONSE_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_NATIVE_BINDING_ABI_VERSION,
    .idl_type_name = "AddResponse",
    .data = &ADD_RESPONSE_DATA};

static const DataBindNativeTypeBinding ADD_REQUEST_CFLOW_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_NATIVE_BINDING_ABI_VERSION,
    .idl_type_name = "AddRequest",
    .data = &ADD_REQUEST_DATA};


FunctionDeclAsAbiResult(
    value, int, &cmeta_type_int, CMETA_ABI_SCALAR,
    CMETA_RESULT_VALUE, chttp_service_test_add,
    (const AddRequest *, request,
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &ADD_REQUEST_PTR_TYPE, CMETA_ABI_OBJECT_POINTER),
    (AddResponse *, response,
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,
     &ADD_RESPONSE_PTR_TYPE, CMETA_ABI_OBJECT_POINTER));

static size_t CHTTP_SERVICE_TEST_ADD_CALLS;

int chttp_service_test_add(const AddRequest *request, AddResponse *response) {
  if (request == NULL || response == NULL) return -1;
  ++CHTTP_SERVICE_TEST_ADD_CALLS;
  if (request->left == 99u) return 7;
  response->sum = request->left + request->right * request->scale;
  return 0;
}

static bool DATA_BIND_NATIVE_CALL chttp_service_test_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  int result;
  (void)context;
  if (return_storage == NULL || params == NULL || param_count != 2u ||
      params[0] == NULL || params[1] == NULL)
    return false;
  result = chttp_service_test_add(
      (const AddRequest *)params[0], (AddResponse *)params[1]);
  *(int *)return_storage = result;
  return true;
}

static bool chttp_service_test_cflow_invoke(
    const cmeta_callable *self, void *out,
    const void *const *args) {
  AddResponse *response = (AddResponse *)out;
  (void)self;
  if (response == NULL || args == NULL || args[0] == NULL)
    return false;
  memset(response, 0, sizeof(*response));
  return chttp_service_test_add(
             (const AddRequest *)args[0], response) == 0;
}

static cflow_function_projection_status
chttp_service_test_cflow_projection(
    cflow_function_typed_adapter_projection *out) {
  cmeta_callable adapter = {0};
  adapter.meta.sig = CMETA_SIG_INVALID;
  adapter.meta.effects = FunctionMeta(chttp_service_test_add)->effects;
  adapter.meta.properties = FunctionMeta(chttp_service_test_add)->properties;
  adapter.invoke = chttp_service_test_cflow_invoke;
  adapter.dispatch = CMETA_CALLABLE_DISPATCH_ADAPTER;
  return cflow_function_typed_adapter_projection_admit(
      FunctionMeta(chttp_service_test_add),
      FunctionAbi(chttp_service_test_add),
      adapter,
      &ADD_REQUEST_TYPE,
      &ADD_RESPONSE_TYPE,
      out);
}


FunctionDeclAsAbiResult(
    value, int, &cmeta_type_int, CMETA_ABI_SCALAR,
    CMETA_RESULT_VALUE, chttp_service_test_other,
    (const AddRequest *, request,
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &ADD_REQUEST_PTR_TYPE, CMETA_ABI_OBJECT_POINTER),
    (AddResponse *, response,
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,
     &ADD_RESPONSE_PTR_TYPE, CMETA_ABI_OBJECT_POINTER));

int chttp_service_test_other(const AddRequest *request, AddResponse *response) {
  return chttp_service_test_add(request, response);
}

static bool DATA_BIND_NATIVE_CALL chttp_service_test_other_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  int result;
  (void)context;
  if (return_storage == NULL || params == NULL || param_count != 2u ||
      params[0] == NULL || params[1] == NULL)
    return false;
  result = chttp_service_test_other(
      (const AddRequest *)params[0], (AddResponse *)params[1]);
  *(int *)return_storage = result;
  return true;
}



typedef struct CHttpServiceOwnedString {
  unsigned char *data;
  size_t size;
} CHttpServiceOwnedString;

typedef struct CHttpServiceOwnedError {
  CHttpServiceOwnedString detail;
} CHttpServiceOwnedError;

typedef struct CHttpServiceOwnedErrorEnvelope {
  uint32_t kind;
  union {
    CHttpServiceOwnedError error_1;
  } payload;
} CHttpServiceOwnedErrorEnvelope;

static _Atomic int CHTTP_SERVICE_OWNED_ERROR_RELEASES;

static const cmeta_type_identity CHTTP_SERVICE_OWNED_STRING_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.OwnedString");
static const cmeta_type_desc CHTTP_SERVICE_OWNED_STRING_TYPE = {
    "CHttpServiceOwnedString",
    sizeof(CHttpServiceOwnedString),
    _Alignof(CHttpServiceOwnedString),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &CHTTP_SERVICE_OWNED_STRING_ID};
static const cmeta_data_buffer_shape CHTTP_SERVICE_OWNED_STRING_SHAPE = {
    CMETA_DATA_BUFFER_OWNED};

static bool chttp_service_owned_string_is_zero(const void *object) {
  const CHttpServiceOwnedString *value =
      (const CHttpServiceOwnedString *)object;
  return value != NULL && value->data == NULL && value->size == 0u;
}

static cmeta_status chttp_service_owned_string_init_zero(void *object) {
  CHttpServiceOwnedString *value =
      (CHttpServiceOwnedString *)object;
  if (value == NULL) return CMETA_INVALID_ARGUMENT;
  value->data = NULL;
  value->size = 0u;
  return CMETA_OK;
}

static cmeta_status chttp_service_owned_string_assign(
    void *object, const unsigned char *data, size_t size, size_t max_bytes) {
  CHttpServiceOwnedString *value =
      (CHttpServiceOwnedString *)object;
  unsigned char *copy = NULL;
  if (value == NULL || (size != 0u && data == NULL) ||
      size > max_bytes || !chttp_service_owned_string_is_zero(value))
    return size > max_bytes
               ? CMETA_CAPACITY_EXCEEDED
               : CMETA_INVALID_ARGUMENT;
  if (size != 0u) {
    copy = (unsigned char *)malloc(size);
    if (copy == NULL) return CMETA_OUT_OF_MEMORY;
    memcpy(copy, data, size);
  }
  value->data = copy;
  value->size = size;
  return CMETA_OK;
}

static void chttp_service_owned_string_restore_zero(void *object) {
  CHttpServiceOwnedString *value =
      (CHttpServiceOwnedString *)object;
  if (value == NULL) return;
  if (value->data != NULL) {
    free(value->data);
    atomic_fetch_add_explicit(
        &CHTTP_SERVICE_OWNED_ERROR_RELEASES, 1, memory_order_relaxed);
  }
  value->data = NULL;
  value->size = 0u;
}

static cmeta_status chttp_service_owned_string_read(
    const void *object, const unsigned char **out_data, size_t *out_size) {
  const CHttpServiceOwnedString *value =
      (const CHttpServiceOwnedString *)object;
  if (value == NULL || out_data == NULL || out_size == NULL)
    return CMETA_INVALID_ARGUMENT;
  *out_data = value->data;
  *out_size = value->size;
  return value->size != 0u && value->data == NULL
             ? CMETA_CALLBACK_ERROR
             : CMETA_OK;
}

static void chttp_service_owned_string_move(
    void *destination, void *source) {
  CHttpServiceOwnedString *dst =
      (CHttpServiceOwnedString *)destination;
  CHttpServiceOwnedString *src =
      (CHttpServiceOwnedString *)source;
  if (dst == NULL || src == NULL) return;
  dst->data = src->data;
  dst->size = src->size;
  src->data = NULL;
  src->size = 0u;
}

static const cmeta_data_buffer_ops CHTTP_SERVICE_OWNED_STRING_OPS = {
    sizeof(cmeta_data_buffer_ops),
    CMETA_DATA_BUFFER_OPS_ABI_VERSION,
    &CHTTP_SERVICE_OWNED_STRING_TYPE,
    CMETA_DATA_BUFFER_OWNED,
    chttp_service_owned_string_is_zero,
    chttp_service_owned_string_assign,
    chttp_service_owned_string_restore_zero,
    chttp_service_owned_string_read,
    chttp_service_owned_string_init_zero,
    chttp_service_owned_string_move};

static const cmeta_data_desc CHTTP_SERVICE_OWNED_STRING_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.OwnedString.data",
    .display_name = "CHttpServiceOwnedString",
    .kind = CMETA_DATA_STRING,
    .storage_type = &CHTTP_SERVICE_OWNED_STRING_TYPE,
    .shape = &CHTTP_SERVICE_OWNED_STRING_SHAPE,
    .buffer_ops = &CHTTP_SERVICE_OWNED_STRING_OPS};

static const cmeta_type_identity CHTTP_SERVICE_OWNED_ERROR_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.OwnedError");
static const cmeta_type_desc CHTTP_SERVICE_OWNED_ERROR_TYPE = {
    "CHttpServiceOwnedError",
    sizeof(CHttpServiceOwnedError),
    _Alignof(CHttpServiceOwnedError),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &CHTTP_SERVICE_OWNED_ERROR_ID};
static const cmeta_field_desc CHTTP_SERVICE_OWNED_ERROR_LAYOUT_FIELDS[] = {
    {"detail", "CHttpServiceOwnedString",
     offsetof(CHttpServiceOwnedError, detail),
     sizeof(CHttpServiceOwnedString),
     _Alignof(CHttpServiceOwnedString),
     &CHTTP_SERVICE_OWNED_STRING_TYPE,
     NULL}};
static const cmeta_struct_desc CHTTP_SERVICE_OWNED_ERROR_LAYOUT = {
    "CHttpServiceOwnedError",
    sizeof(CHttpServiceOwnedError),
    _Alignof(CHttpServiceOwnedError),
    CHTTP_SERVICE_OWNED_ERROR_LAYOUT_FIELDS,
    1u};
static const cmeta_data_field_desc CHTTP_SERVICE_OWNED_ERROR_FIELDS[] = {
    {"test.chttp.OwnedError.detail",
     "detail",
     offsetof(CHttpServiceOwnedError, detail),
     &CHTTP_SERVICE_OWNED_STRING_DATA}};
static const cmeta_data_struct_shape CHTTP_SERVICE_OWNED_ERROR_SHAPE = {
    &CHTTP_SERVICE_OWNED_ERROR_LAYOUT,
    CHTTP_SERVICE_OWNED_ERROR_FIELDS,
    1u};
static const cmeta_data_desc CHTTP_SERVICE_OWNED_ERROR_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.OwnedError.data",
    .display_name = "CHttpServiceOwnedError",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &CHTTP_SERVICE_OWNED_ERROR_TYPE,
    .shape = &CHTTP_SERVICE_OWNED_ERROR_SHAPE};

static const cmeta_type_identity CHTTP_SERVICE_OWNED_ERROR_ENVELOPE_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.OwnedErrorEnvelope");
static const cmeta_type_desc CHTTP_SERVICE_OWNED_ERROR_ENVELOPE_TYPE = {
    "CHttpServiceOwnedErrorEnvelope",
    sizeof(CHttpServiceOwnedErrorEnvelope),
    _Alignof(CHttpServiceOwnedErrorEnvelope),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &CHTTP_SERVICE_OWNED_ERROR_ENVELOPE_ID};
static const cmeta_type_desc CHTTP_SERVICE_OWNED_ERROR_ENVELOPE_PTR_TYPE = {
    "CHttpServiceOwnedErrorEnvelope *",
    sizeof(CHttpServiceOwnedErrorEnvelope *),
    _Alignof(CHttpServiceOwnedErrorEnvelope *),
    CMETA_T_POINTER,
    &CHTTP_SERVICE_OWNED_ERROR_ENVELOPE_TYPE,
    NULL,
    NULL};

FunctionDeclAsAbiResult(
    value, int, &cmeta_type_int, CMETA_ABI_SCALAR,
    CMETA_RESULT_VALUE, chttp_service_test_owned_error,
    (const AddRequest *, request,
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &ADD_REQUEST_PTR_TYPE, CMETA_ABI_OBJECT_POINTER),
    (AddResponse *, response,
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,
     &ADD_RESPONSE_PTR_TYPE, CMETA_ABI_OBJECT_POINTER),
    (CHttpServiceOwnedErrorEnvelope *, error,
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,
     &CHTTP_SERVICE_OWNED_ERROR_ENVELOPE_PTR_TYPE,
     CMETA_ABI_OBJECT_POINTER));

int chttp_service_test_owned_error(
    const AddRequest *request, AddResponse *response,
    CHttpServiceOwnedErrorEnvelope *error) {
  static const unsigned char detail[] = "owned";
  if (request == NULL || response == NULL || error == NULL) return -1;
  if (request->left == 77u) {
    if (chttp_service_owned_string_assign(
            &error->payload.error_1.detail,
            detail, sizeof(detail) - 1u, 16u) != CMETA_OK)
      return -1;
    error->kind = 1u;
    return 0;
  }
  response->sum = request->left + request->right * request->scale;
  return 0;
}

static bool DATA_BIND_NATIVE_CALL chttp_service_test_owned_error_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  int result;
  (void)context;
  if (return_storage == NULL || params == NULL || param_count != 3u ||
      params[0] == NULL || params[1] == NULL || params[2] == NULL)
    return false;
  result = chttp_service_test_owned_error(
      (const AddRequest *)params[0],
      (AddResponse *)params[1],
      (CHttpServiceOwnedErrorEnvelope *)params[2]);
  *(int *)return_storage = result;
  return true;
}

static DataBindStatus chttp_service_owned_error_resolve(
    const cmeta_data_desc **out, DataBindError *error) {
  (void)error;
  if (out == NULL) return DATA_BIND_ERR_INVALID_ARG;
  *out = &CHTTP_SERVICE_OWNED_ERROR_DATA;
  return DATA_BIND_OK;
}

static const DataBindNativeErrorBinding
    CHTTP_SERVICE_OWNED_ERROR_BINDINGS[] = {
        {sizeof(DataBindNativeErrorBinding),
         "OwnedError",
         1u,
         chttp_service_owned_error_resolve,
         offsetof(CHttpServiceOwnedErrorEnvelope, payload.error_1)}};


typedef struct CHttpServiceRollbackBuffer {
  size_t size;
  unsigned char bytes[16];
} CHttpServiceRollbackBuffer;

typedef struct CHttpServiceRollbackRequest {
  CHttpServiceRollbackBuffer first;
  CHttpServiceRollbackBuffer second;
} CHttpServiceRollbackRequest;

static _Atomic int CHTTP_SERVICE_ROLLBACK_FIRST_RESTORES;
static _Atomic int CHTTP_SERVICE_ROLLBACK_SECOND_RESTORES;
static _Atomic int CHTTP_SERVICE_ROLLBACK_CALLS;

static bool chttp_service_rollback_buffer_is_zero(const void *object) {
  const CHttpServiceRollbackBuffer *value =
      (const CHttpServiceRollbackBuffer *)object;
  return value != NULL && value->size == 0u;
}

static cmeta_status chttp_service_rollback_buffer_assign(
    void *object, const unsigned char *data, size_t size, size_t max_bytes) {
  CHttpServiceRollbackBuffer *value =
      (CHttpServiceRollbackBuffer *)object;
  if (value == NULL || (data == NULL && size != 0u))
    return CMETA_INVALID_ARGUMENT;
  if (size > sizeof(value->bytes) || size > max_bytes)
    return CMETA_CAPACITY_EXCEEDED;
  if (size != 0u) memcpy(value->bytes, data, size);
  value->size = size;
  return CMETA_OK;
}

static cmeta_status chttp_service_rollback_buffer_read(
    const void *object, const unsigned char **out_data, size_t *out_size) {
  const CHttpServiceRollbackBuffer *value =
      (const CHttpServiceRollbackBuffer *)object;
  if (value == NULL || out_data == NULL || out_size == NULL)
    return CMETA_INVALID_ARGUMENT;
  *out_data = value->bytes;
  *out_size = value->size;
  return CMETA_OK;
}

static void chttp_service_rollback_buffer_move(
    void *destination, void *source) {
  CHttpServiceRollbackBuffer *dst =
      (CHttpServiceRollbackBuffer *)destination;
  CHttpServiceRollbackBuffer *src =
      (CHttpServiceRollbackBuffer *)source;
  if (dst == NULL || src == NULL) return;
  *dst = *src;
  memset(src, 0, sizeof(*src));
}

static cmeta_status chttp_service_rollback_first_init(void *object) {
  CHttpServiceRollbackBuffer *value =
      (CHttpServiceRollbackBuffer *)object;
  if (value == NULL) return CMETA_INVALID_ARGUMENT;
  memset(value, 0, sizeof(*value));
  return CMETA_OK;
}

static cmeta_status chttp_service_rollback_second_init(void *object) {
  CHttpServiceRollbackBuffer *value =
      (CHttpServiceRollbackBuffer *)object;
  if (value == NULL) return CMETA_INVALID_ARGUMENT;
  memset(value, 0, sizeof(*value));
  return CMETA_CALLBACK_ERROR;
}

static void chttp_service_rollback_first_restore(void *object) {
  CHttpServiceRollbackBuffer *value =
      (CHttpServiceRollbackBuffer *)object;
  if (value == NULL) return;
  atomic_fetch_add_explicit(
      &CHTTP_SERVICE_ROLLBACK_FIRST_RESTORES, 1, memory_order_relaxed);
  memset(value, 0, sizeof(*value));
}

static void chttp_service_rollback_second_restore(void *object) {
  CHttpServiceRollbackBuffer *value =
      (CHttpServiceRollbackBuffer *)object;
  if (value == NULL) return;
  atomic_fetch_add_explicit(
      &CHTTP_SERVICE_ROLLBACK_SECOND_RESTORES, 1, memory_order_relaxed);
  memset(value, 0, sizeof(*value));
}

static const cmeta_type_identity CHTTP_SERVICE_ROLLBACK_BUFFER_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.RollbackBuffer");
static const cmeta_type_desc CHTTP_SERVICE_ROLLBACK_BUFFER_TYPE = {
    "CHttpServiceRollbackBuffer",
    sizeof(CHttpServiceRollbackBuffer),
    _Alignof(CHttpServiceRollbackBuffer),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &CHTTP_SERVICE_ROLLBACK_BUFFER_ID};
static const cmeta_data_buffer_shape CHTTP_SERVICE_ROLLBACK_BUFFER_SHAPE = {
    CMETA_DATA_BUFFER_OWNED};

#define CHTTP_SERVICE_ROLLBACK_OPS(NAME, INIT, RESTORE)                       \
  static const cmeta_data_buffer_ops NAME = {                                 \
      sizeof(cmeta_data_buffer_ops), CMETA_DATA_BUFFER_OPS_ABI_VERSION,       \
      &CHTTP_SERVICE_ROLLBACK_BUFFER_TYPE, CMETA_DATA_BUFFER_OWNED,           \
      chttp_service_rollback_buffer_is_zero,                                  \
      chttp_service_rollback_buffer_assign, RESTORE,                          \
      chttp_service_rollback_buffer_read, INIT,                               \
      chttp_service_rollback_buffer_move}

CHTTP_SERVICE_ROLLBACK_OPS(
    CHTTP_SERVICE_ROLLBACK_FIRST_OPS,
    chttp_service_rollback_first_init,
    chttp_service_rollback_first_restore);
CHTTP_SERVICE_ROLLBACK_OPS(
    CHTTP_SERVICE_ROLLBACK_SECOND_OPS,
    chttp_service_rollback_second_init,
    chttp_service_rollback_second_restore);

#undef CHTTP_SERVICE_ROLLBACK_OPS

static const cmeta_data_desc CHTTP_SERVICE_ROLLBACK_FIRST_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.RollbackRequest.first",
    .display_name = "RollbackFirst",
    .kind = CMETA_DATA_BYTES,
    .storage_type = &CHTTP_SERVICE_ROLLBACK_BUFFER_TYPE,
    .shape = &CHTTP_SERVICE_ROLLBACK_BUFFER_SHAPE,
    .buffer_ops = &CHTTP_SERVICE_ROLLBACK_FIRST_OPS};
static const cmeta_data_desc CHTTP_SERVICE_ROLLBACK_SECOND_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.RollbackRequest.second",
    .display_name = "RollbackSecond",
    .kind = CMETA_DATA_BYTES,
    .storage_type = &CHTTP_SERVICE_ROLLBACK_BUFFER_TYPE,
    .shape = &CHTTP_SERVICE_ROLLBACK_BUFFER_SHAPE,
    .buffer_ops = &CHTTP_SERVICE_ROLLBACK_SECOND_OPS};

static const cmeta_type_identity CHTTP_SERVICE_ROLLBACK_REQUEST_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.chttp.RollbackRequest");
static const cmeta_type_desc CHTTP_SERVICE_ROLLBACK_REQUEST_TYPE = {
    "CHttpServiceRollbackRequest",
    sizeof(CHttpServiceRollbackRequest),
    _Alignof(CHttpServiceRollbackRequest),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &CHTTP_SERVICE_ROLLBACK_REQUEST_ID};
static const cmeta_type_desc CHTTP_SERVICE_ROLLBACK_REQUEST_PTR_TYPE = {
    "const CHttpServiceRollbackRequest *",
    sizeof(CHttpServiceRollbackRequest *),
    _Alignof(CHttpServiceRollbackRequest *),
    CMETA_T_POINTER,
    &CHTTP_SERVICE_ROLLBACK_REQUEST_TYPE,
    NULL,
    NULL};

static const cmeta_field_desc CHTTP_SERVICE_ROLLBACK_LAYOUT_FIELDS[] = {
    {"first", "bytes", offsetof(CHttpServiceRollbackRequest, first),
     sizeof(CHttpServiceRollbackBuffer),
     _Alignof(CHttpServiceRollbackBuffer),
     &CHTTP_SERVICE_ROLLBACK_BUFFER_TYPE, NULL},
    {"second", "bytes", offsetof(CHttpServiceRollbackRequest, second),
     sizeof(CHttpServiceRollbackBuffer),
     _Alignof(CHttpServiceRollbackBuffer),
     &CHTTP_SERVICE_ROLLBACK_BUFFER_TYPE, NULL}};
static const cmeta_struct_desc CHTTP_SERVICE_ROLLBACK_LAYOUT = {
    "CHttpServiceRollbackRequest",
    sizeof(CHttpServiceRollbackRequest),
    _Alignof(CHttpServiceRollbackRequest),
    CHTTP_SERVICE_ROLLBACK_LAYOUT_FIELDS,
    2u};
static const cmeta_data_field_desc CHTTP_SERVICE_ROLLBACK_FIELDS[] = {
    {"test.chttp.RollbackRequest.first", "first",
     offsetof(CHttpServiceRollbackRequest, first),
     &CHTTP_SERVICE_ROLLBACK_FIRST_DATA},
    {"test.chttp.RollbackRequest.second", "second",
     offsetof(CHttpServiceRollbackRequest, second),
     &CHTTP_SERVICE_ROLLBACK_SECOND_DATA}};
static const cmeta_data_struct_shape CHTTP_SERVICE_ROLLBACK_SHAPE = {
    &CHTTP_SERVICE_ROLLBACK_LAYOUT,
    CHTTP_SERVICE_ROLLBACK_FIELDS,
    2u};
static const cmeta_data_desc CHTTP_SERVICE_ROLLBACK_REQUEST_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.chttp.RollbackRequest.data",
    .display_name = "CHttpServiceRollbackRequest",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &CHTTP_SERVICE_ROLLBACK_REQUEST_TYPE,
    .shape = &CHTTP_SERVICE_ROLLBACK_SHAPE};
static const DataBindNativeTypeBinding CHTTP_SERVICE_ROLLBACK_REQUEST_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_NATIVE_BINDING_ABI_VERSION,
    .idl_type_name = "RollbackRequest",
    .data = &CHTTP_SERVICE_ROLLBACK_REQUEST_DATA};

FunctionDeclAsAbiResult(
    value, int, &cmeta_type_int, CMETA_ABI_SCALAR,
    CMETA_RESULT_VALUE, chttp_service_test_rollback,
    (const CHttpServiceRollbackRequest *, request,
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &CHTTP_SERVICE_ROLLBACK_REQUEST_PTR_TYPE, CMETA_ABI_OBJECT_POINTER),
    (AddResponse *, response,
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,
     &ADD_RESPONSE_PTR_TYPE, CMETA_ABI_OBJECT_POINTER));

int chttp_service_test_rollback(
    const CHttpServiceRollbackRequest *request, AddResponse *response) {
  (void)request;
  (void)response;
  atomic_fetch_add_explicit(
      &CHTTP_SERVICE_ROLLBACK_CALLS, 1, memory_order_relaxed);
  return 0;
}

static bool DATA_BIND_NATIVE_CALL chttp_service_test_rollback_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  int result;
  (void)context;
  if (return_storage == NULL || params == NULL || param_count != 2u ||
      params[0] == NULL || params[1] == NULL)
    return false;
  result = chttp_service_test_rollback(
      (const CHttpServiceRollbackRequest *)params[0],
      (AddResponse *)params[1]);
  *(int *)return_storage = result;
  return true;
}

static const DataBindHttpFieldProjection CHTTP_SERVICE_ROLLBACK_FIELDS_HTTP[] = {
    {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_INGRESS,
     "first", DATA_BIND_HTTP_QUERY, "first", SIZE_MAX},
    {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_INGRESS,
     "second", DATA_BIND_HTTP_QUERY, "second", SIZE_MAX},
    {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_EGRESS,
     "sum", DATA_BIND_HTTP_RESPONSE_BODY, "sum", SIZE_MAX}};

static const DataBindHttpProjectionConfig CHTTP_SERVICE_ROLLBACK_HTTP = {
    sizeof(DataBindHttpProjectionConfig),
    DATA_BIND_METHOD_PLAN_ABI_VERSION,
    "GET",
    "/rollback",
    200,
    DATA_BIND_HTTP_CONTEXT_NONE,
    CHTTP_SERVICE_ROLLBACK_FIELDS_HTTP,
    sizeof(CHTTP_SERVICE_ROLLBACK_FIELDS_HTTP) /
        sizeof(CHTTP_SERVICE_ROLLBACK_FIELDS_HTTP[0]),
    NULL,
    0u,
    DATA_BIND_FORMAT_JSON,
    DATA_BIND_FORMAT_JSON};

static native_io_backend_kind chttp_service_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config chttp_service_test_network(size_t connections) {
  const cnet_client_config config = {
      .backend = chttp_service_test_backend(),
      .connection_capacity = connections,
      .command_capacity = 16u,
      .request_capacity = 8u,
      .completion_batch_capacity = 8u,
      .event_capacity = 16u,
      .max_send_bytes = 4096u,
      .receive_buffer_bytes = 256u,
      .connect_timeout_ms = CHTTP_SERVICE_TEST_TIMEOUT_MS,
      .read_timeout_ms = CHTTP_SERVICE_TEST_TIMEOUT_MS,
      .write_timeout_ms = CHTTP_SERVICE_TEST_TIMEOUT_MS};
  return config;
}

static chttp_server_config chttp_service_test_server_config(void) {
  const chttp_server_config config = {
      .host = "127.0.0.1",
      .port = 0u,
      .backlog = 8u,
      .network = chttp_service_test_network(4u),
      .route_capacity = 4u,
      .middleware_capacity = 2u,
      .max_route_middleware_count = 2u,
      .max_route_param_count = 4u,
      .max_route_param_bytes = 128u,
      .max_target_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 1024u,
      .max_request_body_bytes = 512u,
      .max_response_header_count = 16u,
      .max_response_header_bytes = 1024u,
      .max_response_body_bytes = 512u,
      .poll_slice_ms = 2u};
  return config;
}

static chttp_client_config chttp_service_test_client_config(void) {
  const chttp_client_config config = {
      .network = chttp_service_test_network(2u),
      .request_capacity = 1u,
      .max_start_line_bytes = 256u,
      .max_header_count = 16u,
      .max_header_bytes = 1024u,
      .max_request_body_bytes = 512u,
      .max_response_body_bytes = 512u,
      .max_informational_responses = 2u};
  return config;
}

static int chttp_service_test_call(
    chttp_client *client, const char *uri, const char *target,
    chttp_response *response);

typedef struct chttp_service_executor_gate {
  _Atomic int started;
  _Atomic int release;
} chttp_service_executor_gate;

typedef struct chttp_service_request_thread_args {
  chttp_client_config config;
  char uri[64];
  const char *target;
  _Atomic int completed;
  int status;
  unsigned int http_status;
} chttp_service_request_thread_args;

static void chttp_service_request_thread(void *user) {
  chttp_service_request_thread_args *args =
      (chttp_service_request_thread_args *)user;
  chttp_client client = {0};
  chttp_response response = {0};
  if (args == NULL) return;
  args->status = chttp_client_init(&client, &args->config);
  if (args->status == SALTS_OK) {
    args->status =
        chttp_service_test_call(&client, args->uri, args->target, &response);
    if (args->status == SALTS_OK)
      args->http_status = response.status_code;
    chttp_response_destroy(&response);
    if (chttp_client_destroy(&client, CHTTP_SERVICE_TEST_TIMEOUT_MS) !=
            SALTS_OK &&
        args->status == SALTS_OK)
      args->status = SALTS_EIO;
  }
  atomic_store_explicit(&args->completed, 1, memory_order_release);
}

static bool DATA_BIND_NATIVE_CALL chttp_service_test_blocking_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  chttp_service_executor_gate *gate =
      (chttp_service_executor_gate *)context;
  if (gate == NULL) return false;
  atomic_store_explicit(&gate->started, 1, memory_order_release);
  while (atomic_load_explicit(&gate->release, memory_order_acquire) == 0)
    cmeta_thread_yield();
  return chttp_service_test_invoke(
      NULL, return_storage, params, param_count);
}

static void chttp_service_executor_gate_run(void *user) {
  chttp_service_executor_gate *gate =
      (chttp_service_executor_gate *)user;
  if (gate == NULL) return;
  atomic_store_explicit(&gate->started, 1, memory_order_release);
  while (atomic_load_explicit(&gate->release, memory_order_acquire) == 0)
    cmeta_thread_yield();
}

static void chttp_service_executor_noop(void *user) {
  (void)user;
}

static const DataBindHttpFieldProjection CHTTP_SERVICE_CFLOW_FIELDS[] = {
    {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_INGRESS,
     "left", DATA_BIND_HTTP_PATH, "left", SIZE_MAX},
    {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_INGRESS,
     "right", DATA_BIND_HTTP_QUERY, "right", SIZE_MAX},
    {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_INGRESS,
     "scale", DATA_BIND_HTTP_QUERY, "scale", SIZE_MAX},
    {sizeof(DataBindHttpFieldProjection), DATA_BIND_BINDING_EGRESS,
     "sum", DATA_BIND_HTTP_RESPONSE_BODY, "sum", SIZE_MAX}};

static const DataBindHttpProjectionConfig CHTTP_SERVICE_CFLOW_HTTP = {
    sizeof(DataBindHttpProjectionConfig),
    DATA_BIND_METHOD_PLAN_ABI_VERSION,
    "GET",
    "/flow/{left}",
    201,
    DATA_BIND_HTTP_CONTEXT_NONE,
    CHTTP_SERVICE_CFLOW_FIELDS,
    sizeof(CHTTP_SERVICE_CFLOW_FIELDS) /
        sizeof(CHTTP_SERVICE_CFLOW_FIELDS[0]),
    NULL,
    0u,
    DATA_BIND_FORMAT_JSON,
    DATA_BIND_FORMAT_JSON};

static const DataBindHttpErrorMapping
    CHTTP_SERVICE_OWNED_ERROR_MAPPINGS[] = {
        {sizeof(DataBindHttpErrorMapping), "OwnedError", 409}};

static const DataBindHttpProjectionConfig CHTTP_SERVICE_OWNED_ERROR_HTTP = {
    sizeof(DataBindHttpProjectionConfig),
    DATA_BIND_METHOD_PLAN_ABI_VERSION,
    "GET",
    "/owned-error/{left}",
    201,
    DATA_BIND_HTTP_CONTEXT_NONE,
    CHTTP_SERVICE_CFLOW_FIELDS,
    sizeof(CHTTP_SERVICE_CFLOW_FIELDS) /
        sizeof(CHTTP_SERVICE_CFLOW_FIELDS[0]),
    CHTTP_SERVICE_OWNED_ERROR_MAPPINGS,
    sizeof(CHTTP_SERVICE_OWNED_ERROR_MAPPINGS) /
        sizeof(CHTTP_SERVICE_OWNED_ERROR_MAPPINGS[0]),
    DATA_BIND_FORMAT_JSON,
    DATA_BIND_FORMAT_JSON};

static int chttp_service_test_call(
    chttp_client *client, const char *uri, const char *target,
    chttp_response *response) {
  const chttp_options options = {
      .connection_uri = uri,
      .authority = "127.0.0.1",
      .target = target,
      .timeout_ms = CHTTP_SERVICE_TEST_TIMEOUT_MS};
  chttp_error error = {0};
  return chttp_get(client, &options, response, &error);
}

spec("CHttp::Service generated HTTP MethodPlan") {
  it("mounts generated transport projection without IDL HTTP annotations") {
    static const char schema[] =
        "message AddRequest {"
        " @Min(1) uint32 left;"
        " uint32 right;"
        " optional uint32 scale default 1;"
        "}"
        "message AddResponse { uint32 sum; }"
        "service Calc { Add: AddRequest -> AddResponse; }";
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindServiceNativeBinding native =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(
            FunctionMeta(chttp_service_test_add),
            &ADD_REQUEST_NATIVE, &ADD_RESPONSE_NATIVE);
    const DataBindHttpProjectionConfig *projection;
    DataBindHttpMethodPlan *method_plan = NULL;
    const DataBindBindingPlan *binding = NULL;
    DataBindBindingPlanEntry entry = DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    DataBindHttpMethodPlan *unknown_result_plan = NULL;
    DataBindHttpMethodPlan *unborrowed_request_plan = NULL;
    cmeta_function_desc unknown_result_function = {0};
    cmeta_function_abi_desc unknown_result_abi = {0};
    DataBindServiceNativeBinding unknown_result_native = {0};
    DataBindNativeExecution unknown_result_execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    chttp_service_http_mount unknown_result_mount =
        CHTTP_SERVICE_HTTP_MOUNT_INIT;
    cmeta_param_desc unborrowed_request_params[2];
    cmeta_function_desc unborrowed_request_function = {0};
    cmeta_function_abi_desc unborrowed_request_abi = {0};
    DataBindServiceNativeBinding unborrowed_request_native = {0};
    DataBindNativeExecution unborrowed_request_execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    chttp_service_http_mount unborrowed_request_mount =
        CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_service service = {0};
    chttp_service_config service_config = CHTTP_SERVICE_CONFIG_INIT;
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_service_http_mount mismatch = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_service_http_mount invalid = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    DataBindNativeExecution execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    DataBindNativeExecution other_execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    DataBindNativeExecution invalid_execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    chttp_server server = {0};
    chttp_server_config server_config =
        chttp_service_test_server_config();
    chttp_client client = {0};
    chttp_client_config client_config =
        chttp_service_test_client_config();
    chttp_response response = {0};
    uint16_t port = 0u;
    char uri[64];

    execution.function = FunctionMeta(chttp_service_test_add);
    execution.abi = FunctionAbi(chttp_service_test_add);
    execution.invoke = chttp_service_test_invoke;
    other_execution.function = FunctionMeta(chttp_service_test_other);
    other_execution.abi = FunctionAbi(chttp_service_test_other);
    other_execution.invoke = chttp_service_test_other_invoke;
    invalid_execution = execution;
    invalid_execution.invoke = NULL;

    unknown_result_function = *FunctionMeta(chttp_service_test_add);
    unknown_result_function.result_flags = CMETA_RESULT_UNKNOWN;
    unknown_result_abi = *FunctionAbi(chttp_service_test_add);
    unknown_result_abi.function = &unknown_result_function;
    unknown_result_native = native;
    unknown_result_native.function = &unknown_result_function;
    unknown_result_execution = execution;
    unknown_result_execution.function = &unknown_result_function;
    unknown_result_execution.abi = &unknown_result_abi;

    unborrowed_request_params[0] =
        FunctionMeta(chttp_service_test_add)->params[0];
    unborrowed_request_params[1] =
        FunctionMeta(chttp_service_test_add)->params[1];
    unborrowed_request_params[0].flags = CMETA_PARAM_IN;
    unborrowed_request_function = *FunctionMeta(chttp_service_test_add);
    unborrowed_request_function.params = unborrowed_request_params;
    unborrowed_request_abi = *FunctionAbi(chttp_service_test_add);
    unborrowed_request_abi.function = &unborrowed_request_function;
    unborrowed_request_native = native;
    unborrowed_request_native.function = &unborrowed_request_function;
    unborrowed_request_execution = execution;
    unborrowed_request_execution.function = &unborrowed_request_function;
    unborrowed_request_execution.abi = &unborrowed_request_abi;

    projection = data_bind_http_projection_artifact_find(
        &databind_chttp_service_http_projection, "Calc", "Add");
    check_not_null(projection);
    check_equal(projection->method, "GET");
    check_equal(projection->route, "/add/{left}");
    check_equal(projection->success_status, 201);

    check_equal(
        data_bind_create_from_text(
            schema, sizeof(schema) - 1u, &contract, &bind_error),
        DATA_BIND_OK);
    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Calc", "Add", projection, &native,
            &method_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(method_plan);
    check_equal(data_bind_http_method_plan_route(method_plan), "/add/{left}");

    /*
     * One compiled MethodPlan joins three independent semantic authorities:
     * IDL/Schema owns required/default/constraint facts, the HTTP projection
     * owns wire locations, and CMeta owns native function/ownership semantics.
     */
    binding = data_bind_http_method_plan_binding(method_plan);
    check_not_null(binding);
    check_true(cmeta_function_desc_equal(
        data_bind_binding_plan_function(binding),
        FunctionMeta(chttp_service_test_add)));
    check_equal(
        FunctionMeta(chttp_service_test_add)->result_flags,
        (cmeta_result_flags)CMETA_RESULT_VALUE);
    check_equal(
        FunctionMeta(chttp_service_test_add)->params[0].flags,
        (cmeta_param_flags)(CMETA_PARAM_IN | CMETA_PARAM_BORROWED));
    check_equal(
        FunctionMeta(chttp_service_test_add)->params[1].flags,
        (cmeta_param_flags)(CMETA_PARAM_OUT | CMETA_PARAM_BORROWED));

    check_equal(data_bind_binding_plan_ingress_count(binding), (size_t)3u);
    check_true(data_bind_binding_plan_ingress_at(binding, 0u, &entry));
    check_equal(entry.schema_field, "left");
    check_equal(entry.address.space, "http.path");
    check_equal(entry.address.name, "left");
    check_true(entry.required);
    check_false(entry.has_default);

    entry = (DataBindBindingPlanEntry)DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    check_true(data_bind_binding_plan_ingress_at(binding, 2u, &entry));
    check_equal(entry.schema_field, "scale");
    check_equal(entry.address.space, "http.query");
    check_equal(entry.address.name, "scale");
    check_false(entry.required);
    check_true(entry.has_default);
    check_equal(entry.default_value, "1");
    check_true(entry.has_presence);

    check_equal(data_bind_binding_plan_egress_count(binding), (size_t)1u);
    entry = (DataBindBindingPlanEntry)DATA_BIND_BINDING_PLAN_ENTRY_INIT;
    check_true(data_bind_binding_plan_egress_at(binding, 0u, &entry));
    check_equal(entry.schema_field, "sum");
    check_equal(entry.address.space, "http.response.body");

    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Calc", "Add", projection, &unknown_result_native,
            &unknown_result_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(unknown_result_plan);
    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Calc", "Add", projection, &unborrowed_request_native,
            &unborrowed_request_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(unborrowed_request_plan);

    service_config.method_capacity = 1u;
    service_config.max_binding_value_bytes = 64u;
    service_config.max_response_body_bytes = 2u;
    service_config.max_call_frame_bytes = 512u;
    service_config.native_workspace_bytes = 4096u;
    service_config.native_max_depth = 16u;
    service_config.native_max_items = 64u;
    service_config.native_max_owned_bytes = 1024u;
    check_equal(chttp_service_init(&service, &service_config), SALTS_OK);

    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);

    mismatch.method_plan = method_plan;
    mismatch.native_binding = &native;
    mismatch.execution = &other_execution;
    check_equal(
        chttp_service_mount_http(&service, &server, &mismatch), SALTS_EINVAL);

    invalid.method_plan = method_plan;
    invalid.native_binding = &native;
    invalid.execution = &invalid_execution;
    check_equal(
        chttp_service_mount_http(&service, &server, &invalid), SALTS_EINVAL);

    unknown_result_mount.method_plan = unknown_result_plan;
    unknown_result_mount.native_binding = &unknown_result_native;
    unknown_result_mount.execution = &unknown_result_execution;
    check_equal(
        chttp_service_mount_http(
            &service, &server, &unknown_result_mount),
        SALTS_ENOTSUP);

    unborrowed_request_mount.method_plan = unborrowed_request_plan;
    unborrowed_request_mount.native_binding = &unborrowed_request_native;
    unborrowed_request_mount.execution = &unborrowed_request_execution;
    check_equal(
        chttp_service_mount_http(
            &service, &server, &unborrowed_request_mount),
        SALTS_ENOTSUP);

    mount.method_plan = method_plan;
    mount.native_binding = &native;
    mount.execution = &execution;
    check_equal(
        chttp_service_mount_http(&service, &server, &mount), SALTS_OK);

    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(port != 0u);
    check_greater(
        snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                 (unsigned int)port),
        0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    CHTTP_SERVICE_TEST_ADD_CALLS = 0u;

    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/3?right=4&scale=2", &response),
        SALTS_OK);
    check_equal(response.status_code, 201u);
    check_equal(response.body_size, (size_t)2u);
    check_equal(response.body, "11", 2u);
    check_equal(CHTTP_SERVICE_TEST_ADD_CALLS, (size_t)1u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/3?right=4", &response),
        SALTS_OK);
    check_equal(response.status_code, 201u);
    check_equal(response.body_size, (size_t)1u);
    check_equal(response.body, "7", 1u);
    check_equal(CHTTP_SERVICE_TEST_ADD_CALLS, (size_t)2u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/3?right=100", &response),
        SALTS_OK);
    check_equal(response.status_code, 500u);
    check_equal(response.body, "Internal Server Error", 21u);
    check_equal(CHTTP_SERVICE_TEST_ADD_CALLS, (size_t)3u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/3?right=4", &response),
        SALTS_OK);
    check_equal(response.status_code, 201u);
    check_equal(response.body_size, (size_t)1u);
    check_equal(response.body, "7", 1u);
    check_equal(CHTTP_SERVICE_TEST_ADD_CALLS, (size_t)4u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/0?right=4", &response),
        SALTS_OK);
    check_equal(response.status_code, 422u);
    check_equal(response.body, "Validation Error", 16u);
    check_equal(CHTTP_SERVICE_TEST_ADD_CALLS, (size_t)4u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/not-a-number?right=4", &response),
        SALTS_OK);
    check_equal(response.status_code, 400u);
    check_equal(response.body, "Binding Error", 13u);
    check_equal(CHTTP_SERVICE_TEST_ADD_CALLS, (size_t)4u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/3", &response),
        SALTS_OK);
    check_equal(response.status_code, 400u);
    check_equal(response.body, "Binding Error", 13u);
    check_equal(CHTTP_SERVICE_TEST_ADD_CALLS, (size_t)4u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri,
            "/add/111111111111111111111111111111111111111111111111111111111111111111111?right=4",
            &response),
        SALTS_OK);
    check_equal(response.status_code, 413u);
    check_equal(response.body, "Binding Limit Error", 19u);
    check_equal(CHTTP_SERVICE_TEST_ADD_CALLS, (size_t)4u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/99?right=4", &response),
        SALTS_OK);
    check_equal(response.status_code, 500u);
    check_equal(response.body, "Application Error", 17u);
    check_equal(CHTTP_SERVICE_TEST_ADD_CALLS, (size_t)5u);
    chttp_response_destroy(&response);

    check_equal(
        chttp_client_destroy(&client, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_server_stop(&server, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_service_destroy(&service), SALTS_OK);
    data_bind_http_method_plan_free(unborrowed_request_plan);
    data_bind_http_method_plan_free(unknown_result_plan);
    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }


  it("does not double-teardown a DataBind-rolled-back failed request") {
    static const char schema[] =
        "message RollbackRequest {"
        " bytes first;"
        " bytes second;"
        "}"
        "message AddResponse { uint32 sum; }"
        "service Rollback { Run: RollbackRequest -> AddResponse; }";
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindServiceNativeBinding native =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(
            FunctionMeta(chttp_service_test_rollback),
            &CHTTP_SERVICE_ROLLBACK_REQUEST_NATIVE,
            &ADD_RESPONSE_NATIVE);
    DataBindNativeExecution execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    DataBindHttpMethodPlan *method_plan = NULL;
    chttp_service service = {0};
    chttp_service_config service_config = CHTTP_SERVICE_CONFIG_INIT;
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_server server = {0};
    chttp_server_config server_config =
        chttp_service_test_server_config();
    chttp_client client = {0};
    chttp_client_config client_config =
        chttp_service_test_client_config();
    chttp_response response = {0};
    int rollback_first_after_bind;
    int rollback_second_after_bind;
    uint16_t port = 0u;
    char uri[64];

    execution.function = FunctionMeta(chttp_service_test_rollback);
    execution.abi = FunctionAbi(chttp_service_test_rollback);
    execution.invoke = chttp_service_test_rollback_invoke;

    check_equal(
        data_bind_create_from_text(
            schema, sizeof(schema) - 1u, &contract, &bind_error),
        DATA_BIND_OK);
    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Rollback", "Run", &CHTTP_SERVICE_ROLLBACK_HTTP,
            &native, &method_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(method_plan);

    service_config.method_capacity = 1u;
    service_config.max_binding_value_bytes = 64u;
    service_config.max_response_body_bytes = 16u;
    service_config.max_call_frame_bytes = 1024u;
    service_config.native_workspace_bytes = 4096u;
    service_config.native_max_depth = 16u;
    service_config.native_max_items = 64u;
    service_config.native_max_owned_bytes = 1024u;
    check_equal(chttp_service_init(&service, &service_config), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);

    mount.method_plan = method_plan;
    mount.native_binding = &native;
    mount.execution = &execution;
    check_equal(
        chttp_service_mount_http(&service, &server, &mount), SALTS_OK);

    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(
        snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                 (unsigned int)port),
        0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    atomic_store_explicit(
        &CHTTP_SERVICE_ROLLBACK_FIRST_RESTORES, 0, memory_order_relaxed);
    atomic_store_explicit(
        &CHTTP_SERVICE_ROLLBACK_SECOND_RESTORES, 0, memory_order_relaxed);
    atomic_store_explicit(
        &CHTTP_SERVICE_ROLLBACK_CALLS, 0, memory_order_relaxed);

    check_equal(
        chttp_service_test_call(
            &client, uri, "/rollback?first=abc&second=def", &response),
        SALTS_OK);
    check_equal(response.status_code, 500u);
    check_equal(
        atomic_load_explicit(
            &CHTTP_SERVICE_ROLLBACK_CALLS, memory_order_relaxed),
        0);
    rollback_first_after_bind = atomic_load_explicit(
        &CHTTP_SERVICE_ROLLBACK_FIRST_RESTORES, memory_order_relaxed);
    rollback_second_after_bind = atomic_load_explicit(
        &CHTTP_SERVICE_ROLLBACK_SECOND_RESTORES, memory_order_relaxed);
    check_true(rollback_first_after_bind > 0);
    check_true(rollback_second_after_bind > 0);
    chttp_response_destroy(&response);

    check_equal(
        chttp_client_destroy(&client, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_server_stop(&server, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_service_destroy(&service), SALTS_OK);

    /*
     * Service teardown after the failed request must not touch DataBind's
     * already-rolled-back request storage again.
     */
    check_equal(
        atomic_load_explicit(
            &CHTTP_SERVICE_ROLLBACK_FIRST_RESTORES, memory_order_relaxed),
        rollback_first_after_bind);
    check_equal(
        atomic_load_explicit(
            &CHTTP_SERVICE_ROLLBACK_SECOND_RESTORES, memory_order_relaxed),
        rollback_second_after_bind);

    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }

  it("defers direct execution onto a bounded borrowed executor") {
    static const char schema[] =
        "message AddRequest {"
        " @Min(1) uint32 left;"
        " uint32 right;"
        " optional uint32 scale default 1;"
        "}"
        "message AddResponse { uint32 sum; }"
        "service Calc { Add: AddRequest -> AddResponse; }";
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindServiceNativeBinding native =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(
            FunctionMeta(chttp_service_test_add),
            &ADD_REQUEST_NATIVE, &ADD_RESPONSE_NATIVE);
    DataBindNativeExecution execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    const DataBindHttpProjectionConfig *projection;
    DataBindHttpMethodPlan *method_plan = NULL;
    chttp_service service = {0};
    chttp_service_config service_config = CHTTP_SERVICE_CONFIG_INIT;
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_service_http_mount invalid = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_server server = {0};
    chttp_server_config server_config =
        chttp_service_test_server_config();
    chttp_client client = {0};
    chttp_client_config client_config =
        chttp_service_test_client_config();
    cflow_executor executor = {0};
    chttp_response response = {0};
    chttp_service_executor_gate gate;
    cflow_executor_task gate_task;
    cflow_executor_task queued_task;
    uint64_t deadline;
    uint16_t port = 0u;
    char uri[64];

    execution.function = FunctionMeta(chttp_service_test_add);
    execution.abi = FunctionAbi(chttp_service_test_add);
    execution.invoke = chttp_service_test_invoke;

    projection = data_bind_http_projection_artifact_find(
        &databind_chttp_service_http_projection, "Calc", "Add");
    check_not_null(projection);
    check_equal(
        data_bind_create_from_text(
            schema, sizeof(schema) - 1u, &contract, &bind_error),
        DATA_BIND_OK);
    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Calc", "Add", projection, &native,
            &method_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(method_plan);

    service_config.method_capacity = 1u;
    service_config.max_binding_value_bytes = 64u;
    service_config.max_response_body_bytes = 2u;
    service_config.max_call_frame_bytes = 512u;
    service_config.native_workspace_bytes = 4096u;
    service_config.native_max_depth = 16u;
    service_config.native_max_items = 64u;
    service_config.native_max_owned_bytes = 1024u;

    check_true(cflow_executor_worker_init_with_capacity(
        &executor, 1u, 1u));
    check_equal(chttp_service_init(&service, &service_config), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);

    invalid.method_plan = method_plan;
    invalid.native_binding = &native;
    invalid.execution = &execution;
    invalid.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT;
    check_equal(
        chttp_service_mount_http(&service, &server, &invalid),
        SALTS_EINVAL);

    mount.method_plan = method_plan;
    mount.native_binding = &native;
    mount.execution = &execution;
    mount.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT;
    mount.executor = &executor;
    check_equal(
        chttp_service_mount_http(&service, &server, &mount), SALTS_OK);

    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(
        snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                 (unsigned int)port),
        0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/3?right=4&scale=2", &response),
        SALTS_OK);
    check_equal(response.status_code, 201u);
    check_equal(response.body_size, (size_t)2u);
    check_equal(response.body, "11", 2u);
    chttp_response_destroy(&response);
    check_true(cflow_executor_wait_idle(&executor));

    /*
     * Saturate both the worker and its one queued slot. A Service request must
     * not wait for capacity on the HTTP owner thread; it fails the already
     * deferred terminal with 503 instead.
     */
    atomic_init(&gate.started, 0);
    atomic_init(&gate.release, 0);
    gate_task = (cflow_executor_task){
        .run = chttp_service_executor_gate_run,
        .user = &gate};
    queued_task = (cflow_executor_task){
        .run = chttp_service_executor_noop,
        .user = NULL};
    check_equal(
        cflow_executor_try_post_task(&executor, &gate_task),
        CFLOW_ADMISSION_ACCEPTED);
    deadline = cmeta_monotonic_ms() + CHTTP_SERVICE_TEST_TIMEOUT_MS;
    while (atomic_load_explicit(&gate.started, memory_order_acquire) == 0 &&
           cmeta_monotonic_ms() < deadline)
      cmeta_thread_yield();
    check_equal(
        atomic_load_explicit(&gate.started, memory_order_acquire), 1);
    check_equal(
        cflow_executor_try_post_task(&executor, &queued_task),
        CFLOW_ADMISSION_ACCEPTED);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/add/3?right=4", &response),
        SALTS_OK);
    check_equal(response.status_code, 503u);
    check_equal(response.body, "Service Unavailable", 19u);
    chttp_response_destroy(&response);

    atomic_store_explicit(&gate.release, 1, memory_order_release);
    check_true(cflow_executor_wait_idle(&executor));

    check_equal(
        chttp_client_destroy(&client, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_server_stop(&server, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_true(cflow_executor_wait_idle(&executor));
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_service_destroy(&service), SALTS_OK);
    cflow_executor_destroy(&executor);
    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }

  it("keeps deferred method state alive until task finalization") {
    static const char schema[] =
        "message AddRequest {"
        " @Min(1) uint32 left;"
        " uint32 right;"
        " optional uint32 scale default 1;"
        "}"
        "message AddResponse { uint32 sum; }"
        "service Calc { Add: AddRequest -> AddResponse; }";
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindServiceNativeBinding native =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(
            FunctionMeta(chttp_service_test_add),
            &ADD_REQUEST_NATIVE, &ADD_RESPONSE_NATIVE);
    DataBindNativeExecution execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    const DataBindHttpProjectionConfig *projection;
    DataBindHttpMethodPlan *method_plan = NULL;
    chttp_service service = {0};
    chttp_service_config service_config = CHTTP_SERVICE_CONFIG_INIT;
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_server server = {0};
    chttp_server_config server_config =
        chttp_service_test_server_config();
    cflow_executor executor = {0};
    chttp_service_executor_gate invoke_gate;
    chttp_service_request_thread_args request_args;
    cmeta_thread_t request_thread = {0};
    uint64_t deadline;
    uint16_t port = 0u;

    projection = data_bind_http_projection_artifact_find(
        &databind_chttp_service_http_projection, "Calc", "Add");
    check_not_null(projection);
    check_equal(
        data_bind_create_from_text(
            schema, sizeof(schema) - 1u, &contract, &bind_error),
        DATA_BIND_OK);
    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Calc", "Add", projection, &native,
            &method_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(method_plan);

    atomic_init(&invoke_gate.started, 0);
    atomic_init(&invoke_gate.release, 0);
    execution.function = FunctionMeta(chttp_service_test_add);
    execution.abi = FunctionAbi(chttp_service_test_add);
    execution.context = &invoke_gate;
    execution.invoke = chttp_service_test_blocking_invoke;

    service_config.method_capacity = 1u;
    service_config.max_binding_value_bytes = 64u;
    service_config.max_response_body_bytes = 2u;
    service_config.max_call_frame_bytes = 512u;
    service_config.native_workspace_bytes = 4096u;
    service_config.native_max_depth = 16u;
    service_config.native_max_items = 64u;
    service_config.native_max_owned_bytes = 1024u;

    check_true(cflow_executor_worker_init_with_capacity(
        &executor, 1u, 2u));
    check_equal(chttp_service_init(&service, &service_config), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);

    mount.method_plan = method_plan;
    mount.native_binding = &native;
    mount.execution = &execution;
    mount.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_DIRECT;
    mount.executor = &executor;
    check_equal(
        chttp_service_mount_http(&service, &server, &mount), SALTS_OK);

    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(port != 0u);

    memset(&request_args, 0, sizeof(request_args));
    request_args.config = chttp_service_test_client_config();
    request_args.target = "/add/3?right=4&scale=2";
    atomic_init(&request_args.completed, 0);
    check_greater(
        snprintf(request_args.uri, sizeof(request_args.uri),
                 "tcp://127.0.0.1:%u", (unsigned int)port),
        0);
    check_equal(
        cmeta_thread_create(
            &request_thread, chttp_service_request_thread, &request_args),
        SALTS_OK);

    deadline = cmeta_monotonic_ms() + CHTTP_SERVICE_TEST_TIMEOUT_MS;
    while (atomic_load_explicit(
               &invoke_gate.started, memory_order_acquire) == 0 &&
           cmeta_monotonic_ms() < deadline)
      cmeta_thread_yield();
    check_equal(
        atomic_load_explicit(&invoke_gate.started, memory_order_acquire), 1);

    /*
     * The accepted task still dereferences immutable method/execution state.
     * Destroy must therefore fail atomically without releasing any method
     * storage or provider lifetime.
     */
    check_equal(chttp_service_destroy(&service), SALTS_EBUSY);
    check_not_null(service.impl);

    atomic_store_explicit(&invoke_gate.release, 1, memory_order_release);
    check_equal(cmeta_thread_join(&request_thread), SALTS_OK);
    cmeta_thread_destroy(&request_thread);
    check_equal(
        atomic_load_explicit(&request_args.completed, memory_order_acquire), 1);
    check_equal(request_args.status, SALTS_OK);
    check_equal(request_args.http_status, 201u);
    check_true(cflow_executor_wait_idle(&executor));

    check_equal(
        chttp_server_stop(&server, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_service_destroy(&service), SALTS_OK);

    cflow_executor_destroy(&executor);
    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }

  it("releases owned typed-error payloads exactly once at invocation terminal") {
    static const char schema[] =
        "message AddRequest {"
        " @Min(1) uint32 left;"
        " uint32 right;"
        " optional uint32 scale default 1;"
        "}"
        "message AddResponse { uint32 sum; }"
        "message OwnedError { @Size(max = 16) string detail; }"
        "service Calc {"
        " Fail: AddRequest -> AddResponse throws OwnedError;"
        "}";
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindServiceNativeBinding native =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(
            FunctionMeta(chttp_service_test_owned_error),
            &ADD_REQUEST_NATIVE, &ADD_RESPONSE_NATIVE);
    DataBindNativeExecution execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    DataBindHttpMethodPlan *method_plan = NULL;
    chttp_service service = {0};
    chttp_service_config service_config = CHTTP_SERVICE_CONFIG_INIT;
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_server server = {0};
    chttp_server_config server_config =
        chttp_service_test_server_config();
    chttp_client client = {0};
    chttp_client_config client_config =
        chttp_service_test_client_config();
    chttp_response response = {0};
    uint16_t port = 0u;
    char uri[64];

    native.errors = CHTTP_SERVICE_OWNED_ERROR_BINDINGS;
    native.error_count = 1u;
    native.error_param_index = 2u;
    native.error_envelope_bytes = sizeof(CHttpServiceOwnedErrorEnvelope);
    native.error_kind_offset = offsetof(CHttpServiceOwnedErrorEnvelope, kind);
    native.error_kind_bytes = sizeof(uint32_t);

    execution.function = FunctionMeta(chttp_service_test_owned_error);
    execution.abi = FunctionAbi(chttp_service_test_owned_error);
    execution.invoke = chttp_service_test_owned_error_invoke;

    check_equal(
        data_bind_create_from_text(
            schema, sizeof(schema) - 1u, &contract, &bind_error),
        DATA_BIND_OK);
    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Calc", "Fail", &CHTTP_SERVICE_OWNED_ERROR_HTTP,
            &native, &method_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(method_plan);

    service_config.method_capacity = 1u;
    service_config.max_binding_value_bytes = 64u;
    service_config.max_response_body_bytes = 64u;
    service_config.max_call_frame_bytes = 1024u;
    service_config.native_workspace_bytes = 4096u;
    service_config.native_max_depth = 16u;
    service_config.native_max_items = 64u;
    service_config.native_max_owned_bytes = 1024u;
    check_equal(chttp_service_init(&service, &service_config), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);

    mount.method_plan = method_plan;
    mount.native_binding = &native;
    mount.execution = &execution;
    check_equal(
        chttp_service_mount_http(&service, &server, &mount), SALTS_OK);

    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(
        snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                 (unsigned int)port),
        0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    atomic_store_explicit(
        &CHTTP_SERVICE_OWNED_ERROR_RELEASES, 0, memory_order_relaxed);

    check_equal(
        chttp_service_test_call(
            &client, uri, "/owned-error/0?right=1", &response),
        SALTS_OK);
    check_equal(response.status_code, 422u);
    check_equal(
        atomic_load_explicit(
            &CHTTP_SERVICE_OWNED_ERROR_RELEASES, memory_order_relaxed),
        0);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/owned-error/77?right=1", &response),
        SALTS_OK);
    /*
     * Phase-1 Service egress does not flatten structured typed-error bodies;
     * the publication failure is transport-visible as 500, but native error
     * ownership must still terminate exactly once.
     */
    check_equal(response.status_code, 500u);
    check_equal(
        atomic_load_explicit(
            &CHTTP_SERVICE_OWNED_ERROR_RELEASES, memory_order_relaxed),
        1);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/owned-error/3?right=4", &response),
        SALTS_OK);
    check_equal(response.status_code, 201u);
    check_equal(response.body, "7", 1u);
    check_equal(
        atomic_load_explicit(
            &CHTTP_SERVICE_OWNED_ERROR_RELEASES, memory_order_relaxed),
        1);
    chttp_response_destroy(&response);

    check_equal(
        chttp_client_destroy(&client, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_server_stop(&server, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_service_destroy(&service), SALTS_OK);
    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }

  it("pins one Component generation across deferred Service work") {
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindNativeTypeBinding request_native =
        DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    DataBindNativeTypeBinding response_native =
        DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL);
    DataBindServiceNativeBinding native =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(NULL, NULL, NULL);
    const DataBindHttpProjectionConfig *projection = NULL;
    DataBindHttpMethodPlan *method_plan = NULL;

    cmeta_plugin_registry registry = {0};
    const cmeta_plugin_registry_config registry_config = {.capacity = 1u};
    cmeta_plugin_ref plugin_ref = {0};
    cmeta_plugin_lifecycle_info lifecycle = {0};
    bool quiescent = false;

    salts_component_plugin_generation generation =
        SALTS_COMPONENT_PLUGIN_GENERATION_INIT;
    salts_component_deployment deployments[1];
    salts_component_instance instances[1];
    salts_component_dependency dependencies[1];
    size_t activation_order[1];
    salts_component_plugin_module modules[1];
    const salts_component_plugin_generation_storage generation_storage = {
        deployments, 1u,
        instances, 1u,
        dependencies, 1u,
        activation_order, 1u,
        modules, 1u};
    salts_component_plugin_source source = {0};
    salts_component_plugin_runtime component_runtime =
        SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *previous = NULL;
    salts_component_plugin_scope rejected_scope =
        SALTS_COMPONENT_PLUGIN_SCOPE_INIT;

    cflow_executor executor = {0};
    chttp_service service = {0};
    chttp_service_config service_config = CHTTP_SERVICE_CONFIG_INIT;
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_service_http_mount invalid = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_server server = {0};
    chttp_server_config server_config =
        chttp_service_test_server_config();
    chttp_client client = {0};
    chttp_client_config client_config =
        chttp_service_test_client_config();
    chttp_response response = {0};

    chttp_service_executor_gate component_gate;
    cflow_executor_task component_gate_task;
    chttp_service_request_thread_args component_request;
    cflow_executor_stats executor_stats = {0};
    size_t executor_pending_baseline = 0u;
    cmeta_thread_t component_request_thread = {0};
    uint64_t deadline;
    uint16_t port = 0u;
    char uri[64];

    check_equal(
        CHttpPlugin_codec_create(&contract, &bind_error),
        DATA_BIND_OK);
    check_not_null(contract);

    check_equal(
        databind_11_CHttpPlugin_4_Calc_3_Add__databind_native_binding(
            &request_native, &response_native, &native, &bind_error),
        DATA_BIND_OK);

    projection = data_bind_http_projection_artifact_find(
        &databind_chttp_service_plugin_http_projection, "Calc", "Add");
    check_not_null(projection);
    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Calc", "Add", projection, &native,
            &method_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(method_plan);

    check_equal(
        cmeta_plugin_registry_init(&registry, &registry_config),
        CMETA_PLUGIN_OK);
    check_equal(
        cmeta_plugin_registry_load(
            &registry,
            GENERATED_CHTTP_SERVICE_COMPONENT_PROVIDER_PATH,
            &plugin_ref),
        CMETA_PLUGIN_OK);
    check_equal(
        cmeta_plugin_registry_start(&registry, plugin_ref),
        CMETA_PLUGIN_OK);

    source.plugin = plugin_ref;
    source.export_id = "component-provider";

    check_equal(
        salts_component_plugin_generation_build(
            &generation,
            UINT64_C(1),
            &registry,
            &generation_storage,
            NULL, 0u,
            &source, 1u,
            NULL, 0u),
        SALTS_COMPONENT_PLUGIN_OK);

    check_equal(
        salts_component_plugin_runtime_init(&component_runtime),
        SALTS_COMPONENT_PLUGIN_OK);
    check_equal(
        salts_component_plugin_runtime_publish(
            &component_runtime, &generation, &previous),
        SALTS_COMPONENT_PLUGIN_OK);
    check_null(previous);

    check_equal(
        cmeta_plugin_registry_get_lifecycle(
            &registry, plugin_ref, &lifecycle),
        CMETA_PLUGIN_OK);
    check_equal(lifecycle.active_leases, (size_t)1u);

    service_config.method_capacity = 1u;
    service_config.max_binding_value_bytes = 64u;
    service_config.max_response_body_bytes = 2u;
    service_config.max_call_frame_bytes = 512u;
    service_config.native_workspace_bytes = 4096u;
    service_config.native_max_depth = 16u;
    service_config.native_max_items = 64u;
    service_config.native_max_owned_bytes = 1024u;

    check_true(cflow_executor_worker_init_with_capacity(
        &executor, 1u, 2u));
    check_equal(chttp_service_init(&service, &service_config), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);

    invalid.method_plan = method_plan;
    invalid.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_COMPONENT;
    invalid.executor = &executor;
    invalid.component_id = "CHttpPlugin_Calc_Add";
    check_equal(
        chttp_service_mount_http(&service, &server, &invalid),
        SALTS_EINVAL);

    invalid.component_runtime = &component_runtime;
    invalid.component_id = "missing.component";
    check_equal(
        chttp_service_mount_http(&service, &server, &invalid),
        SALTS_EINVAL);
    check_equal(component_runtime.active_scopes, (size_t)0u);

    mount.method_plan = method_plan;
    mount.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_COMPONENT;
    mount.executor = &executor;
    mount.component_runtime = &component_runtime;
    mount.component_id = "CHttpPlugin_Calc_Add";
    check_equal(
        chttp_service_mount_http(&service, &server, &mount),
        SALTS_OK);
    check_equal(component_runtime.active_scopes, (size_t)1u);

    check_equal(
        salts_component_plugin_runtime_close(
            &component_runtime, &previous),
        SALTS_COMPONENT_PLUGIN_OK);
    check_true(previous == &generation);
    check_equal(
        salts_component_plugin_scope_acquire(
            &component_runtime, &rejected_scope),
        SALTS_COMPONENT_PLUGIN_INVALID_STATE);
    check_false(rejected_scope.live);
    check_equal(
        salts_component_plugin_generation_drain(
            &component_runtime, &generation),
        SALTS_COMPONENT_PLUGIN_BUSY);

    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(
        snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                 (unsigned int)port),
        0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);

    atomic_init(&component_gate.started, 0);
    atomic_init(&component_gate.release, 0);
    component_gate_task = (cflow_executor_task){
        .run = chttp_service_executor_gate_run,
        .user = &component_gate};
    check_equal(
        cflow_executor_try_post_task(&executor, &component_gate_task),
        CFLOW_ADMISSION_ACCEPTED);

    deadline = cmeta_monotonic_ms() + CHTTP_SERVICE_TEST_TIMEOUT_MS;
    while (atomic_load_explicit(
               &component_gate.started, memory_order_acquire) == 0 &&
           cmeta_monotonic_ms() < deadline)
      cmeta_thread_yield();
    check_equal(
        atomic_load_explicit(
            &component_gate.started, memory_order_acquire),
        1);
    check_true(cflow_executor_get_stats(&executor, &executor_stats));
    executor_pending_baseline = executor_stats.pending;

    memset(&component_request, 0, sizeof(component_request));
    component_request.config = chttp_service_test_client_config();
    component_request.target = "/plugin/3?right=4&scale=2";
    atomic_init(&component_request.completed, 0);
    check_greater(
        snprintf(component_request.uri, sizeof(component_request.uri),
                 "tcp://127.0.0.1:%u", (unsigned int)port),
        0);
    check_equal(
        cmeta_thread_create(
            &component_request_thread,
            chttp_service_request_thread,
            &component_request),
        SALTS_OK);

    deadline = cmeta_monotonic_ms() + CHTTP_SERVICE_TEST_TIMEOUT_MS;
    do {
      check_true(cflow_executor_get_stats(&executor, &executor_stats));
      if (executor_stats.pending > executor_pending_baseline) break;
      cmeta_thread_yield();
    } while (cmeta_monotonic_ms() < deadline);
    check_true(executor_stats.pending > executor_pending_baseline);

    check_equal(chttp_service_destroy(&service), SALTS_EBUSY);
    check_equal(component_runtime.active_scopes, (size_t)1u);
    check_equal(
        salts_component_plugin_generation_drain(
            &component_runtime, &generation),
        SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(
        cmeta_plugin_registry_unload(&registry, plugin_ref),
        CMETA_PLUGIN_BUSY);

    atomic_store_explicit(
        &component_gate.release, 1, memory_order_release);
    check_equal(
        cmeta_thread_join(&component_request_thread), SALTS_OK);
    cmeta_thread_destroy(&component_request_thread);
    check_equal(component_request.status, SALTS_OK);
    check_equal(component_request.http_status, 201u);
    check_true(cflow_executor_wait_idle(&executor));

    check_equal(
        chttp_client_destroy(&client, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_server_stop(&server, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_true(cflow_executor_wait_idle(&executor));
    check_equal(chttp_server_destroy(&server), SALTS_OK);

    check_equal(chttp_service_destroy(&service), SALTS_OK);
    check_equal(component_runtime.active_scopes, (size_t)0u);

    check_equal(
        salts_component_plugin_generation_drain(
            &component_runtime, &generation),
        SALTS_COMPONENT_PLUGIN_OK);
    check_equal(
        salts_component_plugin_runtime_destroy(&component_runtime),
        SALTS_COMPONENT_PLUGIN_OK);

    check_equal(
        cmeta_plugin_registry_get_lifecycle(
            &registry, plugin_ref, &lifecycle),
        CMETA_PLUGIN_OK);
    check_equal(lifecycle.active_leases, (size_t)0u);

    check_equal(
        cmeta_plugin_registry_request_stop(&registry, plugin_ref),
        CMETA_PLUGIN_OK);
    check_equal(
        cmeta_plugin_registry_poll_quiescent(
            &registry, plugin_ref, &quiescent),
        CMETA_PLUGIN_OK);
    check_true(quiescent);
    check_equal(
        cmeta_plugin_registry_unload(&registry, plugin_ref),
        CMETA_PLUGIN_OK);
    check_equal(
        cmeta_plugin_registry_destroy(&registry),
        CMETA_PLUGIN_OK);

    cflow_executor_destroy(&executor);
    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }

  it("executes the same MethodPlan through an admitted CFlow Service projection") {
    static const char schema[] =
        "message AddRequest {"
        " @Min(1) uint32 left;"
        " uint32 right;"
        " uint32 scale;"
        "}"
        "message AddResponse { uint32 sum; }"
        "service Calc { Add: AddRequest -> AddResponse; }";
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindServiceNativeBinding native =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(
            FunctionMeta(chttp_service_test_add),
            &ADD_REQUEST_CFLOW_NATIVE, &ADD_RESPONSE_NATIVE);
    DataBindHttpMethodPlan *method_plan = NULL;
    cflow_function_typed_adapter_projection projection = {0};
    cflow_executor executor = {0};
    chttp_service service = {0};
    chttp_service_config service_config = CHTTP_SERVICE_CONFIG_INIT;
    chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_service_http_mount invalid = CHTTP_SERVICE_HTTP_MOUNT_INIT;
    chttp_server server = {0};
    chttp_server_config server_config =
        chttp_service_test_server_config();
    chttp_client client = {0};
    chttp_client_config client_config =
        chttp_service_test_client_config();
    chttp_response response = {0};
    uint16_t port = 0u;
    char uri[64];

    check_equal(
        chttp_service_test_cflow_projection(&projection),
        CFLOW_FUNCTION_PROJECTION_OK);
    check_true(
        cflow_function_typed_adapter_projection_valid(&projection));

    check_equal(
        data_bind_create_from_text(
            schema, sizeof(schema) - 1u, &contract, &bind_error),
        DATA_BIND_OK);
    check_equal(
        data_bind_http_method_plan_compile_service(
            contract, "Calc", "Add", &CHTTP_SERVICE_CFLOW_HTTP,
            &native, &method_plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(method_plan);
    check_equal(
        data_bind_http_method_plan_route(method_plan), "/flow/{left}");

    service_config.method_capacity = 1u;
    service_config.max_binding_value_bytes = 64u;
    service_config.max_response_body_bytes = 2u;
    service_config.max_call_frame_bytes = 512u;
    service_config.native_workspace_bytes = 4096u;
    service_config.native_max_depth = 16u;
    service_config.native_max_items = 64u;
    service_config.native_max_owned_bytes = 1024u;

    check_true(cflow_executor_worker_init_with_capacity(
        &executor, 1u, 2u));
    check_equal(chttp_service_init(&service, &service_config), SALTS_OK);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);

    invalid.method_plan = method_plan;
    invalid.native_binding = &native;
    invalid.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_CFLOW;
    invalid.executor = &executor;
    check_equal(
        chttp_service_mount_http(&service, &server, &invalid),
        SALTS_EINVAL);

    mount.method_plan = method_plan;
    mount.native_binding = &native;
    mount.execution = NULL;
    mount.execution_mode = CHTTP_SERVICE_EXECUTION_DEFERRED_CFLOW;
    mount.executor = &executor;
    mount.cflow_projection = &projection;
    check_equal(
        chttp_service_mount_http(&service, &server, &mount),
        SALTS_OK);

    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_greater(
        snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                 (unsigned int)port),
        0);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    CHTTP_SERVICE_TEST_ADD_CALLS = 0u;

    check_equal(
        chttp_service_test_call(
            &client, uri, "/flow/3?right=4&scale=2", &response),
        SALTS_OK);
    check_equal(response.status_code, 201u);
    check_equal(response.body_size, (size_t)2u);
    check_equal(response.body, "11", 2u);
    check_equal(CHTTP_SERVICE_TEST_ADD_CALLS, (size_t)1u);
    chttp_response_destroy(&response);

    response = (chttp_response){0};
    check_equal(
        chttp_service_test_call(
            &client, uri, "/flow/0?right=4&scale=2", &response),
        SALTS_OK);
    check_equal(response.status_code, 422u);
    check_equal(response.body, "Validation Error", 16u);
    check_equal(CHTTP_SERVICE_TEST_ADD_CALLS, (size_t)1u);
    chttp_response_destroy(&response);

    check_equal(
        chttp_client_destroy(&client, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_equal(
        chttp_server_stop(&server, CHTTP_SERVICE_TEST_TIMEOUT_MS),
        SALTS_OK);
    check_true(cflow_executor_wait_idle(&executor));
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(chttp_service_destroy(&service), SALTS_OK);
    cflow_executor_destroy(&executor);
    data_bind_http_method_plan_free(method_plan);
    data_bind_free(contract);
  }
}
