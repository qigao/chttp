#include <http_client/response_scope.h>
#include "tinytest.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { RESPONSE_TEST_ALLOCATION_CAPACITY = 32 };
static void *allocations[RESPONSE_TEST_ALLOCATION_CAPACITY];
static size_t allocation_attempts;
static size_t allocation_failure;
static size_t live_allocations;
static int invalid_release;

static void *response_test_track(void *pointer) {
  size_t index;
  if (pointer == NULL) return NULL;
  for (index = 0u; index < RESPONSE_TEST_ALLOCATION_CAPACITY; ++index) {
    if (allocations[index] == NULL) {
      allocations[index] = pointer;
      ++live_allocations;
      return pointer;
    }
  }
  abort();
}

static void *response_test_malloc(size_t bytes) {
  if (++allocation_attempts == allocation_failure) return NULL;
  return response_test_track(malloc(bytes));
}

static void *response_test_calloc(size_t count, size_t bytes) {
  if (++allocation_attempts == allocation_failure) return NULL;
  return response_test_track(calloc(count, bytes));
}

static void response_test_free(void *pointer) {
  size_t index;
  if (pointer == NULL) return;
  for (index = 0u; index < RESPONSE_TEST_ALLOCATION_CAPACITY; ++index) {
    if (allocations[index] == pointer) {
      allocations[index] = NULL;
      --live_allocations;
      free(pointer);
      return;
    }
  }
  invalid_release = 1;
}

/* Compile the production ownership unit with test-local allocation failures.
 * No allocator hook, mutable global or mock enters the production library. */
#define malloc response_test_malloc
#define calloc response_test_calloc
#define free response_test_free
#include "../src/chttp_response_owner.c"
#undef malloc
#undef calloc
#undef free

static const chttp_header response_headers[] = {
    {"Content-Type", "text/plain"}, {"X-Test", "ownership"}};
static const char response_body[] = "payload";
static const chttp_response_view response_source = {
    .http_major = 1u, .http_minor = 1u, .status_code = 200u,
    .reason = "OK", .headers = response_headers,
    .header_count = sizeof(response_headers) / sizeof(response_headers[0]),
    .body = response_body, .body_size = sizeof(response_body) - 1u,
    .protocol_keep_alive = 1};

const cmeta_type_desc *chttp_response_scope_peer_type(void);

static int copy_then_return(chttp_response *temporary, int result) {
  const int status = chttp_response_copy(&response_source, temporary);
  return status == SALTS_OK ? result : status;
}

spec("CHttp owning response lifecycle") {
  before_each() {
    allocation_attempts = 0u;
    allocation_failure = 0u;
    invalid_release = 0;
  }
  after_each() {
    size_t index;
    for (index = 0u; index < RESPONSE_TEST_ALLOCATION_CAPACITY; ++index) {
      free(allocations[index]);
      allocations[index] = NULL;
    }
    live_allocations = 0u;
  }

  it("rolls back every allocation failure without publishing a partial response") {
    chttp_response response = {0};
    size_t count;
    size_t failure;
    check_equal(chttp_response_copy(&response_source, &response), SALTS_OK);
    count = allocation_attempts;
    chttp_response_destroy(&response);
    check_equal(live_allocations, (size_t)0u);
    for (failure = 1u; failure <= count; ++failure) {
      allocation_attempts = 0u;
      allocation_failure = failure;
      check_equal(chttp_response_copy(&response_source, &response), SALTS_ENOMEM);
      check_null(response.reason);
      check_null(response.headers);
      check_null(response.body);
      check_equal(response.header_count, (size_t)0u);
      check_equal(response.body_size, (size_t)0u);
      check_equal(response.status_code, 0u);
      check_equal(live_allocations, (size_t)0u);
      chttp_response_destroy(&response);
      check_false(invalid_release);
    }
  }

  it("rejects inconsistent borrowed views before allocation") {
    chttp_response_view source = response_source;
    chttp_response response = {0};
    chttp_header invalid_header = {"name", NULL};
    source.headers = NULL;
    check_equal(chttp_response_copy(&source, &response), SALTS_EPROTO);
    source = response_source;
    source.header_count = SIZE_MAX;
    check_equal(chttp_response_copy(&source, &response), SALTS_EPROTO);
    source = response_source;
    source.headers = &invalid_header;
    source.header_count = 1u;
    check_equal(chttp_response_copy(&source, &response), SALTS_EPROTO);
    check_equal(allocation_attempts, (size_t)0u);
    chttp_response_destroy(&response);
    check_false(invalid_release);
  }

  it("deep copies payloads and moves ownership exactly once") {
    chttp_response response = {0};
    chttp_response moved = {0};
    void *body;
    check_equal(chttp_response_copy(&response_source, &response), SALTS_OK);
    check_equal(response.status_code, response_source.status_code);
    check_equal(response.header_count, response_source.header_count);
    check_equal(response.reason, "OK");
    check_equal(response.headers[1].value, "ownership");
    check_true(response.headers != response_source.headers);
    check_true(response.body != response_source.body);
    check_equal(memcmp(response.body, response_body, response.body_size), 0);
    body = response.body;
    chttp_response_move(&moved, &response);
    check_true(moved.body == body);
    check_null(response.body);
    check_equal(response.header_count, (size_t)0u);
    chttp_response_destroy(&response);
    check_greater(live_allocations, (size_t)0u);
    chttp_response_destroy(&moved);
    chttp_response_destroy(&moved);
    check_equal(live_allocations, (size_t)0u);
    check_false(invalid_release);
  }

  it("preserves the byte count of an unbuffered streaming response") {
    chttp_response_view source = response_source;
    chttp_response response = {0};
    source.body = NULL;
    check_equal(chttp_response_copy(&source, &response), SALTS_OK);
    check_null(response.body);
    check_equal(response.body_size, source.body_size);
    chttp_response_destroy(&response);
    check_equal(live_allocations, (size_t)0u);
    check_false(invalid_release);
  }

  it("scope cleans up early body returns and preserves native errors") {
    int status;
    cmeta_scope(status, cmeta_autos((chttp_response, response)),
        cmeta_body(copy_then_return(&response, SALTS_ECANCELED)));
    check_equal(status, SALTS_ECANCELED);
    check_equal(live_allocations, (size_t)0u);
    check_false(invalid_release);
  }

  it("reflects a read-only summary with semantic identity across translation units") {
    const cmeta_data_desc *data = cmeta_reflected_data(chttp_response);
    const cmeta_data_struct_shape *shape =
        (const cmeta_data_struct_shape *)data->shape;
    check_true(cmeta_data_desc_valid(data));
    check_null(data->construct_ops);
    check_true(cmeta_type_equal(
        cmeta_reflected_storage(chttp_response), chttp_response_scope_peer_type()));
    check_equal(shape->field_count, (size_t)6u);
    check_equal(shape->fields[2].name, "status_code");
  }
}
