#include "chttp_h2_hpack.h"
#include "tinytest.h"

#include <stdio.h>
#include <string.h>

typedef struct hpack_collector {
  char names[8][32];
  char values[8][128];
  size_t count;
} hpack_collector;

static size_t decode_hex(const char *hex, uint8_t *output, size_t capacity) {
  size_t size = 0u;
  while (hex[0] != '\0' && hex[1] != '\0' && size < capacity) {
    unsigned int value = 0u;
    if (sscanf(hex, "%2x", &value) != 1) break;
    output[size++] = (uint8_t)value;
    hex += 2;
    while (*hex == ' ' || *hex == '\n')
      ++hex;
  }
  return size;
}

static int collect_header(void *user, const char *name, size_t name_size, const char *value,
                          size_t value_size) {
  hpack_collector *collector = (hpack_collector *)user;
  if (collector->count >= 8u || name_size >= sizeof(collector->names[0]) ||
      value_size >= sizeof(collector->values[0]))
    return -1;
  memcpy(collector->names[collector->count], name, name_size);
  collector->names[collector->count][name_size] = '\0';
  memcpy(collector->values[collector->count], value, value_size);
  collector->values[collector->count][value_size] = '\0';
  ++collector->count;
  return 0;
}

static chttp_h2_hpack *create_hpack(void) {
  const chttp_h2_hpack_config config = {
      .max_dynamic_table_bytes = 4096u, .max_header_block_bytes = 4096u, .max_string_bytes = 1024u};
  return chttp_h2_hpack_create(&config);
}

spec("CHTTP HTTP/2 HPACK") {
  it("encodes RFC 7541 prefix integers") {
    chttp_h2_hpack_buffer buffer;

    check_equal(chttp_h2_hpack_buffer_init(&buffer, 16u, 32u), 0);
    check_equal(chttp_h2_hpack_integer_encode(&buffer, 10u, 5u), 0);
    check_equal(buffer.size, (size_t)1u);
    check_equal(buffer.data[0], (uint8_t)0x0au);

    buffer.size = 0u;
    check_equal(chttp_h2_hpack_integer_encode(&buffer, 1337u, 5u), 0);
    check_equal(buffer.size, (size_t)3u);
    check_equal(buffer.data[0], (uint8_t)0x1fu);
    check_equal(buffer.data[1], (uint8_t)0x9au);
    check_equal(buffer.data[2], (uint8_t)0x0au);
    chttp_h2_hpack_buffer_destroy(&buffer);
  }

  it("refuses buffer growth beyond its hard capacity") {
    chttp_h2_hpack_buffer buffer;
    size_t old_capacity;

    check_equal(chttp_h2_hpack_buffer_init(&buffer, 8u, 8u), 0);
    old_capacity = buffer.capacity;
    check_not_equal(chttp_h2_hpack_buffer_reserve(&buffer, 9u), 0);
    check_equal(buffer.size, (size_t)0u);
    check_equal(buffer.capacity, old_capacity);
    chttp_h2_hpack_buffer_destroy(&buffer);
  }

  it("emits a dynamic table size update when the peer limit becomes zero") {
    chttp_h2_hpack *hpack = create_hpack();
    chttp_h2_hpack_buffer buffer;

    check_not_null(hpack);
    check_equal(chttp_h2_hpack_buffer_init(&buffer, 8u, 8u), 0);
    check_equal(chttp_h2_hpack_encoder_set_max_size(hpack, 0u), 0);
    check_equal(chttp_h2_hpack_encode(hpack, &buffer, NULL, 0u), 0);
    check_equal(buffer.size, (size_t)1u);
    check_equal(buffer.data[0], (uint8_t)0x20u);

    chttp_h2_hpack_buffer_destroy(&buffer);
    chttp_h2_hpack_destroy(hpack);
  }

  it("emits the smallest and final table limits after multiple peer changes") {
    chttp_h2_hpack *hpack = create_hpack();
    chttp_h2_hpack_buffer buffer;

    check_not_null(hpack);
    check_equal(chttp_h2_hpack_buffer_init(&buffer, 8u, 8u), 0);
    check_equal(chttp_h2_hpack_encoder_set_max_size(hpack, 0u), 0);
    check_equal(chttp_h2_hpack_encoder_set_max_size(hpack, 1024u), 0);
    check_equal(chttp_h2_hpack_encode(hpack, &buffer, NULL, 0u), 0);
    check_equal(buffer.size, (size_t)4u);
    check_equal(buffer.data[0], (uint8_t)0x20u);
    check_equal(buffer.data[1], (uint8_t)0x3fu);
    check_equal(buffer.data[2], (uint8_t)0xe1u);
    check_equal(buffer.data[3], (uint8_t)0x07u);

    chttp_h2_hpack_buffer_destroy(&buffer);
    chttp_h2_hpack_destroy(hpack);
  }

  it("decodes the RFC 7541 C.4.1 request vector") {
    static const char vector[] = "8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff";
    uint8_t bytes[64];
    const size_t byte_count = decode_hex(vector, bytes, sizeof(bytes));
    chttp_h2_hpack *hpack = create_hpack();
    hpack_collector collector = {0};
    size_t consumed = 0u;

    check_not_null(hpack);
    check_equal(chttp_h2_hpack_decode(hpack, bytes, byte_count, &consumed, collect_header,
                                      &collector, 1024u),
                0);
    check_equal(consumed, byte_count);
    check_equal(collector.count, (size_t)4u);
    check_equal(collector.names[0], ":method");
    check_equal(collector.values[0], "GET");
    check_equal(collector.names[3], ":authority");
    check_equal(collector.values[3], "www.example.com");
    chttp_h2_hpack_destroy(hpack);
  }

  it("preserves decoder state after rejecting an oversized header list") {
    static const uint8_t indexed_authority[] = {0x41u, 0x0bu, 'e', 'x', 'a', 'm', 'p',
                                                'l',   'e',   '.', 'c', 'o', 'm'};
    static const uint8_t dynamic_authority[] = {0xbeu};
    chttp_h2_hpack *hpack = create_hpack();
    hpack_collector collector = {0};
    size_t consumed = 0u;

    check_not_null(hpack);
    check_equal(chttp_h2_hpack_decode(hpack, indexed_authority, sizeof(indexed_authority),
                                      &consumed, collect_header, &collector, 16u),
                -2);
    check_equal(consumed, sizeof(indexed_authority));
    check_equal(collector.count, (size_t)0u);
    check_equal(chttp_h2_hpack_decode(hpack, dynamic_authority, sizeof(dynamic_authority),
                                      &consumed, collect_header, &collector, 1024u),
                0);
    check_equal(collector.count, (size_t)1u);
    check_equal(collector.names[0], ":authority");
    check_equal(collector.values[0], "example.com");
    chttp_h2_hpack_destroy(hpack);
  }

  it("rejects invalid Huffman padding") {
    static const uint8_t invalid[] = {0x01u, 0x81u, 0x00u};
    chttp_h2_hpack *hpack = create_hpack();
    hpack_collector collector = {0};
    size_t consumed = 0u;

    check_not_null(hpack);
    check_not_equal(chttp_h2_hpack_decode(hpack, invalid, sizeof(invalid), &consumed,
                                          collect_header, &collector, 1024u),
                    0);
    chttp_h2_hpack_destroy(hpack);
  }
}
