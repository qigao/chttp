#ifndef CHTTP_INVOCATION_HPP
#define CHTTP_INVOCATION_HPP
#include "chttp_invocation.h"
namespace chttp::detail {
/* Exact native type; unwind C++ locals before translating at the C boundary. */
template<chttp_invocation_action_fn Action>
int invocation_action(void *self, const chttp_invocation *invocation) noexcept {
  try { return Action(self, invocation); }
  catch (...) { return SALTS_EIO; }
}
}
#endif
