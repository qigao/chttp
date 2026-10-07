#include <http_client/response_scope.h>
#include <tinytest.hpp>

#include <cstdlib>
#include <type_traits>

static_assert(std::is_standard_layout<chttp_response>::value,
    "response scope must preserve the public C layout");

static int transfer_response(chttp_response *temporary, chttp_response *out) {
  temporary->body = std::malloc(sizeof(char));
  if (temporary->body == nullptr) return SALTS_ENOMEM;
  *static_cast<char *>(temporary->body) = 'x';
  temporary->body_size = sizeof(char);
  chttp_response_move(out, temporary);
  return SALTS_OK;
}

spec("CHttp response scope C++ consumer") {
  static chttp_response response = {};
  after_each() { chttp_response_destroy(&response); }

  it("keeps a moved response alive beyond its CMeta scope") {
    int status;
    cmeta_scope(status, cmeta_autos((chttp_response, temporary)),
        cmeta_body(transfer_response(&temporary, &response)));
    check_equal(status, SALTS_OK);
    check_not_null(response.body);
    check_equal(response.body_size, sizeof(char));
    check_equal(*static_cast<const char *>(response.body), 'x');
    check_true(cmeta_data_desc_valid(cmeta_reflected_data(chttp_response)));
  }
}
