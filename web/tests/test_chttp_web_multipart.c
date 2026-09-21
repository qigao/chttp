#include <chttp_web/web.h>

#include "tinytest.h"

#include <salts/error_codes.h>

#include <stddef.h>
#include <string.h>

enum {
  MP_TEST_PARTS = 8,
  MP_TEST_NAME_BYTES = 64,
  MP_TEST_FILENAME_BYTES = 128,
  MP_TEST_CONTENT_TYPE_BYTES = 128,
  MP_TEST_DATA_BYTES = 512
};

typedef struct mp_test_part {
  char name[MP_TEST_NAME_BYTES];
  size_t name_size;
  char filename[MP_TEST_FILENAME_BYTES];
  size_t filename_size;
  char content_type[MP_TEST_CONTENT_TYPE_BYTES];
  size_t content_type_size;
  int has_filename;
  int has_content_type;
  char data[MP_TEST_DATA_BYTES];
  size_t data_size;
} mp_test_part;

typedef struct mp_test_probe {
  mp_test_part parts[MP_TEST_PARTS];
  size_t begin_count;
  size_t end_count;
  int fail_begin;
  int fail_data;
  int fail_end;
} mp_test_probe;

static int mp_test_copy_view(
    char *output, size_t capacity,
    chttp_web_string_view value, size_t *out_size) {
  if (value.size >= capacity) return SALTS_ENOBUFS;
  if (value.size != 0u) memcpy(output, value.data, value.size);
  output[value.size] = '\0';
  *out_size = value.size;
  return SALTS_OK;
}

static int mp_test_begin(
    void *user, const chttp_web_multipart_part *part) {
  mp_test_probe *probe = (mp_test_probe *)user;
  mp_test_part *copy;
  int status;

  if (probe == NULL || part == NULL ||
      probe->begin_count >= MP_TEST_PARTS)
    return SALTS_EINVAL;
  if (probe->fail_begin) return SALTS_EIO;

  copy = &probe->parts[probe->begin_count];
  memset(copy, 0, sizeof(*copy));

  status = mp_test_copy_view(
      copy->name, sizeof(copy->name), part->name, &copy->name_size);
  if (status != SALTS_OK) return status;

  copy->has_filename = part->has_filename ? 1 : 0;
  if (part->has_filename) {
    status = mp_test_copy_view(
        copy->filename, sizeof(copy->filename),
        part->filename, &copy->filename_size);
    if (status != SALTS_OK) return status;
  }

  copy->has_content_type = part->has_content_type ? 1 : 0;
  if (part->has_content_type) {
    status = mp_test_copy_view(
        copy->content_type, sizeof(copy->content_type),
        part->content_type, &copy->content_type_size);
    if (status != SALTS_OK) return status;
  }

  ++probe->begin_count;
  return SALTS_OK;
}

static int mp_test_data(void *user, const void *data, size_t size) {
  mp_test_probe *probe = (mp_test_probe *)user;
  mp_test_part *part;

  if (probe == NULL || (size != 0u && data == NULL) ||
      probe->begin_count == 0u)
    return SALTS_EINVAL;
  if (probe->fail_data) return SALTS_EIO;

  part = &probe->parts[probe->begin_count - 1u];
  if (size > sizeof(part->data) - part->data_size)
    return SALTS_ENOBUFS;
  if (size != 0u)
    memcpy(part->data + part->data_size, data, size);
  part->data_size += size;
  return SALTS_OK;
}

static int mp_test_end(void *user) {
  mp_test_probe *probe = (mp_test_probe *)user;
  if (probe == NULL) return SALTS_EINVAL;
  if (probe->fail_end) return SALTS_EIO;
  ++probe->end_count;
  return SALTS_OK;
}

static chttp_web_multipart_callbacks mp_test_callbacks(void) {
  const chttp_web_multipart_callbacks callbacks = {
      .size = sizeof(chttp_web_multipart_callbacks),
      .part_begin = mp_test_begin,
      .part_data = mp_test_data,
      .part_end = mp_test_end};
  return callbacks;
}

