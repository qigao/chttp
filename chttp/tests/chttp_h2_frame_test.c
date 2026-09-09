#include "chttp_h2_frame.h"
#include "tinytest.h"

#include <string.h>

spec("CHTTP HTTP/2 frame layer") {
  it("round-trips a frame header within the configured frame limit") {
    unsigned char bytes[CHTTP_H2_FRAME_HEADER_SIZE];
    chttp_h2_frame_header header;
    size_t encoded_size = 0u;
    size_t consumed_size = 0u;

    check_equal(chttp_h2_frame_header_encode(bytes, sizeof(bytes), &encoded_size, 1234u,
                                             CHTTP_H2_FRAME_HEADERS, 0x05u, 3u),
                0);
    check_equal(encoded_size, (size_t)CHTTP_H2_FRAME_HEADER_SIZE);
    check_equal(chttp_h2_frame_header_decode(bytes, sizeof(bytes), &consumed_size, &header, 16384u),
                0);
    check_equal(consumed_size, (size_t)CHTTP_H2_FRAME_HEADER_SIZE);
    check_equal(header.length, (uint32_t)1234u);
    check_equal(header.type, (uint8_t)CHTTP_H2_FRAME_HEADERS);
    check_equal(header.flags, (uint8_t)0x05u);
    check_equal(header.stream_id, (uint32_t)3u);
  }

  it("rejects reserved stream identifiers and oversized payloads") {
    unsigned char bytes[CHTTP_H2_FRAME_HEADER_SIZE] = {0};
    chttp_h2_frame_header header;
    size_t size = 0u;

    check_not_equal(chttp_h2_frame_header_encode(bytes, sizeof(bytes), &size, 0u,
                                                 CHTTP_H2_FRAME_HEADERS, 0u, 0x80000000u),
                    0);

    bytes[0] = 0x01u;
    bytes[1] = 0x00u;
    bytes[2] = 0x00u;
    check_not_equal(chttp_h2_frame_header_decode(bytes, sizeof(bytes), &size, &header, 16384u), 0);
  }

  it("extracts DATA payload and rejects malformed padding") {
    const unsigned char padded[] = {2u, 'a', 'b', 'c', 0u, 0u};
    const unsigned char malformed[] = {4u, 'a'};
    const unsigned char *data = NULL;
    size_t data_size = 0u;

    check_equal(chttp_h2_frame_data_payload(padded, sizeof(padded), CHTTP_H2_FLAG_PADDED, &data,
                                            &data_size),
                0);
    check_equal(data_size, (size_t)3u);
    check_equal(memcmp(data, "abc", 3u), 0);
    check_not_equal(chttp_h2_frame_data_payload(malformed, sizeof(malformed), CHTTP_H2_FLAG_PADDED,
                                                &data, &data_size),
                    0);
  }

  it("extracts HEADERS payload with priority") {
    const unsigned char payload[] = {1u, 0u, 0u, 0u, 5u, 9u, 'a', 'b', 'c', 0u};
    const unsigned char *block = NULL;
    size_t block_size = 0u;
    int has_priority = 0;
    uint32_t dependency = 0u;
    uint32_t weight = 0u;

    check_equal(chttp_h2_frame_headers_payload(
                    payload, sizeof(payload), CHTTP_H2_FLAG_PADDED | CHTTP_H2_FLAG_PRIORITY, &block,
                    &block_size, &has_priority, &dependency, &weight),
                0);
    check_equal(has_priority, 1);
    check_equal(dependency, (uint32_t)5u);
    check_equal(weight, (uint32_t)10u);
    check_equal(block_size, (size_t)3u);
    check_equal(memcmp(block, "abc", 3u), 0);
  }

  it("serializes SETTINGS GOAWAY and WINDOW_UPDATE payloads") {
    unsigned char bytes[32];
    const uint32_t setting_ids[] = {3u, 4u};
    const uint32_t setting_values[] = {100u, 65535u};
    uint32_t parsed_ids[2] = {0u, 0u};
    uint32_t parsed_values[2] = {0u, 0u};
    uint32_t last_stream_id = 0u;
    uint32_t error_code = 0u;
    size_t size = 0u;
    size_t setting_count = 0u;

    check_equal(chttp_h2_frame_settings_encode(bytes, sizeof(bytes), &size, setting_ids,
                                               setting_values, 2u),
                0);
    check_equal(size, (size_t)12u);
    check_equal(
        chttp_h2_frame_settings_parse(bytes, size, parsed_ids, parsed_values, &setting_count, 2u),
        0);
    check_equal(setting_count, (size_t)2u);
    check_equal(parsed_ids[0], setting_ids[0]);
    check_equal(parsed_values[0], setting_values[0]);
    check_equal(parsed_ids[1], setting_ids[1]);
    check_equal(parsed_values[1], setting_values[1]);

    check_equal(chttp_h2_frame_goaway_encode(bytes, sizeof(bytes), &size, 7u, 0x1234u), 0);
    check_equal(chttp_h2_frame_goaway_parse(bytes, size, &last_stream_id, &error_code), 0);
    check_equal(last_stream_id, (uint32_t)7u);
    check_equal(error_code, (uint32_t)0x1234u);

    check_equal(chttp_h2_frame_window_update_encode(bytes, sizeof(bytes), &size, 65535u), 0);
    check_equal(size, (size_t)4u);
    check_equal(bytes[0], (unsigned char)0u);
    check_equal(bytes[1], (unsigned char)0u);
    check_equal(bytes[2], (unsigned char)0xffu);
    check_equal(bytes[3], (unsigned char)0xffu);
    check_not_equal(chttp_h2_frame_window_update_encode(bytes, sizeof(bytes), &size, 0u), 0);
  }
}
