#ifndef CHTTP_INVOCATION_HPP
#define CHTTP_INVOCATION_HPP
#include "chttp_invocation.h"
#include "chttp_application_worker.h"
namespace chttp::detail {
/* Exact native type; unwind C++ locals before translating at the C boundary. */
template<chttp_invocation_action_fn Action>
int invocation_action(void *self, const chttp_invocation *invocation) noexcept {
  try { return Action(self, invocation); }
  catch (...) { return SALTS_EIO; }
}
template<chttp_application_admission_fn Action>
int admission_action(void *self, const chttp_server_request_view *request,
    chttp_server_admission_result *result) noexcept {
  try { return Action(self, request, result); }
  catch (...) { return SALTS_EIO; }
}
template<chttp_application_asset_policy_fn Action>
int asset_policy_action(void *self, const char *path, chttp_application_asset_policy_result *result) noexcept {
  try { return Action(self, path, result); }
  catch (...) { return SALTS_EIO; }
}
template<chttp_body_read_fn Read>
int source_read(void *user, void *buffer, size_t capacity, size_t *out) noexcept {
  try { return Read(user, buffer, capacity, out); }
  catch (...) { return SALTS_EIO; }
}
template<chttp_application_upload_open_fn Open>
int upload_open(void *self, const chttp_server_request_view *request, chttp_body_sink *sink) noexcept {
  try { return Open(self, request, sink); }
  catch (...) { return SALTS_EIO; }
}
template<chttp_body_write_fn Write>
int sink_write(void *user, const void *data, size_t size) noexcept {
  try { return Write(user, data, size); }
  catch (...) { return SALTS_EIO; }
}
}
#endif
