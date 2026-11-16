#include <cjwt/cjwt.h>
#include <tinytest.hpp>

spec("Public JWT C++ API") {
    it("links the C ABI from a C++ translation unit") {
        cjwt_alg_t algorithm = alg_none;
        check_equal(cjwt_alg_string_to_enum("RS256", sizeof("RS256") - 1u, &algorithm), CJWTE_OK);
        check_equal(algorithm, alg_rs256);
    }
}
