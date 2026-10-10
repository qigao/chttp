#include "chttp_document.service_native.h"
#include "chttp_document_native.h"
#include <string.h>

int databind_8_Document_3_Api_10_EchoNested(const Nested_t *request, Nested_t *response) {
  const cmeta_data_desc *data = NULL;
  DataBindError error = DATA_BIND_ERROR_INIT;
  if (Nested_cmeta_data(&data, &error) != DATA_BIND_OK) return -1;
  return cmeta_data_value_copy(data, response, request) == CMETA_OK ? 0 : -1;
}

int databind_8_Document_3_Api_9_GetNested(const Request_t *request, Nested_t *response) {
  return databind_8_Document_3_Api_3_Get(request, &response->child);
}

int databind_8_Document_3_Api_9_GetBinary(const Request_t *request, BinaryReply_t *response) {
  (void)request;
  (void)response;
  return 0; /* An empty binary value still has no XML representation. */
}

int databind_8_Document_3_Api_4_Echo(const Reply_t *request, Reply_t *response) {
  response->total = request->total;
  response->ready = request->ready;
  response->text = tstr_from_v(vstr_from_buf(request->text, tstr_len(request->text)));
  return response->text != NULL ? 0 : -1;
}

int databind_8_Document_3_Api_9_EchoState(const StateReply_t *request, StateReply_t *response) {
  response->note = request->note;
  response->count = request->count;
  memcpy(response->_presence, request->_presence, sizeof(response->_presence));
  memcpy(response->_nulls, request->_nulls, sizeof(response->_nulls));
  response->text = tstr_from_v(vstr_from_buf(request->text, tstr_len(request->text)));
  return response->text != NULL ? 0 : -1;
}

int databind_8_Document_3_Api_3_Get(const Request_t *request, Reply_t *response) {
  if (request == NULL || response == NULL) return -1;
  response->total = UINT64_MAX;
  response->ready = 1u;
  response->text = tstr_from_v(vstr_from_cstr("<tag>&\"hello\""));
  if (response->text == NULL) return -1;
  if (request->large != 0u) {
    /* Exceeds transport staging while remaining within native ownership limits. */
    for (size_t i = 0; i < 8u; ++i) {
      tstr next = tstr_cat_v(response->text, vstr_from_cstr(
          "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"));
      if (next == NULL) return -1;
      response->text = next;
    }
  }
  return 0;
}

int databind_8_Document_3_Api_5_State(const Request_t *request, StateReply_t *response) {
  if (request == NULL || response == NULL) return -1;
  if (request->large == 0u) {
    response->_nulls[StateReply_NULLABLE_count / 8u] |=
        (uint8_t)(1u << (StateReply_NULLABLE_count % 8u));
  } else {
    response->_presence[StateReply_OPTIONAL_note / 8u] |=
        (uint8_t)(1u << (StateReply_OPTIONAL_note % 8u));
    response->note = 9u;
    response->count = 0u;
  }
  response->text = tstr_from_v(vstr_from_cstr("state"));
  return response->text != NULL ? 0 : -1;
}