static chttp_web_multipart_limits mp_test_limits(void) {
  chttp_web_multipart_limits limits =
      (chttp_web_multipart_limits)CHTTP_WEB_MULTIPART_LIMITS_INIT;
  limits.max_parts = MP_TEST_PARTS;
  limits.max_header_count = 8u;
  limits.max_header_bytes = 1024u;
  limits.max_name_bytes = 32u;
  limits.max_filename_bytes = 96u;
  limits.max_content_type_bytes = 64u;
  limits.max_field_bytes = 256u;
  limits.max_total_bytes = 4096u;
  return limits;
}

static chttp_web_status mp_test_init(
    chttp_web_multipart_parser *parser,
    mp_test_probe *probe,
    const chttp_web_multipart_limits *limits,
    chttp_web_error *error) {
  const chttp_web_multipart_callbacks callbacks = mp_test_callbacks();
  return chttp_web_multipart_init(
      parser,
      "multipart/form-data; charset=utf-8; boundary=\"AaB03x\"",
      limits,
      &callbacks,
      probe,
      error);
}

static chttp_web_status mp_test_feed_chunks(
    chttp_web_multipart_parser *parser,
    const char *body,
    size_t body_size,
    size_t chunk_size,
    chttp_web_error *error) {
  size_t position = 0u;
  while (position < body_size) {
    size_t size = chunk_size;
    chttp_web_status status;
    if (size > body_size - position) size = body_size - position;
    status = chttp_web_multipart_feed(
        parser, body + position, size, error);
    if (status != CHTTP_WEB_OK) return status;
    position += size;
  }
  return CHTTP_WEB_OK;
}

static chttp_web_status mp_test_run(
    mp_test_probe *probe,
    const chttp_web_multipart_limits *limits,
    const char *body,
    size_t body_size,
    size_t chunk_size,
    chttp_web_error *error) {
  chttp_web_multipart_parser parser = CHTTP_WEB_MULTIPART_PARSER_INIT;
  chttp_web_status status = mp_test_init(&parser, probe, limits, error);
  if (status != CHTTP_WEB_OK) return status;
  status = mp_test_feed_chunks(
      &parser, body, body_size, chunk_size, error);
  if (status != CHTTP_WEB_OK) return status;
  return chttp_web_multipart_finish(&parser, error);
}

