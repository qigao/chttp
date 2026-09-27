#include "chttp_cnet_retained.h"

#include "tinytest.h"

#include <string.h>

spec("CHTTP retained CNet buffer ownership") {
  it("blocks mutation and rebind while a retained send owns the wrapper") {
    unsigned char first[32];
    unsigned char second[64];
    mem_buffer_t *wrapper = NULL;
    mem_buffer_t *retained;

    memset(first, 0x11, sizeof(first));
    memset(second, 0x22, sizeof(second));

    check_equal(
        chttp_cnet_retained_bind(&wrapper, first, sizeof(first)),
        SALTS_OK);
    check_not_null(wrapper);
    check_equal(mem_buffer_ref_count(wrapper), (uint32_t)1u);

    retained = mem_buffer_retain(wrapper);
    check_not_null(retained);
    check_equal(mem_buffer_ref_count(wrapper), (uint32_t)2u);

    check_equal(
        chttp_cnet_retained_prepare(wrapper, first, sizeof(first), 8u),
        SALTS_EBUSY);
    check_equal(
        chttp_cnet_retained_bind(&wrapper, second, sizeof(second)),
        SALTS_EBUSY);
    check_equal(
        chttp_cnet_retained_release(&wrapper),
        SALTS_EBUSY);
    check_not_null(wrapper);

    mem_buffer_release(retained);
    check_equal(mem_buffer_ref_count(wrapper), (uint32_t)1u);

    check_equal(
        chttp_cnet_retained_prepare(wrapper, first, sizeof(first), 8u),
        SALTS_OK);
    check_equal(mem_buffer_used(wrapper), (size_t)8u);

    check_equal(
        chttp_cnet_retained_bind(&wrapper, second, sizeof(second)),
        SALTS_OK);
    check_true(mem_buffer_const_data(wrapper) == (const char *)second);
    check_equal(mem_buffer_capacity(wrapper), sizeof(second));
    check_equal(mem_buffer_used(wrapper), (size_t)0u);

    check_equal(chttp_cnet_retained_release(&wrapper), SALTS_OK);
    check_null(wrapper);
  }
}