spec("CHttp::Web multipart/form-data parser") {
  it("parses byte-by-byte chunks, repeated fields, empty filename, and optional content type") {
    static const char body[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"alpha\"\r\n"
        "\r\n"
        "one\r\n"
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"\"\r\n"
        "Content-Type: text/plain\r\n"
        "X-Ignored: yes\r\n"
        "\r\n"
        "file-data\r\n"
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"alpha\"\r\n"
        "\r\n"
        "\r\n"
        "--AaB03x--\r\n";
    mp_test_probe probe = {0};
    chttp_web_multipart_limits limits = mp_test_limits();
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    check_equal(
        mp_test_run(
            &probe, &limits, body, sizeof(body) - 1u, 1u, &error),
        CHTTP_WEB_OK);
    check_equal(probe.begin_count, (size_t)3u);
    check_equal(probe.end_count, (size_t)3u);

    check_equal(probe.parts[0].name, "alpha");
    check_false(probe.parts[0].has_filename);
    check_equal(probe.parts[0].data_size, (size_t)3u);
    check_equal(probe.parts[0].data, "one", 3u);

    check_equal(probe.parts[1].name, "upload");
    check_true(probe.parts[1].has_filename);
    check_equal(probe.parts[1].filename_size, (size_t)0u);
    check_true(probe.parts[1].has_content_type);
    check_equal(probe.parts[1].content_type, "text/plain");
    check_equal(probe.parts[1].data_size, (size_t)9u);
    check_equal(probe.parts[1].data, "file-data", 9u);

    check_equal(probe.parts[2].name, "alpha");
    check_equal(probe.parts[2].data_size, (size_t)0u);
  }

  it("accepts unquoted boundaries and enforces the 70-byte hard maximum") {
    static const char body[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "ok\r\n"
        "--AaB03x--\r\n";
    static const char boundary70_type[] =
        "multipart/form-data; boundary=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char boundary71_type[] =
        "multipart/form-data; boundary=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    chttp_web_multipart_parser parser = CHTTP_WEB_MULTIPART_PARSER_INIT;
    chttp_web_multipart_limits limits = mp_test_limits();
    chttp_web_multipart_callbacks callbacks = mp_test_callbacks();
    mp_test_probe probe = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    check_equal(
        chttp_web_multipart_init(
            &parser, "multipart/form-data; boundary=AaB03x",
            &limits, &callbacks, &probe, &error),
        CHTTP_WEB_OK);
    check_equal(
        mp_test_feed_chunks(
            &parser, body, sizeof(body) - 1u, 1u, &error),
        CHTTP_WEB_OK);
    check_equal(chttp_web_multipart_finish(&parser, &error), CHTTP_WEB_OK);
    check_equal(probe.begin_count, (size_t)1u);
    check_equal(probe.end_count, (size_t)1u);

    memset(&probe, 0, sizeof(probe));
    check_equal(
        chttp_web_multipart_init(
            &parser, boundary70_type,
            &limits, &callbacks, &probe, &error),
        CHTTP_WEB_OK);
    check_equal(
        parser.boundary_size,
        (size_t)CHTTP_WEB_MULTIPART_BOUNDARY_HARD_MAX);

    check_equal(
        chttp_web_multipart_init(
            &parser, boundary71_type,
            &limits, &callbacks, &probe, &error),
        CHTTP_WEB_INVALID_ARGUMENT);
  }

  it("preserves false boundary prefixes as part data") {
    static const char body[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"value\"\r\n"
        "\r\n"
        "a\r\n--AaB03y-b\r-c\r\n"
        "--AaB03x--\r\n";
    static const char expected[] = "a\r\n--AaB03y-b\r-c";
    mp_test_probe probe = {0};
    chttp_web_multipart_limits limits = mp_test_limits();
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    check_equal(
        mp_test_run(
            &probe, &limits, body, sizeof(body) - 1u, 2u, &error),
        CHTTP_WEB_OK);
    check_equal(probe.begin_count, (size_t)1u);
    check_equal(probe.end_count, (size_t)1u);
    check_equal(probe.parts[0].data_size, sizeof(expected) - 1u);
    check_equal(
        probe.parts[0].data, expected, sizeof(expected) - 1u);
  }

  it("rejects invalid outer boundary, malformed headers, and truncated bodies") {
    chttp_web_multipart_parser parser = CHTTP_WEB_MULTIPART_PARSER_INIT;
    chttp_web_multipart_limits limits = mp_test_limits();
    chttp_web_multipart_callbacks callbacks = mp_test_callbacks();
    mp_test_probe probe = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;
    static const char bare_lf[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"x\"\n\n"
        "v\r\n--AaB03x--\r\n";
    static const char missing_disposition[] =
        "--AaB03x\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "v\r\n--AaB03x--\r\n";
    static const char truncated[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "v";

    check_equal(
        chttp_web_multipart_init(
            &parser,
            "multipart/form-data",
            &limits, &callbacks, &probe, &error),
        CHTTP_WEB_INVALID_ARGUMENT);
    check_equal(
        chttp_web_multipart_init(
            &parser,
            "multipart/form-data; boundary=\"bad \"",
            &limits, &callbacks, &probe, &error),
        CHTTP_WEB_INVALID_ARGUMENT);

    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            bare_lf, sizeof(bare_lf) - 1u, 1u, &error),
        CHTTP_WEB_MULTIPART);
    check_equal(probe.end_count, (size_t)0u);

    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            missing_disposition, sizeof(missing_disposition) - 1u,
            sizeof(missing_disposition) - 1u, &error),
        CHTTP_WEB_MULTIPART);
    check_equal(probe.begin_count, (size_t)0u);

    memset(&probe, 0, sizeof(probe));
    check_equal(mp_test_init(&parser, &probe, &limits, &error), CHTTP_WEB_OK);
    check_equal(
        chttp_web_multipart_feed(
            &parser, truncated, sizeof(truncated) - 1u, &error),
        CHTTP_WEB_OK);
    check_equal(
        chttp_web_multipart_finish(&parser, &error),
        CHTTP_WEB_MULTIPART);
    check_equal(probe.begin_count, (size_t)1u);
    check_equal(probe.end_count, (size_t)0u);
  }

  it("rejects malformed disposition and does not publish a terminal part end") {
    static const char empty_name[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"\"\r\n\r\n"
        "x\r\n--AaB03x--\r\n";
    static const char bad_suffix[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "ok\r\n--AaB03xX";
    mp_test_probe probe = {0};
    chttp_web_multipart_limits limits = mp_test_limits();
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    check_equal(
        mp_test_run(
            &probe, &limits,
            empty_name, sizeof(empty_name) - 1u, 3u, &error),
        CHTTP_WEB_MULTIPART);
    check_equal(probe.begin_count, (size_t)0u);

    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            bad_suffix, sizeof(bad_suffix) - 1u, 1u, &error),
        CHTTP_WEB_MULTIPART);
    check_equal(probe.begin_count, (size_t)1u);
    check_equal(probe.end_count, (size_t)0u);
  }

  it("enforces exact and one-over header-count and header-byte limits") {
    static const char one_header[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n"
        "\r\n"
        "v\r\n--AaB03x--\r\n";
    static const char two_headers[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "v\r\n--AaB03x--\r\n";
    static const char header_block[] =
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n";
    chttp_web_multipart_limits limits = mp_test_limits();
    mp_test_probe probe = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    limits.max_header_count = 1u;
    check_equal(
        mp_test_run(
            &probe, &limits,
            one_header, sizeof(one_header) - 1u, 5u, &error),
        CHTTP_WEB_OK);

    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            two_headers, sizeof(two_headers) - 1u, 5u, &error),
        CHTTP_WEB_CAPACITY);

    limits = mp_test_limits();
    limits.max_header_bytes = sizeof(header_block) - 1u;
    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            one_header, sizeof(one_header) - 1u, 7u, &error),
        CHTTP_WEB_OK);

    --limits.max_header_bytes;
    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            one_header, sizeof(one_header) - 1u, 7u, &error),
        CHTTP_WEB_CAPACITY);
  }

  it("enforces exact and one-over name filename content-type and field limits") {
    static const char name_exact[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"abcd\"\r\n\r\n"
        "1234\r\n--AaB03x--\r\n";
    static const char name_over[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"abcde\"\r\n\r\n"
        "1234\r\n--AaB03x--\r\n";
    static const char filename_exact[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"f\"; filename=\"abc\"\r\n\r\n"
        "data\r\n--AaB03x--\r\n";
    static const char filename_over[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"f\"; filename=\"abcd\"\r\n\r\n"
        "data\r\n--AaB03x--\r\n";
    static const char type_exact[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"f\"\r\n"
        "Content-Type: abc\r\n\r\n"
        "x\r\n--AaB03x--\r\n";
    static const char type_over[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"f\"\r\n"
        "Content-Type: abcd\r\n\r\n"
        "x\r\n--AaB03x--\r\n";
    chttp_web_multipart_limits limits = mp_test_limits();
    mp_test_probe probe = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    limits.max_name_bytes = 4u;
    limits.max_field_bytes = 4u;
    check_equal(
        mp_test_run(
            &probe, &limits,
            name_exact, sizeof(name_exact) - 1u, 3u, &error),
        CHTTP_WEB_OK);

    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            name_over, sizeof(name_over) - 1u, 3u, &error),
        CHTTP_WEB_CAPACITY);

    limits = mp_test_limits();
    limits.max_filename_bytes = 3u;
    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            filename_exact, sizeof(filename_exact) - 1u, 4u, &error),
        CHTTP_WEB_OK);
    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            filename_over, sizeof(filename_over) - 1u, 4u, &error),
        CHTTP_WEB_CAPACITY);

    limits = mp_test_limits();
    limits.max_content_type_bytes = 3u;
    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            type_exact, sizeof(type_exact) - 1u, 6u, &error),
        CHTTP_WEB_OK);
    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            type_over, sizeof(type_over) - 1u, 6u, &error),
        CHTTP_WEB_CAPACITY);

    limits = mp_test_limits();
    limits.max_field_bytes = 4u;
    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            name_exact, sizeof(name_exact) - 1u, 1u, &error),
        CHTTP_WEB_OK);

    --limits.max_field_bytes;
    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            name_exact, sizeof(name_exact) - 1u, 1u, &error),
        CHTTP_WEB_CAPACITY);
  }

  it("enforces part count and whole-body total bytes before partial chunk consumption") {
    static const char two_parts[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"a\"\r\n\r\n"
        "1\r\n"
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"b\"\r\n\r\n"
        "2\r\n"
        "--AaB03x--\r\n";
    chttp_web_multipart_limits limits = mp_test_limits();
    mp_test_probe probe = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    limits.max_parts = 2u;
    limits.max_field_bytes = 1u;
    limits.max_total_bytes = sizeof(two_parts) - 1u;
    check_equal(
        mp_test_run(
            &probe, &limits,
            two_parts, sizeof(two_parts) - 1u,
            sizeof(two_parts) - 1u, &error),
        CHTTP_WEB_OK);
    check_equal(probe.begin_count, (size_t)2u);

    limits.max_parts = 1u;
    limits.max_total_bytes = 4096u;
    memset(&probe, 0, sizeof(probe));
    check_equal(
        mp_test_run(
            &probe, &limits,
            two_parts, sizeof(two_parts) - 1u, 8u, &error),
        CHTTP_WEB_CAPACITY);
    check_equal(probe.begin_count, (size_t)1u);
    check_equal(probe.end_count, (size_t)1u);

    limits = mp_test_limits();
    limits.max_field_bytes = 1u;
    limits.max_total_bytes = sizeof(two_parts) - 2u;
    memset(&probe, 0, sizeof(probe));
    {
      chttp_web_multipart_parser parser = CHTTP_WEB_MULTIPART_PARSER_INIT;
      check_equal(mp_test_init(&parser, &probe, &limits, &error), CHTTP_WEB_OK);
      check_equal(
          chttp_web_multipart_feed(
              &parser, two_parts, sizeof(two_parts) - 1u, &error),
          CHTTP_WEB_CAPACITY);
      check_equal(probe.begin_count, (size_t)0u);
      check_equal(parser.total_bytes, (size_t)0u);
    }
  }

  it("resets and reuses the same parser without state leakage") {
    static const char body[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "one\r\n--AaB03x--\r\n";
    chttp_web_multipart_parser parser = CHTTP_WEB_MULTIPART_PARSER_INIT;
    chttp_web_multipart_limits limits = mp_test_limits();
    mp_test_probe probe = {0};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    check_equal(mp_test_init(&parser, &probe, &limits, &error), CHTTP_WEB_OK);
    check_equal(
        mp_test_feed_chunks(
            &parser, body, sizeof(body) - 1u, 2u, &error),
        CHTTP_WEB_OK);
    check_equal(chttp_web_multipart_finish(&parser, &error), CHTTP_WEB_OK);

    check_equal(chttp_web_multipart_reset(&parser, &error), CHTTP_WEB_OK);
    check_equal(parser.part_count, (size_t)0u);
    check_equal(parser.total_bytes, (size_t)0u);
    check_equal(
        mp_test_feed_chunks(
            &parser, body, sizeof(body) - 1u, 5u, &error),
        CHTTP_WEB_OK);
    check_equal(chttp_web_multipart_finish(&parser, &error), CHTTP_WEB_OK);

    check_equal(probe.begin_count, (size_t)2u);
    check_equal(probe.end_count, (size_t)2u);
  }

  it("propagates callback failures through native status") {
    static const char body[] =
        "--AaB03x\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n\r\n"
        "value\r\n--AaB03x--\r\n";
    chttp_web_multipart_parser parser = CHTTP_WEB_MULTIPART_PARSER_INIT;
    chttp_web_multipart_limits limits = mp_test_limits();
    mp_test_probe probe = {.fail_data = 1};
    chttp_web_error error = CHTTP_WEB_ERROR_INIT;

    check_equal(mp_test_init(&parser, &probe, &limits, &error), CHTTP_WEB_OK);
    check_equal(
        chttp_web_multipart_feed(
            &parser, body, sizeof(body) - 1u, &error),
        CHTTP_WEB_MULTIPART);
    check_equal(error.native_status, SALTS_EIO);
    check_equal(probe.begin_count, (size_t)1u);
    check_equal(probe.end_count, (size_t)0u);
  }
}
